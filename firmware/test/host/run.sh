#!/bin/sh
# Builds and runs the text adventure on the host (see stubs.c).
#
#   test/host/run.sh                     play interactively
#   test/host/run.sh < walkthrough.txt   replay a transcript
set -e

here=$(cd "$(dirname "$0")" && pwd)
fw=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/bsorl26-host
mkdir -p "$out/include"

# game.c and ctf.c include ch32fun.h for the badge build only.
: > "$out/include/ch32fun.h"

python3 "$fw/scripts/gen_text.py" "$fw/src/game_text.txt" "$out/include/game_text.h" >/dev/null

cc -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g -fsanitize=address,undefined -DGAME_HOST_BUILD \
	-I"$out/include" -I"$fw/src" \
	"$here/stubs.c" "$fw/src/game.c" "$fw/src/ctf.c" \
	-o "$out/game"

exec "$out/game"
