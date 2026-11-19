# Backstab HD for Nintendo Switch

![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch%20(AArch32)-e60012)
![Állapot](https://img.shields.io/badge/%C3%A1llapot-k%C3%ADs%C3%A9rleti-orange)
![Licenc](https://img.shields.io/badge/licenc-MIT-blue)

Nem hivatalos Nintendo Switch-port a **Backstab HD** (Gameloft, 2011) Android-kiadásához. A loader a játék eredeti
ARMv7 kódját (`libPirates.so`) natívan futtatja egy 32 bites Horizon-folyamatban, és Android-szerű környezetet ad
alá: libc-hidat, JNI-t, OpenGL ES 2-t (Mesa), hangot és teljes kontrolleres irányítást. A
[backstab-vita](https://github.com/v-atamanenko/backstab-vita) PS Vita-portra épül.

**English:** [see below](#english).

> [!IMPORTANT]
> A repó **nem tartalmaz semmit a játékból** – se kódot, se grafikát, se hangot. A futtatáshoz a saját, legálisan
> beszerzett Android-példányod kell (v1.2.8d). A lefordított NSP-t és a csomagkészítő kimenetét ne terjeszd: a
> játékfájlokon túl a binárisba ágyazott eredeti betűkészlet is a játékból származik.

## Tartalom
- [Funkciók](#funkciók)
- [Állapot](#állapot)
- [Követelmények](#követelmények)
- [Telepítés](#telepítés)
- [Irányítás](#irányítás)
- [Beállítások](#beállítások)
- [Fordítás forrásból](#fordítás-forrásból)
- [A projekt felépítése](#a-projekt-felépítése)
- [Fejlesztés és hibakeresés](#fejlesztés-és-hibakeresés)
- [Köszönet](#köszönet)
- [Licenc és jogi nyilatkozat](#licenc-és-jogi-nyilatkozat)
- [English](#english)

## Funkciók
- **Az eredeti játék, módosítatlan kóddal:** a loader csak a platformot pótolja; a játékmenet, a menük és a mentés a
  játék saját kódja.
- **Teljes kontrolleres irányítás** a játék saját HUD-objektumaira építve, menükben fókusznavigációval; kézi módban az
  érintőképernyő is működik.
- **Switch-gombjelek** a súgókban és a HUD-on (a PlayStation-stílusú Xperia Play-jelek helyett).
- **1920×1080, 30 FPS**, docked és kézi módban; a grafikai szintet a játék maga választja, mint egy erős
  Android-eszközön.
- **Eredeti betűkészlet és shaderek** a Gameloft eredeti csomagjából (ha megvannak a fordító gépen).
- **Bevezető videó**, nyelvválasztás (a játék 9 nyelve vagy a Switch rendszernyelve), mentés az SD-kártyára.

## Állapot

| Környezet | |
|---|---|
| Eden emulátor (macOS) | ✅ játszható: mind a 23 helyszín és az összes történetváltozat betölt, stabil memóriával |
| Valódi Switch | ⚠️ még nincs kipróbálva (a 32 bites Mesa a valódi GPU-n az első éles próba) |

Ismert korlátok:
- A játékhoz a `sprites_1920x1080.gla` sprite-csomag kell; ez a Gameloft eredeti (Tegra/DXT) csomagjában nincs meg.
- 60 FPS-sel nincs kipróbálva; a LIVE és a „More games!” gomb offline nem csinál semmit.

## Követelmények

**Konzol:** Atmosphère custom firmware sigpatchekkel, NSP-telepítő (pl. DBI vagy Tinfoil), kb. 1,5 GB szabad hely
az SD-kártyán. A 32 bites folyamat a Homebrew menüből (.nro) nem indul, ezért telepíthető NSP-ként fut.

**Játékfájlok** (a saját példányodból):

| Fájl | Honnan | Szükséges? |
|---|---|---|
| `libPirates.so` | az APK-ból: `lib/armeabi-v7a/libPirates.so` | igen; v1.2.8d, SHA1 `9a36ce92ac09f2908c155ab1debb3e138a3496b8` |
| adatmappa | `Android/data/com.gameloft.android.ANMP.GloftSDHM/files/` | igen; benne a `sprites_1920x1080.gla` |
| `effects.gla` | a Gameloft eredeti csomagjából | ajánlott: az elterjedt 1080p-s adatcsomag 21 shadere módosított ([részletek](docs/visszafejtes.md#grafika)) |
| `sprites_1280x800.gla` | a Gameloft eredeti csomagjából | fordításkor: ebből készül az eredeti betűkészlet |
| `intro.mpg` | a játék `intro.mp4`-éből: `tools/assets/make-intro.sh` (ffmpeg) | nem; nélküle videó nélkül indul |

## Telepítés

1. **Fájlok az SD-kártyára:**
   ```
   sdmc:/switch/backstab/
   ├── libPirates.so
   └── com.gameloft.android.ANMP.GloftSDHM/
       └── files/          ← a játék adatmappája (+ effects.gla, intro.mpg)
   ```
2. **Az NSP telepítése** (lásd [Telepíthető NSP](#telepíthető-nsp)) DBI-jal vagy Tinfoillal az SD-kártyára.
3. **Indítás** a főképernyőről („Backstab HD”). Az első induláskor létrejön a `switch/backstab/config.txt` és a
   `log.txt`.

A mentések a játék adatmappájában vannak (`save*.dat`, `globalsave*.dat`, `settings.dat`): újratelepítéskor
megmaradnak, és fájlként menthetők; indításkor nincs felhasználóválasztás.

Kész csomag a saját fájljaidból (SD-mappa + NSP + lépésenkénti útmutató): `tools/package/make-package.sh` →
`dist/BackstabHD-Switch/`. Az útmutató: [`tools/package/OLVASS_EL.txt`](tools/package/OLVASS_EL.txt).

## Irányítás

| Switch | Játékmenet | Menüben |
|---|---|---|
| bal kar / D-pad | mozgás (a kar 85%-áig séta, afölött futás; a D-pad fut) | lépés a gombok között |
| jobb kar | kamera | – |
| A | kontra, különleges támadás (helyzettől függően interakció, bosszú); átvezetőben: Skip | a kijelölt gomb (a mentésválasztón rögtön indít) |
| B | ugrás; átvezetőben: a párbeszéd következő sora | vissza (a szünetmenü alfülein egy szinttel) |
| Y | kard; célzás közben következő fegyver/gránát | – |
| X | interakció; nyomva tartva ló hívása; célzás közben következő fegyver/gránát | – |
| ZL / L | célzás: koppintásra be/ki, nyomva tartva a felengedés elteszi a pisztolyt | L: előző fül |
| ZR / R | lövés, gránátdobás (célzás közben); különleges pisztolyos támadás | R: következő fül |
| ZR + Y | bosszú-kombó | – |
| − | térkép | – |
| + | szünetmenü | – |
| R3 | szabad kurzor be/ki | szabad kurzor be/ki (ahol nincs kijelölhető gomb, magától jön) |

A menükben és az átvezetőkben a gombok felirata mellett a játék saját stílusában látszik a kontrollergomb
(B a vissza-gombon és a párbeszéd-dobozon, A a kijelölt gombon, az indításon és a Skipen, L/R a szünetmenü
fülsorának két szélső füle alatt). A tippek és a súgó a játék kontrolleres szövegeit mutatják Switch-jelekkel:
A/B/X/Y, ZL/ZR (célzás, lövés), L- és R-kar, D-pad, +/−.

## Beállítások

A `sdmc:/switch/backstab/config.txt` az első induláskor jön létre; a tört értékek százszorosan szerepelnek benne.

| Kulcs | Érték | Alap |
|---|---|---|
| `fpsLock` | 30 vagy 60; 0 = nincs korlát | 30 |
| `gfxDetail`, `geometryDetail` | -1 = a játék saját választása, 0 alacsony, 1 közepes, 2 magas | -1 (ezen a gépen: magas) |
| `viewDistance` | látótáv-szorzó, 40..300; 0 = a játék saját értéke | 0 (1,1) |
| `leftStickDeadZone`, `rightStickDeadZone` | holtjáték, 1..50 | 11 |
| `cameraSensitivity` | a kamera gyorsasága %-ban, 10..300 | 100 |
| `language` | 0 en, 1 fr, 2 de, 3 es, 4 it, 5 pt, 6 ja, 7 ko, 8 zh; -1 = a Switch nyelve | 0 |

## Fordítás forrásból

A játék csak 32 bites ARM-ra létezik, ezért a port a [vita2hos](https://github.com/vita2hos) projekt 32 bites
eszközláncával készül (devkitARM-, libnx- és switch-tools-forkok); a hivatalos 64 bites devkitPro-val nem keveredik.
A szkriptek macOS-re készültek; kell hozzájuk az Xcode Command Line Tools és a Homebrew (`bison`, `flex`, `texinfo`,
`make`, `gpatch`, `python@3.11`, `meson`, `ninja`).

```sh
toolchain/build-toolchain.sh      # → ~/devkitpro32: devkitARM, switch-tools, libnx32 (egyszer)
toolchain/build-portlibs.sh       # Mesa (EGL + GLES2) és libdrm_nouveau 32 bitre
export DEVKITPRO=~/devkitpro32 DEVKITARM=~/devkitpro32/devkitARM
export PATH=$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH
make                              # → build/backstab.nsp
```

**Eredeti betűkészlet.** Az 1080p-s sprite-csomag fontjai nem a Gameloft eredetijei (hiányzó ékezetesek, más
méretarányok). Ha a fordító gépen megvan a Gameloft 1280×800-as csomagja, a `make` 1080p-re nagyítja és a
binárisba ágyazza: `make FONT_PACK=<sprites_1280x800.gla> FONT_PY=<Pillow-s python>`. Nélküle a csomag saját fontja
marad.

### Telepíthető NSP
`tools/package/pack-nsp.sh` → `dist/BackstabHD.nsp` (program + control + meta NCA, „Backstab HD” címmel, mind a 16
rendszernyelvre ugyanazzal az ikonnal). Kell hozzá a [hacBrewPack](https://github.com/rlaphoenix/hacBrewPack) és a
saját konzolod `prod.keys`-e; az ikon a saját APK-dból: `tools/assets/make-icon.py <apk>` → `icon.jpg`. A `make`
kimenete (`build/backstab.nsp`) csak exefs-csomag: emulátorban indítható, telepíteni nem lehet.

## A projekt felépítése

```
backstab-nx/
├── source/          a loader: ELF-betöltő, libc-híd (reimpl/), JNI, GL, hang, irányítás, a játék foltjai
├── lib/             külső könyvtárak: FalsoJNI, pl_mpeg
├── data/            a binárisba ágyazott fájlok (a Vita-port kontrollersémája)
├── docs/            visszafejtett tudnivalók a játékról
├── toolchain/       a 32 bites eszközlánc és a Mesa fordítása; hello/: próbaprogram
├── tools/
│   ├── assets/      eszközgyártók: betűkészlet, gombjelek, ikon, bevezető videó, .gla-archívumok
│   ├── dev/         fejlesztés: Eden-indító, tesztbevitel, szálpillanatkép, képkocka-átalakító, importlefedettség
│   └── package/     telepíthető NSP és kész csomag (az útmutatóval)
├── backstab.json    NPDM: 32 bites folyamat, címazonosító, jogosultságok
└── Makefile         minden kimenet a build/-be; a telepíthető NSP és a csomag a dist/-be
```

## Fejlesztés és hibakeresés

- **Debug-build:** `make DEFINES="-D__SWITCH__ -D_GNU_SOURCE -DFALSOJNI_DEBUGLEVEL=FALSOJNI_DEBUG_WARN -DDEBUG"` –
  részletes napló (fájlok, HUD, menük, köd, shaderek) és tesztkapcsolók az SD-n: automatikus pályabejárás,
  időzített gombnyomások, képkocka-mentés.
- **Eden:** Eden v0.2.1, **CPU → multi-core ki** (multi-core módban a 32 bites JIT összeomlik); a kulcsok és a firmware a
  `~/.local/share/eden` alatt, az SD-elrendezés a `sdmc/` alá. A `dist/` mappát játékkönyvtárként hozzáadva a
  telepíthető NSP a főoldalon jelenik meg; fordítás és indítás egyben: `tools/dev/run-eden.sh`.
- **Napló:** `sdmc:/switch/backstab/log.txt`; valódi gépen összeomláskor az `atmosphere/crash_reports/` is.
- Fejlesztői jegyzetek (ami a kódból nem látszik, tesztreceptek): [`AGENTS.md`](AGENTS.md); a játék belső szerkezete:
  [`docs/visszafejtes.md`](docs/visszafejtes.md).

## Köszönet
- [backstab-vita](https://github.com/v-atamanenko/backstab-vita) és [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) –
  Volodymyr Atamanenko; a so-loader alapja Andy Nguyen (TheFloW) és Rinnegatamante munkája.
- [vita2hos](https://github.com/vita2hos) – a 32 bites Switch-eszközlánc.
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) – Dominic Szablewski, a bevezető videóhoz.
- [Mesa](https://mesa3d.org) és a devkitPro [libdrm_nouveau](https://github.com/devkitPro/libdrm_nouveau) portja.

## Licenc és jogi nyilatkozat
A forráskód MIT-licencű (lásd [`LICENSE`](LICENSE)); kivétel a `source/elf.h` és a `toolchain/compat/elf.h` (GNU C
Library, LGPL-2.1, lásd a fájlok fejlécét), valamint a `lib/` könyvtárai a saját licencükkel.

Ez a projekt nem hivatalos, és nem áll kapcsolatban a Gameloft-tal vagy a Nintendóval. A Backstab a Gameloft, a
Nintendo Switch a Nintendo védjegye. A projekt nem tartalmaz játékfájlokat, és nem segít azok jogosulatlan
beszerzésében.

---

## English

An unofficial Nintendo Switch port of the Android release of **Backstab HD** (Gameloft, 2011). The loader runs the
game's original ARMv7 code (`libPirates.so`) natively in a 32-bit Horizon process and provides an Android-like
environment underneath it: a libc bridge, JNI, OpenGL ES 2 (Mesa), audio and full controller support. It is based on
the [backstab-vita](https://github.com/v-atamanenko/backstab-vita) PS Vita port.

> [!IMPORTANT]
> This repository **contains nothing from the game** – no code, no graphics, no audio. You need your own, legally
> obtained copy of the Android release (v1.2.8d). Do not distribute the built NSP or the output of the package script:
> besides the game files, the original font embedded into the binary also comes from the game.

### Features
- **The original game with unmodified code:** the loader only replaces the platform; gameplay, menus and saving are
  the game's own code.
- **Full controller support** built on the game's own HUD objects, with focus navigation in menus; the touch screen
  also works in handheld mode.
- **Switch button prompts** in tutorials and on the HUD (instead of the PlayStation-style Xperia Play prompts).
- **1920×1080 at 30 FPS**, docked and handheld; the game picks its graphics level by itself, as on a high-end Android
  device.
- **Original font and shaders** from Gameloft's original data package (if available on the build machine).
- **Intro video**, language selection (the game's 9 languages or the Switch system language), saves on the SD card.

### Status

| Environment | |
|---|---|
| Eden emulator (macOS) | ✅ playable: all 23 locations and every story variant load, with stable memory use |
| Real Switch hardware | ⚠️ not tested yet (32-bit Mesa on the real GPU is the first live test) |

Known limitations:
- The game needs the `sprites_1920x1080.gla` sprite package, which is not part of Gameloft's original (Tegra/DXT)
  package.
- 60 FPS has not been tested; the LIVE and "More games!" buttons do nothing offline.

### Requirements

**Console:** Atmosphère custom firmware with sigpatches, an NSP installer (e.g. DBI or Tinfoil), about 1.5 GB of free
space on the SD card. A 32-bit process does not start from the Homebrew Menu (.nro), so the port runs as an
installable NSP.

**Game files** (from your own copy):

| File | Source | Required? |
|---|---|---|
| `libPirates.so` | from the APK: `lib/armeabi-v7a/libPirates.so` | yes; v1.2.8d, SHA1 `9a36ce92ac09f2908c155ab1debb3e138a3496b8` |
| data folder | `Android/data/com.gameloft.android.ANMP.GloftSDHM/files/` | yes; must contain `sprites_1920x1080.gla` |
| `effects.gla` | from Gameloft's original package | recommended: 21 shaders of the common 1080p data package are modified ([details, in Hungarian](docs/visszafejtes.md#grafika)) |
| `sprites_1280x800.gla` | from Gameloft's original package | at build time: the original font is generated from it |
| `intro.mpg` | from the game's `intro.mp4`: `tools/assets/make-intro.sh` (ffmpeg) | no; without it the game starts without the video |

### Installation

1. **Files on the SD card:**
   ```
   sdmc:/switch/backstab/
   ├── libPirates.so
   └── com.gameloft.android.ANMP.GloftSDHM/
       └── files/          ← the game's data folder (+ effects.gla, intro.mpg)
   ```
2. **Install the NSP** (see [Installable NSP](#installable-nsp)) to the SD card with DBI or Tinfoil.
3. **Launch** "Backstab HD" from the HOME menu. On first launch, `switch/backstab/config.txt` and `log.txt` are
   created.

Saves are stored in the game's data folder (`save*.dat`, `globalsave*.dat`, `settings.dat`): they survive
reinstalling and can be backed up as plain files; there is no user selection at launch.

A ready-made package from your own files (SD folder + NSP + step-by-step guide in Hungarian):
`tools/package/make-package.sh` → `dist/BackstabHD-Switch/`.

### Controls

| Switch | Gameplay | In menus |
|---|---|---|
| left stick / D-pad | move (walk up to 85% tilt, run above; the D-pad runs) | move between buttons |
| right stick | camera | – |
| A | counter, special attack (depending on context: interact, vengeance); in cutscenes: Skip | press the selected button (starts the game right away on the save slot screen) |
| B | jump; in cutscenes: next line of dialogue | back (one level up in the pause menu's sub-tabs) |
| Y | sword; while aiming: next weapon/grenade | – |
| X | interact; hold to call the horse; while aiming: next weapon/grenade | – |
| ZL / L | aim: tap to toggle, or hold – releasing holsters the pistol | L: previous tab |
| ZR / R | shoot, throw grenade (while aiming); pistol special attack | R: next tab |
| ZR + Y | vengeance combo | – |
| − | map | – |
| + | pause menu | – |
| R3 | free cursor on/off | free cursor on/off (appears automatically where there is no selectable button) |

In menus and cutscenes the controller button is shown next to the button labels, in the game's own style
(B on the back button and the dialogue box, A on the selected button, start and Skip, L/R under the outermost
tabs of the pause menu). Tips and help show the game's own controller texts with Switch glyphs: A/B/X/Y,
ZL/ZR (aim, shoot), left and right stick, D-pad, +/−.

### Settings

`sdmc:/switch/backstab/config.txt` is created on first launch; fractional values are stored multiplied by 100.

| Key | Value | Default |
|---|---|---|
| `fpsLock` | 30 or 60; 0 = no limit | 30 |
| `gfxDetail`, `geometryDetail` | -1 = the game's own choice, 0 low, 1 medium, 2 high | -1 (high on this device) |
| `viewDistance` | view distance multiplier, 40..300; 0 = the game's own value | 0 (1.1) |
| `leftStickDeadZone`, `rightStickDeadZone` | dead zone, 1..50 | 11 |
| `cameraSensitivity` | camera speed in %, 10..300 | 100 |
| `language` | 0 en, 1 fr, 2 de, 3 es, 4 it, 5 pt, 6 ja, 7 ko, 8 zh; -1 = Switch system language | 0 |

### Building from source

The game exists for 32-bit ARM only, so the port is built with the 32-bit toolchain of the
[vita2hos](https://github.com/vita2hos) project (forks of devkitARM, libnx and switch-tools); it does not mix with the
official 64-bit devkitPro. The scripts target macOS and need the Xcode Command Line Tools and Homebrew (`bison`,
`flex`, `texinfo`, `make`, `gpatch`, `python@3.11`, `meson`, `ninja`).

```sh
toolchain/build-toolchain.sh      # → ~/devkitpro32: devkitARM, switch-tools, libnx32 (once)
toolchain/build-portlibs.sh       # Mesa (EGL + GLES2) and libdrm_nouveau for 32-bit
export DEVKITPRO=~/devkitpro32 DEVKITARM=~/devkitpro32/devkitARM
export PATH=$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH
make                              # → build/backstab.nsp
```

**Original font.** The fonts in the 1080p sprite package are not Gameloft's originals (missing accented letters,
different proportions). If Gameloft's 1280×800 package is available on the build machine, `make` upscales its fonts to
1080p and embeds them into the binary: `make FONT_PACK=<sprites_1280x800.gla> FONT_PY=<python with Pillow>`. Without
it, the package's own font is used.

#### Installable NSP
`tools/package/pack-nsp.sh` → `dist/BackstabHD.nsp` (program + control + meta NCA, titled "Backstab HD", with the same
icon for all 16 system languages). It needs [hacBrewPack](https://github.com/rlaphoenix/hacBrewPack) and your own
console's `prod.keys`; the icon comes from your own APK: `tools/assets/make-icon.py <apk>` → `icon.jpg`. The output of
`make` (`build/backstab.nsp`) is an ExeFS-only package: it runs in emulators but cannot be installed.

### Project layout

```
backstab-nx/
├── source/          the loader: ELF loader, libc bridge (reimpl/), JNI, GL, audio, input, game patches
├── lib/             third-party libraries: FalsoJNI, pl_mpeg
├── data/            files embedded into the binary (the Vita port's controller scheme)
├── docs/            reverse-engineering notes about the game (Hungarian)
├── toolchain/       building the 32-bit toolchain and Mesa; hello/: test program
├── tools/
│   ├── assets/      asset generators: font, button prompts, icon, intro video, .gla archives
│   ├── dev/         development: Eden launcher, test input, thread snapshots, frame converter, import coverage
│   └── package/     installable NSP and ready-made package (with the guide)
├── backstab.json    NPDM: 32-bit process, title ID, permissions
└── Makefile         all build output goes to build/; the installable NSP and the package go to dist/
```

### Development and debugging

- **Debug build:** `make DEFINES="-D__SWITCH__ -D_GNU_SOURCE -DFALSOJNI_DEBUGLEVEL=FALSOJNI_DEBUG_WARN -DDEBUG"` –
  verbose logging (files, HUD, menus, fog, shaders) and test switches on the SD card: automatic level sweep, timed
  button presses, frame capture.
- **Eden:** Eden v0.2.1 with **CPU → multi-core off** (the 32-bit JIT crashes in multi-core mode); keys and firmware go
  under `~/.local/share/eden`, the SD layout under `sdmc/`. With the `dist/` folder added as a game directory, the
  installable NSP appears on the main screen; build and launch in one step: `tools/dev/run-eden.sh`.
- **Log:** `sdmc:/switch/backstab/log.txt`; on real hardware, crashes also leave reports in `atmosphere/crash_reports/`.
- Developer notes (what is not obvious from the code, test recipes): [`AGENTS.md`](AGENTS.md); the game's internals:
  [`docs/visszafejtes.md`](docs/visszafejtes.md) (both in Hungarian).

### Credits
- [backstab-vita](https://github.com/v-atamanenko/backstab-vita) and [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) –
  Volodymyr Atamanenko; the so-loader is based on the work of Andy Nguyen (TheFloW) and Rinnegatamante.
- [vita2hos](https://github.com/vita2hos) – the 32-bit Switch toolchain.
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) – Dominic Szablewski, used for the intro video.
- [Mesa](https://mesa3d.org) and devkitPro's [libdrm_nouveau](https://github.com/devkitPro/libdrm_nouveau) port.

### License and disclaimer
The source code is MIT-licensed (see [`LICENSE`](LICENSE)), except `source/elf.h` and `toolchain/compat/elf.h` (GNU C
Library, LGPL-2.1, see the file headers) and the libraries in `lib/`, which keep their own licenses.

This project is unofficial and not affiliated with Gameloft or Nintendo. Backstab is a trademark of Gameloft, Nintendo
Switch is a trademark of Nintendo. The project contains no game files and does not help to obtain them without
authorization.
