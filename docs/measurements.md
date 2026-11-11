# Measurements

All on the 7.3 fleet12 kernel, Fedora 45, PipeWire 1.6.9, 1920x1080 NV12 at 30 fps, evening room light.

## Noise (MacBook Neo, static scene)

Sensor gain during the captures was 3253 to 4168 (12.7x to 16.3x), above the Neo's 2304 on level.

The reference is the average of every frame of a long static capture of the same scene. Both rows use the same input frames, the second after the CPU backend (frames 40 onward, after it settles).

Captured through the filter node in pass-through, 240 frames, mean luma 67:

| | Temporal std Y / Cb / Cr | PSNR Y / C vs reference | SSIM Y |
| --- | --- | --- | --- |
| Camera | 11.07 / 2.05 / 1.90 | 25.40 / 41.16 dB | 0.511 |
| Filtered | 6.40 / 1.13 / 1.10 | 27.68 / 44.53 dB | 0.805 |

Captured with v4l2-ctl, 150 frames, mean luma 49:

| | Temporal std Y / Cb / Cr | PSNR Y / C vs reference | SSIM Y |
| --- | --- | --- | --- |
| Camera | 8.60 / 1.25 / 1.52 | 28.51 / 44.72 dB | 0.545 |
| Filtered | 4.67 / 0.71 / 0.87 | 32.70 / 48.42 dB | 0.783 |

On the MacBook Pro (J616s) the test scene had a person walking through it, so temporal statistics mean little. Checked by eye frame by frame, no trails or doubled edges behind the moving subject. The synthetic test (`tests/test_cpu.c`) checks the same: still areas 18 dB cleaner, the area a moving block just left closer to the truth than the noisy input.

## CPU

`aurora-camera-denoise-bench`, one thread, back to back (so at full clock):

| Machine | ms per frame | CPU at 30 fps |
| --- | --- | --- |
| MacBook Pro J616s | 3.1 | 9% of one core |
| MacBook Neo | 3.1 | 9% of one core |

Live, in the PipeWire filter at 30 fps, the governor runs it at low clocks (1.26 GHz on the J616s, 0.8 to 1.8 GHz on the Neo), so each frame takes longer but costs little energy:

| Machine | Filtering | Passing through |
| --- | --- | --- |
| MacBook Pro J616s | 8.8 ms per frame, 27% of one core | |
| MacBook Neo | 13.2 ms per frame, 39% of one core | 3.7% of one core |

The filter adds that processing time as latency and no frames, since the temporal filter only looks back.

Four threads on the J616s cut a frame to 1.2 ms for about 15% more CPU time (3.5 ms). One thread stays the default since it costs the least, `threads = 4` trades a little CPU for latency.

## Video encoder filter

`temporal-backend = auto` on the MacBook Neo (fleet kernel with the AVE MCTF device and the scaler's lossless form). `aurora-camera-denoise-bench` paced at 30 fps over a 60-frame 1080p camera clip, 180 s per run, system power from the SMC power sensor (hwmon), screen on, same boot:

| Run | System power | Above idle | CPU at 30 fps |
| --- | --- | --- | --- |
| idle | 6.249 W | | |
| NEON (`cpu`) | 6.740 W | +0.49 W | 38.7% of one core (12.9 ms per frame) |
| MCTF plugin (`ave`) | 6.581 W | +0.33 W | 4.1% (1.36 ms per frame) |
| MCTF chain alone | 6.504 W | +0.26 W | 0.17 ms per frame in the process |

The baseline dropped by about 1.8 W later in the session, so a second pair is compared on its own: idle 4.419 W, `ave` 4.720 W (+0.30 W, 3.7%). The plugin's output is bit-exact with macOS 26.6.2 VideoToolbox's temporal noise filter at strength 1.0 on the same frames, at 15 ms latency.
