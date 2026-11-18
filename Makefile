#---------------------------------------------------------------------------------
# backstab-nx – 32 bites (AArch32) Switch-loader. Kimenet: build/backstab.nsp (+ build/exefs/main, main.npdm)
# Előfeltétel: toolchain/build-toolchain.sh + toolchain/build-portlibs.sh, DEVKITPRO=~/devkitpro32
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO=~/devkitpro32 kell (a 32 bites eszközlánc, lásd toolchain/)")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx32/switch32_rules

TARGET   := backstab
BUILD    := build
SOURCES  := source source/reimpl lib/FalsoJNI
INCLUDES := source lib
APP_JSON := backstab.json
PORTLIBS32 := $(DEVKITPRO)/portlibs/switch32

# az NSP-be (exefs) a cím és az ikon is bekerül (control.nacp, icon_AmericanEnglish.dat), így az Eden
# játéklistája „Backstab HD”-ként, ikonnal mutatja; az ikon a saját APK-ból: tools/assets/make-icon.py → icon.jpg
APP_TITLE   := Backstab HD
APP_AUTHOR  := Gameloft (Switch port: Levi)
APP_VERSION := 1.2.8d
# 0x nélkül: a nacptool legfeljebb 16 karaktert olvas, a „0x0100…” végéről két nullát levágna (az NACP mentés-
# és jelenlét-azonosítói így 0x000100BAC57AB000 lettek)
NACPFLAGS   := --titleid=0100BAC57AB00000

# softfp: az Android armeabi-v7a hívási konvenciója; -fno-short-enums: 4 bájtos enumok, mint az
# Androidon és a (szintén így fordított) libnx32-ben/Mesában
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -mfloat-abi=softfp \
            -mfpu=neon-fp-armv8 -fno-short-enums

DEFINES  := -D__SWITCH__ -D_GNU_SOURCE -DFALSOJNI_DEBUGLEVEL=FALSOJNI_DEBUG_ERROR
CFLAGS   := -g -Wall -Wno-unknown-pragmas -O2 -ffunction-sections -fno-strict-aliasing $(ARCH) $(DEFINES) $(INCLUDE)
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
# a newlib malloc-családja a source/heap.c-n át (nagy foglalások külön régióban, a feldarabolódás ellen)
HEAPWRAP := $(foreach f,_malloc_r _free_r _realloc_r _calloc_r _memalign_r _malloc_usable_size_r,-Wl,--wrap=$(f))
LDFLAGS  := -specs=$(DEVKITPRO)/libnx32/switch32.specs -g $(ARCH) -Wl,--no-enum-size-warning $(HEAPWRAP) \
            -Wl,-Map,$(notdir $*.map)

# Mesa (EGL + GLES2, nouveau) a portlibs/switch32-ből; a sorrend számít
LIBS     := -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lstdc++ -lm -lnx
LIBDIRS  := $(PORTLIBS32) $(LIBNX)

# Eredeti betűkészlet (tools/assets/make-fonts.py): ha megvan a Gameloft 1280×800-as csomagja (lásd README) és a
# Pillow-s Python, a make legyártja a build/hdfonts/-ba és a binárisba ágyazza; nélküle az adatcsomag fontja marad
FONT_PACK ?= $(HOME)/Library/Caches/backstab-nx-toolchain/gameloft-data/sprites_1280x800.gla
FONT_PY   ?= $(HOME)/Library/Caches/backstab-nx-toolchain/venv-tools/bin/python

ifneq ($(BUILD),$(notdir $(CURDIR)))
# minden kimenet a build/-be kerül (a gyökérben csak a forrás marad)
export OUTPUT  := $(CURDIR)/$(BUILD)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o) $(SFILES:.S=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_JSON := $(TOPDIR)/$(APP_JSON)
export APP_ICON := $(firstword $(wildcard $(CURDIR)/icon.jpg) $(LIBNX)/default_icon.jpg)
.PHONY: $(BUILD) clean all
# A dist/ mappát (ezt látja az Eden játéklistája) a tools/package/pack-nsp.sh tölti: NCA-s, telepíthető NSP;
# a clean nem törli.
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@# debug ↔ release váltáskor (más DEFINES) a régi objektumok nem keveredhetnek
	@echo '$(DEFINES)' | cmp -s - $@/.defines || { rm -f $@/*.o $@/*.d; echo '$(DEFINES)' > $@/.defines; }
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD)
else
BUILD_EXEFS_SRC := exefs_extra
all: $(OUTPUT).nsp
$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm exefs_extra/control.nacp exefs_extra/icon_AmericanEnglish.dat
exefs_extra/control.nacp: $(OUTPUT).nacp
	@mkdir -p exefs_extra && cp $< $@
exefs_extra/icon_AmericanEnglish.dat: $(APP_ICON)
	@mkdir -p exefs_extra && cp $< $@
$(OUTPUT).nso: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
# a data/ fájljai és az eredeti betűkészlet .incbin-nel kerülnek a binárisba
data_install.o: $(wildcard $(TOPDIR)/data/*)
ifneq ($(and $(wildcard $(FONT_PACK)),$(wildcard $(FONT_PY))),)
data_install.o: CFLAGS += -DHD_FONTS
data_install.o: hdfonts/.kesz
hdfonts/.kesz: $(FONT_PACK) $(TOPDIR)/tools/assets/make-fonts.py $(TOPDIR)/tools/assets/gla.py
	@echo "betűkészlet: $(notdir $(FONT_PACK)) → hdfonts/"
	@$(FONT_PY) $(TOPDIR)/tools/assets/make-fonts.py $(FONT_PACK) hdfonts >/dev/null && touch $@
else
$(info figyelem: nincs $(FONT_PACK) vagy $(FONT_PY) – az adatcsomag betűkészlete marad)
endif
-include $(DEPSDIR)/*.d
endif
