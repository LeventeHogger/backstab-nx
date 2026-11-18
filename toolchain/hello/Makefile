.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO=~/devkitpro32 kell")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx32/switch32_rules

TARGET   := hello
BUILD    := build
SOURCES  := source
APP_JSON := ../../backstab.json

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -mfloat-abi=softfp -mfpu=neon-fp-armv8 -fno-short-enums
CFLAGS   := -g -Wall -O2 -fno-tree-vectorize -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__
LDFLAGS  := -Wl,--no-enum-size-warning -specs=$(DEVKITPRO)/libnx32/switch32.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lnx
LIBDIRS  := $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_JSON := $(TOPDIR)/$(APP_JSON)
.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
else
all: $(OUTPUT).nsp
$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPSDIR)/*.d
endif
