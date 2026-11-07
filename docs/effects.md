# Per-stream video effects

Background blur/replacement and auto-framing are user choices. Every exported
output starts with both effects off. Installing the model backend or selecting
a board never enables an effect.

For a call that needs its own controls, start a named filter instance:

```sh
aurora-camera-denoise --stream work-call
```

Select the camera named `<camera name> / work-call` in the calling application.
Its node name is `aurora_camera_denoise.work-call.<source node name>`. A second
instance, e.g. `--stream personal-call`, has separate controls and history.
Named outputs have priority zero so they do not change the default camera.
All clients attached to the **same** output see that output's choices; use
separate named outputs for different calls. The underlying camera still owns
its physical format constraints. User stream names use 1–48 letters, digits,
underscores or hyphens.

Use `pw-cli ls Node` or `pw-dump` to find that output's node ID, then set its
standard `Props` parameter. These are stream controls, not environment or
kernel tuning variables:

```sh
# Background blur only
pw-cli set-param NODE_ID Props '{ params = [ "camera.background" "blur" ] }'

# Replace with a user-owned image. The filter runs as this user.
pw-cli set-param NODE_ID Props '{ params = [ "camera.background.path" "/path/to/background.png" "camera.background" "replace" ] }'

# Automatic face-driven framing (on a supported board)
pw-cli set-param NODE_ID Props '{ params = [ "camera.framing" true "camera.zoom" 0.0 ] }'

# User-selected fixed zoom, while continuing to center on detected faces
pw-cli set-param NODE_ID Props '{ params = [ "camera.framing" true "camera.zoom" 2.0 ] }'

# Disable both. This releases the model worker and restores direct output.
pw-cli set-param NODE_ID Props '{ params = [ "camera.background" "off" "camera.framing" false ] }'

pw-cli enum-params NODE_ID Props
```

Control transactions are validated completely before applying any change.
Unknown/malformed controls, unsupported framing, out-of-range zoom, or replacement
without an image path leave the previous choices in place and log a diagnostic.
Settings last for that output's lifetime. `camera.zoom = 0.0` means automatic
zoom; framing starts at the board's default. Both effects can be combined.

The existing `boards.conf`, selected by device-tree compatible, supplies stock
framing capability and zoom bounds. J616s has `framing-supported = true`, zoom
1.0–6.0, and default 1.5 for its ultra-wide front sensor. J700/Neo and unknown
boards have no framing capability by default. On any board the user can opt in:
`framing-supported = true` and the
`framing-zoom-min`/`-max`/`-default` keys in the `[denoise]` section of
`~/.config/aurora-camera-denoise/denoise.conf` (or `/etc/...`) enable it with
the same validation as board data. Reload an instance with SIGHUP after editing
the config; capabilities update while each stream's effects remain opt-in.
On a narrower sensor, configure the range to match the available field of view.
Node properties `camera.framing.supported` and `camera.zoom.min`,
`camera.zoom.max`, `camera.zoom.default` expose this capability to UIs. No
framing calibration was taken from private files. Denoise controls and board
thresholds continue to use their established config.

## Optional backend and frame ownership

Install `examples/camera-effects` from the `aurora-silicon/omarchy-ane` companion
change, including its audited open models, Python environment and shared libane
runtime. The filter loads `<libdir>/aurora-camera-denoise/libaurora-camera-effects.so`
only after a stream explicitly requests an effect. `src/camera_effects.h` mirrors
the companion backend's ABI v1 exactly.

Model work runs on a non-RT worker with four-slot input/output queues and owned
NV12 buffers. The PipeWire callback copies/submits and takes completed frames;
it neither allocates model state nor waits for inference. A pending output
buffer is returned to PipeWire's free pool. A slow worker drops overloaded
inputs, discards output older than 200 ms and retains original frame timestamps.
It never combines an old mask with a new image. Ordinary dropped frames preserve time-based tracking; timestamp reversals or gaps
over one second reset it.

Effects run after the existing denoiser and are independent of its gain trigger.
Disabling denoise does not disable a chosen effect. Model creation/inference
failure passes the original denoised frame and logs a diagnostic; effects are
visual conveniences, not a privacy boundary. A format change (including matrix
or range), stream restart or control change discards queued results and history.
The worker is joined on teardown; no PipeWire buffer survives a callback.

## Host verification and lead qualification

`BUILD_OUT=/fresh/output bash tools/check-buildbox.sh` builds the full daemon
with warnings as errors, runs the C tests, then uses an isolated PipeWire server
and synthetic NV12 source. The optional host test plugin is built only into that
job's temporary prefix; it injects an inference stall and is never installed by
the project. The fixture checks control round-trips, stream isolation and
unsupported-board rejection, then drives the frame pipeline through a stall.

Target camera, portal/UI selection, real lighting, multi-client negotiation and
ANE behavior remain lead-run qualification. See the companion backend's
`LEAD-TEST-PLAN.md` for acceptance criteria and rollback. No packaged effects
policy, model service preset or target deployment is included here.

## Explicit digital-crop opt-in

A named demo can use `--config /path/to/session.conf`. This replaces the system
and default user settings for that process; packaged board data is still read.
A missing explicit file is an error. CLI mode/backend still take precedence.

To opt into digital face framing on another board, put this in that file:

```ini
[denoise]
framing-supported = true
framing-zoom-min = 1
framing-zoom-max = 6
framing-zoom-default = 1.5
```

This enables a software crop, without lens calibration or a wider field of view.
It does not change packaged board data. Only processes selecting this file gain
the capability; framing itself still requires the per-output Props control.
