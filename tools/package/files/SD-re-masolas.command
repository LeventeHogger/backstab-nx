#!/bin/bash
# Backstab HD – felmásolás a Switch SD-kártyájára (macOS). Dupla kattintással indul.
# Megkeresi a bedugott Switch-kártyát (amin van „atmosphere” mappa), az „SD-kartyara” mappa tartalmát
# összefésülve rámásolja (a kártyán lévő többi fájl és a játék mentései megmaradnak), majd kiadja a kártyát.
cd "$(dirname "$0")" || exit 1

pause() { echo; read -r -p "Nyomj Entert a bezáráshoz…" _; }
fail() { echo; echo "HIBA: $*"; pause; exit 1; }

[ -d SD-kartyara/switch/backstab ] || fail "nem találom az SD-kartyara mappát a szkript mellett."

cards=()
for v in /Volumes/*; do
  [ -d "$v/atmosphere" ] && cards+=("$v")
done
case ${#cards[@]} in
  0) fail "nem találok Switch SD-kártyát (amin van atmosphere mappa). Dugd be a kártyát, és indítsd újra." ;;
  1) SD="${cards[0]}" ;;
  *) echo "Több Switch-kártyát látok, melyikre másoljak?"
     select SD in "${cards[@]}"; do [ -n "$SD" ] && break; done ;;
esac

need_kb=$(du -sk SD-kartyara | cut -f1)
free_kb=$(df -k "$SD" | awk 'NR == 2 { print $4 }')
echo "SD-kártya:  $SD"
echo "Kell:       $((need_kb / 1024)) MB, szabad: $((free_kb / 1024)) MB"
[ "$free_kb" -gt $((need_kb + 51200)) ] || fail "nincs elég hely a kártyán."
echo
read -r -p "Mehet a másolás? (i/n) " answer
[ "$answer" = i ] || [ "$answer" = I ] || { echo "Megszakítva."; pause; exit 0; }

echo
echo "Másolás… (pár perc, a kártyaolvasótól függően)"
# rsync: csak hozzáad/frissít, semmit nem töröl a kártyáról; a macOS-attribútumokat nem viszi át
rsync -rt --progress --exclude '.DS_Store' --exclude '._*' SD-kartyara/ "$SD/" || fail "a másolás nem sikerült."
# a macOS a FAT/exFAT kártyán ._ fájlokat hagyhat: ezek a Switchen gondot okozhatnak
dot_clean -m "$SD/switch/backstab" 2>/dev/null
rm -f "$SD/._BackstabHD.nsp"
sync

echo
if diskutil eject "$SD" >/dev/null; then
  echo "Kész, a kártya kiadva – kihúzhatod. Folytatás az OLVASS_EL.txt 2. lépésénél (telepítés DBI-jal)."
else
  echo "Kész, de a kártyát nem tudtam kiadni: a Finderben dobd ki (Eject), mielőtt kihúzod."
fi
pause
