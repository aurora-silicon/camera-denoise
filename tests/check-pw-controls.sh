#!/bin/bash
# SPDX-License-Identifier: MIT
# Isolated PipeWire server, synthetic source, no session/device inspection.
set -euo pipefail
filter=$1
fixture=$2
work=$3
mkdir -p "$work/runtime" "$work/config";chmod 700 "$work/runtime"
export LC_ALL=C XDG_CONFIG_HOME="$work/config"
export XDG_RUNTIME_DIR="$work/runtime" PIPEWIRE_RUNTIME_DIR="$work/runtime" PIPEWIRE_REMOTE=pipewire-0  # PipeWire >= 1.4 names the socket from module-protocol-native, not core.name
pids=()
cleanup() { for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done; wait || true; }
trap cleanup EXIT
# Only the stock server config: distro drop-ins (AirPlay discovery, device
# quirks) slow start-up past the wait below and reach outside the test.
mkdir -p "$work/pwconf"; cp /usr/share/pipewire/pipewire.conf "$work/pwconf/"
PIPEWIRE_CONFIG_DIR="$work/pwconf" pipewire -c pipewire.conf -P '{ core.name = effects-test }' > "$work/server.log" 2>&1 & pids+=($!)
for i in $(seq 1 100); do if pw-cli info 0 >/dev/null 2>&1; then break; fi; sleep .05; done
"$fixture" > "$work/fixture.log" 2>&1 & pids+=($!)
"$filter" --stream callA --mode off > "$work/A.log" 2>&1 & pids+=($!)
"$filter" --stream callB --mode off > "$work/B.log" 2>&1 & pids+=($!)
find_node() {
    pw-dump | python3 -c 'import json,sys; name=sys.argv[1]; print(next((o["id"] for o in json.load(sys.stdin) if o.get("info",{}).get("props",{}).get("node.name")==name),""))' "$1"
}
for i in $(seq 1 100); do
    a=$(find_node aurora_camera_denoise.callA.camera-effects-fixture)
    b=$(find_node aurora_camera_denoise.callB.camera-effects-fixture)
    if test -n "$a" && test -n "$b"; then break; fi
    sleep .05
done
test -n "$a" && test -n "$b"
pw-cli enum-params "$a" Props > "$work/A-off.txt"
pw-cli enum-params "$b" Props > "$work/B-off.txt"
pw-cli set-param "$a" Props '{ params = [ "camera.background" "blur" ] }'
pw-cli enum-params "$a" Props > "$work/A-blur.txt"
pw-cli enum-params "$b" Props > "$work/B-still-off.txt"
# Board has no framing block on this host; reject the whole mixed transaction.
pw-cli set-param "$a" Props '{ params = [ "camera.background" "replace" "camera.background.path" "/unused.png" "camera.framing" true ] }'
pw-cli enum-params "$a" Props > "$work/A-rejected.txt"
python3 - "$work" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])
assert 'String "off"' in (p/'A-off.txt').read_text()
assert 'String "off"' in (p/'B-off.txt').read_text()
assert 'String "blur"' in (p/'A-blur.txt').read_text()
assert 'String "off"' in (p/'B-still-off.txt').read_text()
assert 'String "blur"' in (p/'A-rejected.txt').read_text()
assert 'String "replace"' not in (p/'A-rejected.txt').read_text()
print('PASS: effects off, per-stream isolation, control round-trip, unsupported board rejection')
PY

timeout 12 "$fixture" --sink > "$work/sink.log" 2>&1 & sink=$!;pids+=("$sink")
for i in $(seq 1 100); do if pw-link -i | grep -q effects-test-sink; then break; fi; sleep .05; done
# Link by node: port names differ between PipeWire releases (out_0, capture_1).
pw-link aurora_camera_denoise.callA.camera-effects-fixture effects-test-sink
for i in $(seq 1 100); do if pw-link -i | grep -q aurora_camera_denoise.input; then break; fi; sleep .05; done
pw-link camera-effects-fixture aurora_camera_denoise.input
wait "$sink"
cat "$work/sink.log"
