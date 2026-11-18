#!/usr/bin/env python3
"""A nacptool NACP-jének kiegészítése a telepíthető NSP-hez (tools/package/pack-nsp.sh hívja).

- Mind a 16 nyelvi címbejegyzés kitöltve (a nacptool a koreai/kínai/brazil bejegyzést üresen hagyja), és mind
  támogatottnak jelölve (SupportedLanguageFlag): a HOME menü a rendszernyelv szerinti címet és ikont
  (icon_<nyelv>.dat) keresi – a pack-nsp.sh mindegyik nyelvre bemásolja az ikont.
- Ellenőrzi, hogy a mentés-/jelenlét-azonosítók a címazonosítóval egyeznek.
- A mentések az SD-n vannak (sdmc:/switch/backstab/…/files), ezért nincs felhasználói mentéshely és nincs
  felhasználóválasztás indításkor (a nacptool alapból 62 MB-os mentést és kötelező felhasználót kér).

Futtatás: python3 tools/package/nacp-fix.py <control.nacp> <címazonosító hexben>
"""
import struct
import sys

LANGS = ("AmericanEnglish BritishEnglish Japanese French German LatinAmericanSpanish Spanish Italian Dutch "
         "CanadianFrench Portuguese Russian Korean TraditionalChinese SimplifiedChinese BrazilianPortuguese").split()


def main():
    path, tid = sys.argv[1], int(sys.argv[2], 16)
    d = bytearray(open(path, "rb").read())
    assert len(d) == 0x4000, f"nem NACP: {len(d)} bájt"
    title = bytes(d[0:0x300])
    assert title[0], "üres az AmericanEnglish cím"
    for i in range(len(LANGS)):
        if not d[i * 0x300]:
            d[i * 0x300:(i + 1) * 0x300] = title
    struct.pack_into("<I", d, 0x302C, (1 << len(LANGS)) - 1)       # SupportedLanguageFlag
    d[0x3025] = 0                                                   # StartupUserAccount: nincs
    struct.pack_into("<QQ", d, 0x3080, 0, 0)                        # UserAccountSaveDataSize + Journal
    for off, name in ((0x3038, "PresenceGroupId"), (0x3078, "SaveDataOwnerId")):
        v = struct.unpack_from("<Q", d, off)[0]
        assert v == tid, f"{name} = {v:#018x}, a címazonosító {tid:#018x} (régi nacptool-hívás?)"
    open(path, "wb").write(d)
    print(f"NACP kész: 16 nyelv, felhasználóválasztás nélkül, azonosító {tid:#018x}")


if __name__ == "__main__":
    main()
