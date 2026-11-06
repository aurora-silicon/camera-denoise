# camera-denoise

A PipeWire video filter that takes the grain out of the built-in camera on Apple silicon laptops, the way macOS does: a temporal noise filter on every frame, and its automatic video noise reduction on top in low light.

It runs as a user service. For each built-in camera (the `apple-isp` V4L2 driver) it adds a camera node with the same name and formats, which apps pick by default through the camera portal, PipeWire or GStreamer. The original stays available as "FaceTime HD Camera (V4L2)". Nothing touches the camera until an app streams from the filter.

## When it filters

The temporal filter runs on every frame of every stream, as macOS runs its motion-compensated temporal filter on every camera stream. It's what makes the macOS camera look clean in a normal room. Measured on the MacBook Pro 16-inch M4 Pro, same scene, on a flat part of the picture: luma noise 13.6 straight from the camera, 2.4 through this filter and 2.1 on macOS (8-bit code values).

A low-light stage joins in on top when a backend plugin such as the Neural Engine one is installed, like macOS' machine-learning noise reduction. In `auto` mode it follows two signals, each with its own on and off level so it does not flicker:

- the total sensor gain the camera driver reports for every frame (`V4L2_CID_GAIN`, read-only, 1/256 steps), with the same levels macOS uses on each machine
- the noise it measures from frame to frame, for noise the gain does not explain (a camera running without its sensor tuning, for example)

It also requires a frame rate of 30 fps or less, and fades out over 30 frames instead of switching off. Below the levels the low-light stage is off and only the temporal filter runs.

The levels are board data in `/usr/share/aurora-camera-denoise/boards.conf`, keyed by the device-tree compatible. Boards without data get the temporal filter only, unless you set `mode = on`.

| Board | On at gain | Off below | Noise on / off |
| --- | --- | --- | --- |
| MacBook Neo (`apple,j700`) | 2304 (9x) | 1792 (7x) | 7 / 5 |
| MacBook Pro 16-inch M4 Pro (`apple,j616s`) | 3584 (14x) | 2816 (11x) | 7 / 5 |

Kernels without the gain report fall back to the noise signal alone.

## Settings

Copy `/usr/share/aurora-camera-denoise/examples/denoise.conf` to `~/.config/aurora-camera-denoise/denoise.conf` (or `/etc/aurora-camera-denoise/denoise.conf` for everyone) and change what you like, then `systemctl --user reload aurora-camera-denoise`.

- `mode = auto | on | off`, where `off` passes every frame through unchanged
- `low-light-frame-rate = on | off`. With `on` (the default) the camera may slow down to 15 fps in low light for a longer exposure instead of more gain, as macOS lets it. The filter sets this on the camera before it streams; `off` keeps a fixed frame rate
- `temporal = on | off`. With `off` the filter works as it did before: nothing runs below the levels, and above them the one backend (Neural Engine or CPU) does all the work
- `temporal-backend = auto | cpu | gpu`, where `auto` is the CPU. `gpu` runs the temporal filter on Vulkan compute when the GPU plugin is installed and works, else the CPU. It is opt-in because it costs more energy than the CPU today
- `backend = auto | cpu`, where `auto` uses the Neural Engine backend for low light when installed and working; `cpu` forces the CPU for both stages, for measurements
- any board level, e.g. `enter-gain = 1536`, or `enter-noise = 0` to follow the gain only

To turn it off completely, `systemctl --user mask --now aurora-camera-denoise`. The distro packages enable it for every user through a preset, so `disable` alone does not stick.

The optional [Vulkan backend](docs/gpu-backend.md) implements the same filter, including noise estimation, on the GPU, when you set `temporal-backend = gpu`. It imports linear camera DMA-BUFs when supported and otherwise uploads once. Build with `-Dgpu=enabled` to require it.

## How the filter works

Each pixel keeps a short history. A motion test compares the new frame against it twice: the 3x3 sum of absolute differences catches textured motion, and the 5x5 sum of signed differences catches a dark subject moving over a dark background, where the noise cancels but the change adds up. Still pixels average about 15 frames, moving ones restart a running mean (1/2, 1/3, ...) so nothing leaves a trail. An edge-preserving 3x3 filter then smooths what the temporal filter could not, mostly moving areas, and chroma gets two wider passes since low-light colour noise is blotchy. All thresholds scale with the noise measured on the still parts of the frame.

It is integer NEON code, one thread by default. At 1080p30 a frame takes about 3 ms of one core at full clock. See `docs/measurements.md`.

## Backends

The filter calls a small backend interface (`src/backend.h`). The CPU backend is built in, other backends are shared objects in the package's library directory (`/usr/lib64/aurora-camera-denoise/` on Fedora, `/usr/lib/aurora-camera-denoise/` on Arch) loaded by name. `docs/ane-backend.md` describes how the Neural Engine backend plugs in.

## Tools

- `aurora-camera-denoise-bench W H frames.nv12` runs a backend over raw NV12 frames and reports latency and CPU time (`-b gpu`, `-r 30` for paced timing)
- `aurora-camera-denoise-compare W H cpu.nv12 gpu.nv12` checks paired output PSNR against CPU, with a 40 dB minimum for every frame and plane
- `aurora-camera-denoise-metrics W H reference.nv12 test.nv12` reports temporal noise, PSNR and SSIM against the average of a long static capture

## Building

    meson setup build && ninja -C build && meson test -C build

Needs `libpipewire-0.3` for the filter itself. The backend, tools and tests build without it.

## License

MIT
