# Backstab HD for Nintendo Switch

![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch%20(AArch32)-e60012)
![Állapot](https://img.shields.io/badge/%C3%A1llapot-k%C3%ADs%C3%A9rleti-orange)
![Licenc](https://img.shields.io/badge/licenc-MIT-blue)

Nem hivatalos Nintendo Switch-port a **Backstab HD** (Gameloft, 2011) Android-kiadásához. A loader a játék eredeti
ARMv7 kódját (`libPirates.so`) natívan futtatja egy 32 bites Horizon-folyamatban, és Android-szerű környezetet ad
alá: libc-hidat, JNI-t, OpenGL ES 2-t (Mesa), hangot és teljes kontrolleres irányítást. A
[backstab-vita](https://github.com/v-atamanenko/backstab-vita) PS Vita-portra épül.

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
| bal kar / D-pad | mozgás (kis kitérés: séta, teljes: futás) | lépés a gombok között |
| jobb kar | kamera | – |
| A | kontra (helyzettől függően interakció, bosszú) | a kijelölt gomb |
| B | ugrás | vissza (a szünetmenü alfülein egy szinttel) |
| Y | kard | – |
| X | interakció; nyomva tartva ló hívása; célzás közben következő gránát | – |
| ZL / L | célzás | L: előző fül |
| ZR / R | lövés | R: következő fül |
| ZR + Y | bosszú-kombó | – |
| − | térkép | – |
| + | szünetmenü | – |
| R3 | szabad kurzor be/ki | szabad kurzor be/ki (ahol nincs kijelölhető gomb, magától jön) |

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

**Eredeti betűkészlet.** Az 1080p-s sprite-csomag fontjai nem a Gameloft eredetijei (cirill betűk, hiányzó
ékezetesek, más méretek). Ha a fordító gépen megvan a Gameloft 1280×800-as csomagja, a `make` 1080p-re nagyítja és a
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
