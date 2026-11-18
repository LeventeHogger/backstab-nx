#!/usr/bin/env bash
# A bevezető videó a Switch-porthoz: a játék saját intro.mp4-éből (MPEG-4 Part 2 + AAC, 854×480, 25 fps)
# MPEG-1 + MP2 (48 kHz) – ezt a source/video.c a pl_mpeg-gel játssza le az első töltés előtt, mint Androidon
# a Java-indító MyVideoView-ja. Kell hozzá az ffmpeg (brew install ffmpeg).
#   tools/assets/make-intro.sh <intro.mp4> <kimenet: …/files/intro.mpg>
set -euo pipefail
[ $# -eq 2 ] || { echo "használat: $0 <intro.mp4> <intro.mpg>" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "nincs ffmpeg (brew install ffmpeg)" >&2; exit 1; }
ffmpeg -v error -y -i "$1" -c:v mpeg1video -r 25 -q:v 3 -c:a mp2 -b:a 192k -ar 48000 -ac 2 -f mpeg "$2"
echo "kész: $2 ($(du -h "$2" | cut -f1))"
