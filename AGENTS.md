# backstab-nx

A Backstab HD (Gameloft, 2011) Android-kiadásának (v1.2.8d, `lib/armeabi-v7a/libPirates.so`) betöltője Nintendo Switchre.
A [backstab-vita](https://github.com/v-atamanenko/backstab-vita) (MIT) loaderére épül: a játék eredeti ARMv7 kódja natívan fut,
a loader adja alá az Android-szerű környezetet (libc-híd, JNI, GLES2, hang, irányítás). A repó **nyilvános**: játékból
származó fájl, személyes útvonal vagy adat ne kerüljön bele. Állapot, teendők és a fejlesztőgép útvonalai: a helyi,
nem verziózott `HANDOFF.md` (ha van); visszafejtett részletek: `docs/visszafejtes.md`.

## Parancsok
- Mappák: `source/` a loader, `lib/` külső könyvtárak, `data/` beágyazott fájl, `toolchain/` eszközlánc (`hello/`
  próba), `tools/assets|dev|package`, `docs/`. Minden fordítási kimenet a `build/`-be, a telepíthető NSP és a csomag a
  `dist/`-be; a gyökérbe nem kerül generált fájl.
- Eszközlánc (egyszer): `toolchain/build-toolchain.sh`, majd `toolchain/build-portlibs.sh` → `~/devkitpro32`.
- Fordítás: `export DEVKITPRO=~/devkitpro32 DEVKITARM=~/devkitpro32/devkitARM PATH=~/devkitpro32/tools/bin:~/devkitpro32/devkitARM/bin:$PATH; make`
  → `build/backstab.nsp`. Debug: `make DEFINES="-D__SWITCH__ -D_GNU_SOURCE -DFALSOJNI_DEBUGLEVEL=FALSOJNI_DEBUG_WARN -DDEBUG"`
  (a `build/.defines` alapján váltáskor magától újrafordít; a debug-build naplózza a fájlmegnyitásokat, a HUD-ot, a
  menüket, a ködöt/látótávot – `source/debug_hooks.c`).
- Próba Edenben: `tools/dev/run-eden.sh [mp]` (`NOBUILD=1`: fordítás nélkül – debug-buildnél kell, mert a szkript
  kiadásit fordít; az Eden a vendég fájlírásait pufferben tartja, a szkript végi `kill -9` a napló utolsó sorait
  elnyelheti – kiadási buildben, ahol utána nincs több napló, a `menütöltés:` sort is: ez nem elakadás); szálak pillanatképe: `tools/dev/eden-stacks.py` (leírás a fájl elején). Gépelés nélküli pályapróba
  (debug-build, az SD `switch/backstab/` mappájában; előtte a mentéseket visszamásolni, a `leveltest` checkpointot ír):
  `autoinput` = „20:SHOT 10:SHOT 10:SHOT 1:A 5:A 4:A” (cím → Continue → az A a Save 1-en rögtön indít; a bevezető
  videó autoinputnál kimarad), `leveltest` = „25:130”; a töltések bontása a naplóban: `menütöltés:` és `betöltés:` sor (debugban a PVRTC-ellenőrzés lassít, `nopvrtccheck` kikapcsolja); `tools/dev/run-eden.sh 420`, a képek: `shots/` → `tools/dev/shot2png.py`.
- Telepíthető NSP (NCA-kkal; ezt listázza az Eden főoldala a `dist/`-ből, és ez megy valódi Switchre DBI/Tinfoil):
  `tools/package/pack-nsp.sh` (hacBrewPack + prod.keys; ezt a felhasználó maga indítja: `! tools/package/pack-nsp.sh`).
  A `make` kimenete (`build/backstab.nsp`) csak exefs-csomag: az Eden elindítja, de nem listázza.
- Importlefedettség: `tools/dev/check-imports.sh`. Eden-bevitel teszthez: `tools/dev/eden-input.sh keyboard|controller` (zárt Edennel).
- Eredeti betűkészlet: a `make` magától gyártja (`tools/assets/make-fonts.py`) és a binárisba ágyazza, ha megvan a cache-beli
  Gameloft-csomag (`FONT_PACK`, alap: `…/gameloft-data/sprites_1280x800.gla`) és a Pillow-s Python (`FONT_PY`); nélküle
  figyelmeztet, és az adatcsomag fontja marad. Az SD-re nem kell semmi.
- Kipróbálható csomag Switchre: kiadási `make` + `tools/package/pack-nsp.sh`, majd `tools/package/make-package.sh` → `dist/BackstabHD-Switch/`
  (SD-mappa, NSP, magyar útmutató: `tools/package/OLVASS_EL.txt`; a játékfájlok APFS-klónként, a saját példányból; a bevezető videót `tools/assets/make-intro.sh` gyártja ffmpeg-gel:
  `files/intro.mpg`). Források: `SO`, `DATA`, `STRINGS`, `EFFECTS` környezeti változók (a szkript fejlécében).
- Debug-kapcsolók az SD-n (`sdmc:/switch/backstab/`): `nopvrtccheck` (a gyors PVRTC-kibontást nem veti össze az eredetivel – a valódi töltési időhöz; alapból minden textúrát összevet, eltérésnél az eredeti marad, és a `pvrtc-önteszt` induláskor fut), `tiptest` (MAIN-szövegsorszámok, pl. 1536 = koponyás
  tipp; több szám esetén töltőképernyőnként a következő, és mindegyikről kép készül: `shots/tip_<sorszám>`), `ref800` (1280×800-as képernyő → a játék az eredeti `sprites_1280x800.gla`-t
  tölti, DXT-vel; ehhez a csomagot a `files/`-ba kell klónozni – referencia-képhez az eredeti kinézetről, a 3D bíbor),
  `leveltest` („<kezdő index> <mp>” vagy lista „idx:mp idx:mp”: a mentés kézi betöltése után sorban végigtölti a
  pályákat, napló: `[leveltest]` + memória; a mentéseket előtte el kell tenni, mert a betöltés checkpointot ír),
  `shadertest` (1/2/3: a VertexAnim-növényzet piros / köd nélkül / fehér fénnyel), `autoinput` (időzített
  gombnyomások, pl. a mentés betöltése gépelés nélkül: „40:SHOT 1:A 8:A …”; nyomva tartás „ZL/3000”, kar „LS0,-50”,
  „WAITSKIP” = vár a következő átvezetőre – `controls.c`; ha van, a bevezető videó kimarad), `evlog` (a játék
  eseményei játékmenetben: melyik gomb mit küld), `noglyphpatch` (a gombjel-képkockák átállítása nélkül). A `leveltest` és az
  `autoinput` SHOT-ja képkockát ment (`shots/*.rgba` → `tools/dev/shot2png.py`): így a teszthez nem kell a képernyőt
  fotózni, és billentyűt sem kell az Eden-ablakba küldeni. A debug-build a GL-t is figyeli
  (`dynlib.c`): le nem forduló shaderek, minden shader forrása `sdmc:/switch/backstab/shaders/<hash>.glsl`, köd- és
  mátrix-uniformok rajzoláskor (`[gl] rajz/TÁVOLI/VA`).
- Játékarchívumok/textúrák vizsgálata: `tools/assets/gla.py` (list/extract/png/modules); Python-eszközök Pillow-val:
  `~/Library/Caches/backstab-nx-toolchain/venv-tools/bin/python tools/assets/<x>.py` (a kimenet a cache-be, ne a repóba).

## Ami a kódból nem látszik
- A játék csak 32 bites ARM-ra létezik, ezért ez **32 bites (AArch32) Horizon-folyamat**, nem a szokásos 64 bites libnx-homebrew.
  Eszközlánc: a vita2hos-fejlesztők forkjai (patchelt devkitARM, `libnx32`). A hivatalos 64 bites devkitPro-val nem keveredik.
- **Mindent `-fno-short-enums`-szal** (4 bájtos enumok): az arm-none-eabi alapból 1 bájtos enumot használ, de a libnx a 64 bites
  elrendezésre épül (pl. NvGraphicBuffer az IPC-ben), és az Android ABI is 4 bájtos. A newlib/libstdc++ rövid enumú marad
  (`--no-enum-size-warning`).
- A libnx32-ben a mutatót tartalmazó IPC-struktúrák 32 biten rossz elrendezésűek lehetnek (az audout ilyen volt, az
  `audio.c` saját hívásokkal kerüli). Új libnx-szolgáltatásnál ezt ellenőrizni kell.
- **`address_space_type: 2`** (`backstab.json`, 32 bites címtér alias-régió nélkül): a 0-s típus heap-régiója csak 1 GB,
  és a kernel a folyamat memóriáját is erre korlátozza – a játék egy helyszín után ~580 MB-ot foglal, a GPU-pufferek is a
  heapből jönnek, és a 3–4. pályaváltásnál egy nagy foglalás elfogyott (memset NULL-ra). A 2-es típussal 2 GB a heap.
- **`heap.c`** (`--wrap` a newlib `_malloc_r`-családjára): a ≥128 KB-os foglalások a heap tetejéről lefelé növő külön
  régióba kerülnek, mert a newlib egyetlen sbrk-arénája pályaváltásonként feldarabolódott (743 → 1309 MB hat váltás alatt,
  miközben a foglalt 490–670 MB maradt). Azóta a lábnyom pályától függően 600–750 MB, nem nő.
- **`gl_fix.c`**: a Mesa szabványosan elutasít játék-shadereket, amiket az Android-meghajtók elnéztek (`UnlitTexturedFP`
  kockatérképes ága `texture2D(samplerCube, …)`) – a forrást feltöltéskor javítjuk.
- **Az 1080p-s adatcsomag 21 shadere eltér** az eredeti Gameloft-csomagétól (`effects.gla`; pl. `LightMapDC-f`: fix 0,2-es köd
  a távolságfüggő helyett, telítettség-csökkentés nélkül) – ettől a távoli növényzet (eredeti `VertexAnim*`, távolságfüggő
  köd) ködszínű „kék sziluett” a tiszta sziklák előtt. Ezért az eredeti `effects.gla` kell (a Gameloft `.amz`-csomagjából,
  `docs/visszafejtes.md`; a `make-package.sh` magától ezt teszi a csomagba, az Eden-SD-n is ez van). Az eredeti
  `UnlitTexturedFP` „`#endif]`”-jét a `gl_fix.c` javítja.
- **Eszközerő = a gyári app legerősebb osztálya:** a játék a JNI CPU-frekvenciáját **kHz-ben** várja (`jni_impl.c`:
  1 785 000; a régi 1020.0 1 MHz-nek számított → járókelő-korlát, alacsony geometria). A grafikai jelzőket a játék maga
  választja (`config.txt` -1), a kiadási napló `patch: AutoConfig:` sora mutatja. Részletek: `docs/visszafejtes.md`.
- Lebegőpontos ABI: `-mfloat-abi=softfp`, egyezik az Android armeabi-v7a-val – a libm átjáró nélkül köthető.
- **1920×1080 a játék logikai felbontása**: az `Application::Init` 16:9-nél csak 1920 szélesen tölti a `sprites_1920x1080.gla`-t,
  más szélességnél `sprites_800x480.gla`-t keres (az adatcsomagban nincs) és nem rajzol semmit.
- **A HUD és az érintés 480×320-as logikai koordinátában él** (`s_logicalScreenWidth/Height`; az `appOnTouch` a pixelt
  `s_screenScaleW/H`-val osztja). Az irányítás ezért a játék élő HUD-objektumaiból számol (`CLevel` → `ILevelInterface`
  → `ControlScheme`: WalkJoystick, kamera, multi-gombok; `AnalogJoystick::MAX_DISTANCE` logikai pixelben), a kamera a
  játék saját forgatás-eseményét küldi, a menük a natív Xperia Play-fókusznavigációt (`menu_action`, `CButton +0x74`).
  Részletek: `source/controls.c` fejléce. Fix (Vita-ból skálázott) érintéspontokra csak tartalékként támaszkodunk.
  A gombok a súgók **kontrolleres (Xperia) szövegét** követik (`dpad_open` = 1, ezt látja a játékos): célzás közben a
  „második” gomb tiltva van, a lövés a helyén álló `CButtonSweep` (ide koppint a ZR); a ZL nyomva tartva célzás, a
  felengedés elteszi. A séta fix sebességű, a játék küszöbe |kitérés| 0,10–0,95 (`PlayerComponent`+0x2f0): a kar
  85%-áig ebbe a sávba képezünk. Esemény-azonosítók és mérések: `docs/visszafejtes.md`.
- **Gombjelek a játék feliratain:** a `patch.c` a `CButtonSpr::Draw` idejére a felirat (+0x88) elé teszi a
  `\x02<képkocka>` ikont (`port.h` `GLYPH_*`: a glyphs.c Switch-jelei), és utána visszaállítja – a játék
  objektumába tartósan nem írunk (a felszabadított gombokba írás memóriát rontana). Melyik gombra mi kerül: `controls.c`
  `labels_update`. Átvezetőben (a `CLevel`+0x1af8 Skip él) A = Skip, B = a párbeszéd következő sora. A jobbra
  igazított szövegbe ágyazott ikont a `CFont::DrawString` rosszul igazítja (lelóg): a jobb szélre lógó feliratnál
  a szöveg marad, az ikont a `DrawString`-hook külön rajzolja elé; a horgonybitek: `docs/visszafejtes.md`.
  Az `interface` textúraatlaszban nincs szabad hely: ahol két súgójel közös modulon osztozott (mozgás/nézelődés,
  L1/R1), a `patch.c` `glyph_frames_patch` a képkockát memóriában az érintéses súgó (kontrolleres módban soha meg
  nem jelenő) moduljára állítja át (tükrözés-jelzővel együtt), a képet a `make-glyphs.py` rajzolja oda.
- **Az 1080p-s adatcsomag `eu_font_*` fontjai nem eredetiek** (utólag gyártottak: hiányzó à é ß, a
  „small” nagyobb a „medium”-nál). Az eredeti a Gameloft 1280×800-as csomagjában van; a játék a fontot nem skálázza
  (NoScale modulméret, a sorköz is abból), ezért 1080/800-szoros modulokkal pontosan az eredeti arány jön ki.
  A `patch.c` a binárisba ágyazott `hd_` fájlokat adja helyettük memóriafájlként (`CCustomFileSystem::createAndOpenFile`
  + `CTextureManager::getTexture`; a textúrát a játék `.tga` néven kéri, a `.pvr`-cserét a saját fájlrendszere végezné). A szövegbe ágyazott ikonok (`\x02<képkocka>`) mérete a képernyőhöz
  képest az eredetivel azonos – a „túl nagy ikon” a kis, nem eredeti font miatt látszott.
- A glitch `SColor` bájtsorrendje ABGR (a `SColorf(SColor)` a legalsó bájtot olvassa pirosként) – a pályák ködszínei
  (pl. 0xfa23385c = narancsbarna füst) így helyesek, nem kell cserélni.
- Bevezető videó: Androidon a Java-indító játszotta le (`files/intro.mp4`, a natív kód nem tud róla); itt a `video.c`
  (pl_mpeg, MIT, `lib/pl_mpeg`) játssza a `files/intro.mpg`-t a motor indulása előtt, A/B/+/−/koppintással átugorható.
- Hook csak `so_finalize()` előtt (utána a kód RX); az eredeti hívása pontos típussal: `SO_CONTINUE(int (*)(void *), h, ...)`.
  Hookolni csak olyan ARM-függvényt, aminek az első két utasítása áthelyezhető (nincs ág, csak `ldr rX,[pc,#imm]` PC-relatív).
- Az Eden (yuzu-alapú) a nyers `exefs/main`-t 64 bitesnek veszi: mindig az NSP-t kell betölteni. Multi-core módban a
  dynarmic A32 JIT összeomlik → egymagos mód. A Ryujinx 32 bites JIT-je hosszú ciklusoknál elszáll, arra nem jó.
- 32 bites folyamat Homebrew menüből (.nro) nem indul: NSP-ként fut. Valódi Switchen az alkalmazás erőforráskorlátja szűk
  (1 saját session, 96 szál, 32 transfer memory), az Eden nem korlátoz: ami ott megy, a gépen 0x10801-gyel (LimitReached)
  elszállhat – ezért zárja le a `so_util.c` a saját folyamat-handle megszerzésének idejére az `sm:`-et (mint az nx-hbloader).
- Címazonosító: `0100BAC57AB00000` (`backstab.json`, `Makefile` NACPFLAGS – **0x nélkül**, mert a nacptool csak 16
  karaktert olvas –, `tools/package/pack-nsp.sh`; együtt változnak). A telepíthető NSP NACP-jét a `tools/package/nacp-fix.py` egészíti ki:
  16 nyelv címmel és ikonnal (a HOME menü a rendszernyelv szerintit keresi), nincs felhasználóválasztás és
  rendszermentés – **a mentés az SD-n van** (`files/save*.dat`; Levi döntése, a Switch-mentés később ráépíthető). A korábbi
  `010000000000100D` a rendszer Album-appletjéé (a homebrew-k ezt használják): valódi gépre telepítve ütközne.

## Játékfájlok
- Soha nem kerülnek a repóba, és a lefordított NSP-t sem tesszük közzé (a beágyazott font a játékból van). A `data/`
  alatt csak a Vita-port saját `control0` billentyűsémája van (MIT). A helyi példány útvonalai: `HANDOFF.md`.
- A Gameloft eredeti csomagja (angol `strings.gla`, eredeti `effects.gla`, 1280×800-as sprite-ok) a cache-ben:
  `~/Library/Caches/backstab-nx-toolchain/gameloft-data/` (kibontás: `docs/visszafejtes.md`).
- `libPirates.so` SHA1: `9a36ce92ac09f2908c155ab1debb3e138a3496b8` – más verzióra a foltok címei nem stimmelnek.
