#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
export LC_ALL=C
filter=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export XDG_CONFIG_HOME="$work/config"
mkdir -p "$XDG_CONFIG_HOME/aurora-camera-denoise"
printf '[denoise]\nmode = invalid\n' > "$XDG_CONFIG_HOME/aurora-camera-denoise/denoise.conf"
if "$filter" --print-config > "$work/default.log" 2>&1; then exit 1; fi
printf '[denoise]\nmode = off\n' > "$work/session.conf"
"$filter" --config "$work/session.conf" --print-config > "$work/session.log" 2>&1
grep -q 'mode off' "$work/session.log"
if "$filter" --config "$work/missing.conf" --print-config > "$work/missing.log" 2>&1; then exit 1; fi
grep -q 'No such file' "$work/missing.log"
