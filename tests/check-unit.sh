#!/bin/sh
# SPDX-License-Identifier: MIT
# The user unit must skip root and system users (greeter) and must not
# restart forever when PipeWire is down.
set -eu
unit=$1
grep -qx 'ConditionUser=!root' "$unit"
grep -qx 'ConditionUser=!@system' "$unit"
grep -qx 'Restart=on-failure' "$unit"
interval=$(sed -n 's/^StartLimitIntervalSec=//p' "$unit")
burst=$(sed -n 's/^StartLimitBurst=//p' "$unit")
delay=$(sed -n 's/^RestartSec=//p' "$unit")
[ -n "$interval" ] && [ -n "$burst" ] && [ -n "$delay" ]
# The burst must be reachable inside the interval, or the limit never trips.
[ $((burst * delay)) -lt "$interval" ]
# Both settings belong to [Unit]; systemd ignores StartLimit* in [Service].
awk '/^\[/{s=$0} /^StartLimit/{if (s != "[Unit]") exit 1}' "$unit"
awk '/^\[/{s=$0} /^ConditionUser/{if (s != "[Unit]") exit 1}' "$unit"
if command -v systemd-analyze >/dev/null 2>&1; then
  out=$(systemd-analyze --user verify "$unit" 2>&1 || true)
  if printf '%s\n' "$out" | grep -E 'Unknown (key|section)|Failed to parse|bad-setting'; then
    exit 1
  fi
fi
