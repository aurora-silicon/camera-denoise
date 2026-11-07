#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
out=$(realpath -m "${BUILD_OUT:?}")
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
python3 -m venv "$out/venv"
source "$out/venv/bin/activate"
pip -q install meson==1.9.1 ninja==1.13.0
pkg-config --modversion libpipewire-0.3
meson setup "$out/filter" . -Ddaemon=enabled -Dwerror=true --prefix="$out/prefix" --libdir=lib
sha256sum src/*.c src/*.h tests/*.[ch] tests/check-pw-controls.sh tools/check-buildbox.sh > "$out/filter/meson-logs/effects-source.sha256"
meson compile -C "$out/filter" -j8
meson test -C "$out/filter" --print-errorlogs

cc -Wall -Wextra -Werror $(pkg-config --cflags libpipewire-0.3) tests/pw-fixture.c $(pkg-config --libs libpipewire-0.3) -o "$out/pw-fixture"
mkdir -p "$out/prefix/lib/aurora-camera-denoise"
cc -Wall -Wextra -Werror -O2 -fPIC -shared tests/effects-plugin.c -o "$out/prefix/lib/aurora-camera-denoise/libaurora-camera-effects.so"
result=0
bash tests/check-pw-controls.sh "$out/filter/aurora-camera-denoise" "$out/pw-fixture" "$out/pw" > "$out/pw-controls.log" 2>&1 || result=$?
tail -20 "$out/pw-controls.log"
if test "$result" != 0; then
  for log in "$out/pw/server.log" "$out/pw/A.log" "$out/pw/fixture.log" "$out/pw/sink.log"; do
    if test -f "$log"; then tail -10 "$log"; fi
  done
fi
exit "$result"
