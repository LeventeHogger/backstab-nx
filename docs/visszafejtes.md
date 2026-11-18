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

## Az eredeti Gameloft-csomag
- A játék eredetileg az első induláskor töltötte le az eszközéhez illő adatcsomagot. A Tegra-változat
  (`BackStab_Photon_4G_Ver128_DXT.amz`, DXT-textúrák, 1280×800-as sprite-ok) egymás után fűzött ZIP-archívumokból
  áll: a `PK\x05\x06` végrekordok mentén szeletelve a Python `zipfile`-ja kibontja (34 bejegyzés, köztük minden
  `location_*.gla`, az `effects.gla` és az angol `strings.gla`).
- `sprites_1920x1080.gla` nincs benne; ehhez a port ma az 1080p-s adatcsomagra támaszkodik. A teljesen eredeti
  adatokkal futáshoz a `GetTextureFormat`-nak 3-at (.dds) kellene adnia, és 1280×800-as sprite-okkal kellene
  renderelni. A debug-build `ref800` kapcsolója a 2D-t (menük, HUD) már így mutatja; a 3D ott bíbor, mert az
  1080p-s csomag `location_*.gla`-i PVRTC-textúrásak – ehhez az eredeti csomag pályafájljai kellenének.
