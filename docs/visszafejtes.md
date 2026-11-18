# Visszafejtett tudnivalók (libPirates.so 1.2.8d)

A port a játék saját, futásidejű adatszerkezeteire épül. Ezek az eltolások és viselkedések statikus elemzésből
(`pirates.dis`) és a debug-build naplóiból jöttek; a kódban is ott vannak a felhasználás helyén. Más verziójú
`libPirates.so`-ra (SHA1 ≠ `9a36ce92…`) nem érvényesek.

## Képernyő, sprite-ok, betűk
- **Sprite-készlet:** az `Application::Init` 16:9-es képernyőn csak 1920 szélesen tölti a `sprites_1920x1080.gla`-t,
  minden más szélességnél `sprites_800x480.gla`-t keres. Ezért renderel a port 1920×1080-ban.
- **`interface.bsprite`:** 352 modul, a típusbájtok 0x10-től, az x/y/w/h `u16`-tömbök 0x170-től; 938 fmodul
  0xc70-től; 473 képkocka. A Switch-gombjelek (`source/glyphs.c`) ezekre a modulhelyekre kerülnek.
- **Szövegbe ágyazott ikon** (`CFont::DrawString` 0x226154, a `\x02<képkocka>` ág): a képernyőhöz mérten ugyanakkora,
  mint 1280×800-on.
- **Betűk:** a játék a fontot nem skálázza (NoScale modulméret, a sorköz is ebből jön), ezért az eredeti
  1280×800-as fontot 1080/800-szoros modulokkal kell adni (`tools/assets/make-fonts.py`).
- **Töltőképernyő-tipp** (`GS_Loading`): +56 az érintéses, +60 az Xperia/gamepad tipp (ha `dpad_open`),
  `s_FontMedium`-mal.

## Grafika
- **Köd:** a pályák Lua-szkriptjei állítják (`CLevel::SetFogDistance`/`SetFogColor`/`SetFogColorInterpolator`); a
  shaderek `FogStartEnd`/`FogColor` uniformmal számolnak, az erősséget `FogColor.a`-ra vágják. A glitch `SColor`
  bájtsorrendje ABGR (pl. 0xfa23385c = narancsbarna füst).
- **Látótáv:** far plane = 6000 × látótáv-szorzó; a játék AutoConfig-ja erős eszközön 1,1, gyengén 0,85.
- **Eszközerő** (`CGameConfig::CalculateDevicePower` 0x218fc4, `AutoConfig` 0x219438): csak a JNI-ből jövő modellnevet,
  CPU-frekvenciát és GPU-nevet nézi (a GL-sztringeket, a magszámot, a képernyőt nem). Mezők: +0 `m_iCPUProfile`, +16
  `m_iNPC_Limit_Level`, +0x444 `m_iGPUProfile`. A CPU-t a modelllista (`GetModelDevice` 0x217e58) után a frekvencia dönti
  el **kHz-ben** (`cpuinfo_max_freq`): ≥ 1,2e6 → 3 + magas geometria, < 1,0e6 → 1 + alacsony, köztük 2 + közepes. A GPU-nál
  „nvidia tegra”/„mali-400 mp” → 3 + magas grafika, „adreno” > 204, „powervr sgx” > 529 → 3. Az `AutoConfig` 1-es
  CPU-profilnál járókelő-korlátot kapcsol (`m_iNPC_Limit_Level` = 1: legfeljebb 1 × `m_NPC_citizen_factor` (2) járókelő,
  `Dragnet::GameEntity::UpdateDispecer`), magas grafikánál 1,1-es látótávot ad. A port válasza „NVIDIA Tegra X1” és
  1 785 000 kHz → a legerősebb osztály (a korábbi 1020.0 egy 1 MHz-es CPU volt: korláttal, alacsony geometriával).
- **Távoli alakok eltűnése** (fade, `CGameObject::UpdateRenderState` 0x4a0a00): ha a kamerától mért távolság² >
  `GFX_FACTOR` × `CLevel`+0x1918 (a pályaadat +0x9c négyzete, `CLevel::SetAllLodsDist` 0x22f750), az actor
  elhalványul. `GFX_FACTOR` = 0,25, ha a mentett `CGameSettings`+20 (az Advanced menü teljesítmény-kapcsolója) be van
  kapcsolva, különben 1,0 – a grafikai jelzők és a látótáv nem hatnak rá, tehát a gyárival azonos. Alacsony
  geometriánál ezen felül a `CNovaSceneManager::isCulled` (0x25b940) a képhez képest kicsi tárgyakat is kivágja.
- **Hiányzó textúrák** (`OW_Rocks`, `OW_Water`, `Sky_*` …): az eredeti Gameloft-csomagban sincsenek meg – nem port-hiba.
- **Shaderek:** az elterjedt 1080p-s adatcsomag `effects.gla`-jában 21 shader eltér a Gameloft eredeti csomagjáétól
  (pl. `LightMapDC-f`: fix 0,2-es köd a távolságfüggő helyett, telítettség-csökkentés nélkül; továbbá Caustics, Ice,
  Skybox, víz, lightmap-ek). Ettől a távoli növényzet (eredeti `VertexAnim*`, távolságfüggő köd) ködszínű
  sziluett a tiszta sziklák előtt. Az eredeti `effects.gla`-val (a Tegra-csomagból) a köd mindenen egyforma, a
  közeli kép változatlan; az eredeti `UnlitTexturedFP`-ben viszont „`#endif]`” elírás van, amit a Mesa elutasít
  (`source/gl_fix.c` javítja).

## Pályák, mentés
- **Pályabetöltés:** `Application+0x5c` = a pálya indexe a `GS_BaseMenu::m_levelsInfo` táblában (296 bájtos
  bejegyzések, +0x50 a fájlnév, `m_levelCount` = 52), `+0x60` = belépési pont (**-1 = alapértelmezett**, ahogy a Lua
  `RequireLoadLevel` 2. paraméter nélkül; a 0 egy tetszőleges spawn-pont). Az `Application::CheckLoadLevel` tölt –
  menüből, mentés nélkül a `CQuestManager` NULL, és elszáll. Betöltéskor a globális adatokat menti és checkpointot ír.
- **Continue:** `GS_SelectSaveSlot::StartContinueGame(bool, slot)`.

## Menük, irányítás
- **Szünetmenü** (`GS_InGameMenu`): +0x10 `CTabControl`, +0x48 Resume, +0x4c főmenü-gomb (a főfülön engedélyezett),
  +0x158 Back (alfüleken). Az Android BACK (`Handle_dv_key(4)`) az egész menüt zárja, az
  `is_open_cf_main_menu_screen` egy szintet lép.
- **HUD és érintés:** 480×320-as logikai koordinátában; a részletes mezőtérkép a `source/controls.c` fejlécében.
- **Mentésválasztó** (`GS_SelectSaveSlot`): +0x14 `CMenuButtons` (a mentések), +0x28 vissza, +0x2c indító gomb
  (`StartContinueGame`), +0x44 a kijelölt mentés; az Update a kijelölést a fókuszjelzős (`CButton`+0x74) elemhez
  igazítja. Új játéknál foglalt helyre a `GS_MMConfirm` kérdez rá.
- **Megerősítő ablakok:** `GS_Confirm` +0x30 „Yes”, +0x2c „No” (PopState, a Vissza is ezt teszi); `GS_MMConfirm`
  +0x14 `CMenuButtons`: 0 „Yes”, 1 „No”.
- **Átvezetők:** a `CLevel`+0x1af8 Skip-gomb (`EnableSkipCinematic`, 77-es esemény) az egész átvezetőt ugorja; a
  párbeszéd (`CDialogTextBox`, `ILevelMessage`) koppintásra a sor végére, illetve a következő sorra lép.
- **Gombfeliratok** (a `Draw`-ok `CFont::DrawString`-hívásai): `CButtonSpr` szöveg +0x88, betű +0x84, horgony +0x9c;
  `CButtonAnim` +0x98, +0x9c, +0xb0; `CButtonText` +0x80, +0x84, +0x8c. Betűk: `CSpriteManager::GetFont(s_FontSmall…)`;
  a `DrawString` x/y logikai képpont, a `GetStringSize` képernyő-képpontot ad. Horgonybitek: vízszintesen 1 közép,
  2 jobb (alap bal), függőlegesen 16 közép, 32 alul (alap felül). A jobbra igazításba és a középre igazításba a
  beágyazott ikon a rajzoltnál keskenyebben számít (a `GetStringSize` méri, és alulméri), így az ikonos felirat
  jobbra csúszik. A jobb szélre lógó `CButtonSpr` (Skip, Continue) szövegét a játék x = 470-re, jobbra igazítja.
- **Szövegbe ágyazott gombjelek** (`\x02<képkocka>`, az `interface.bsprite` képkockái; a képkocka-tábla: modulonkénti
  `u8` darabszám 0x2616-tól, a modulindexek `u16`-tömbje 0xc70-től): 327 ×, 330 □, 333 △, 336 ○ (a 240-es
  alapgombon a 250–253-as jel), 340 SELECT, 348/351 L/R (246 + 257/258), 321/408/363/362/405 a D-pad karjai
  (233/234/236/237, a játék tükrözve rakja össze), 354/397 az érintőpad (249). A 429–448 az érintéses HUD ikonjai.

## Betöltés
- A `GS_Loading::Update` képkockánként egy `CLevel::LoadStep`-et futtat, és közben `SetTargetFPS(1000)`-re vált
  (a `Release` 30-ra állítja vissza): az eredeti app a lépéseket a lehető leggyorsabban akarja. A port ilyenkor
  nem vár vsync-re (`gl.c` `gl_set_fast_frames`).
- Az indulás töltése a `GS_LoadMenu` (a `GS_Intro` után, a címkép – `GS_Splash` – előtt), lépésszámláló +0x18:
  1 sprite, 4 `CGameConfig::Config`, 5 `LoadInterface`, 6 `LoadGameConfig`, **10–137 üres képkockák**, 139–155 a menü
  háttérpályája (`GS_BaseMenu::LoadMenuLevel(20)`), utána a főmenü. A cél-fps-t ez nem emeli (`Application`+0x50 a
  cél, +0x54 az ezredmásodperce), így a 128 üres képkocka 30 fps-sel ~4,3 mp várakozás volt – a port erre az időre
  ugyanúgy 1000-re állítja, mint a `GS_Loading`.
- A PVRTC-textúrákat a játék szoftveresen bontja (`PVRTCDecompress` 0x425b60, a régi PowerVR SDK 2.x képpontonkénti
  `Decompress`-e; a segédek: `TwiddleUV` 0x425534, `InterpolateColours` 0x42539c, a moduláció a fő függvényben,
  táblák 0x681904/0x681914). Furcsaságai: az `Unpack5554Colour` kék-kiegészítése mindig az A színt írja; a 2 bites
  interpolált módnak nincs H/V-csak változata. A `source/pvrtc.c` ezt bájtra azonosan, szomszédságonként, NEON-nal és
  3 szálon számolja (Edenben, egy magon a 25-ös helyszínen 30 mp → 1 mp).
- Mérés (Eden, debug, egy mag): menütöltés 9,2 → 4,6 mp, pályatöltés 35 → 5,5 mp (fájl 0,8, textúrafeltöltés 1,9,
  PVRTC 1,0, shader 0,3 mp). A kiadási napló `menütöltés:` és `betöltés:` sora bontja.

## Az eredeti Gameloft-csomag
- A játék eredetileg az első induláskor töltötte le az eszközéhez illő adatcsomagot. A Tegra-változat
  (`BackStab_Photon_4G_Ver128_DXT.amz`, DXT-textúrák, 1280×800-as sprite-ok) egymás után fűzött ZIP-archívumokból
  áll: a `PK\x05\x06` végrekordok mentén szeletelve a Python `zipfile`-ja kibontja (34 bejegyzés, köztük minden
  `location_*.gla`, az `effects.gla` és az angol `strings.gla`).
- `sprites_1920x1080.gla` nincs benne; ehhez a port ma az 1080p-s adatcsomagra támaszkodik. A teljesen eredeti
  adatokkal futáshoz a `GetTextureFormat`-nak 3-at (.dds) kellene adnia, és 1280×800-as sprite-okkal kellene
  renderelni. A debug-build `ref800` kapcsolója a 2D-t (menük, HUD) már így mutatja; a 3D ott bíbor, mert az
  1080p-s csomag `location_*.gla`-i PVRTC-textúrásak – ehhez az eredeti csomag pályafájljai kellenének.
