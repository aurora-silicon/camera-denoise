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
meson compile -C "$out/filter" -j8
meson test -C "$out/filter" --print-errorlogs

cc -Wall -Wextra -Werror $(pkg-config --cflags libpipewire-0.3) tests/pw-fixture.c $(pkg-config --libs libpipewire-0.3) -o "$out/pw-fixture"
mkdir -p "$out/prefix/lib/aurora-camera-denoise"
cc -Wall -Wextra -Werror -O2 -fPIC -shared tests/effects-plugin.c -o "$out/prefix/lib/aurora-camera-denoise/libaurora-camera-effects.so"
bash tests/check-pw-controls.sh "$out/filter/aurora-camera-denoise" "$out/pw-fixture" "$out/pw" > "$out/pw-controls.log" 2>&1
