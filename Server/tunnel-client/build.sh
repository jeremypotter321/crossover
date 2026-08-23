#!/usr/bin/env bash
# Builds the player-side tunnel for the platforms players actually run.
#   ./tunnel-client/build.sh   ->  dist/crossover-tunnel        (this machine)
#                                  dist/crossover-tunnel.exe    (Windows, 32-bit)
#
# The Windows build is 32-bit to match the rest of the mod's tooling, but it is a
# separate process from Fable.exe so the bitness is not actually load-bearing.
# It is statically linked so a player needs no runtime installed.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p dist

echo "==> native"
cc -O2 -Wall -o dist/crossover-tunnel tunnel-client/crossover-tunnel.c

if command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
  echo "==> windows (i686, static)"
  i686-w64-mingw32-gcc -O2 -Wall -static \
    -o dist/crossover-tunnel.exe tunnel-client/crossover-tunnel.c -lws2_32
else
  echo "==> windows SKIPPED (install mingw-w64: brew install mingw-w64)"
fi

ls -lh dist/
