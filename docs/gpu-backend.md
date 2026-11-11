# Vulkan temporal backend

`cdn-backend-gpu.so` implements the ABI v1 backend on Vulkan 1.1 compute.
It needs no display server or window surface. Honeykrisp/Borealis use the
same shaders on G13/G14/G16/G17; actual driver/device validation is recorded
separately below. The temporal stage stays on the CPU unless you ask for
the GPU with `temporal-backend = gpu` in `denoise.conf` or
`--temporal-backend gpu`. `auto` never picks it, because on the Neo this
path costs about 3.3 times the CPU path's energy (see below). With the GPU
asked for, a missing plugin or device or a processing error falls back to
CPU. Processing failure recreates CPU history, so its first frame passes
through and temporal averaging warms up again. `--backend cpu` forces the
CPU baseline, including the temporal stage. The gain-triggered low-light
plugin remains independent.

Build with Vulkan development headers/loader and `glslangValidator`:

```sh
meson setup build -Dgpu=enabled
meson compile -C build
meson test -C build --print-errorlogs
meson install -C build
```

The default `-Dgpu=auto` omits the plugin if either build dependency is
missing; `-Dgpu=disabled` builds without Vulkan. SPIR-V is compiled from the
in-tree MIT GLSL and embedded in the plugin. No downloaded shaders, models,
runtime shader compiler or private implementation sources are used.

## Output and temporal state

Every stage uses the CPU backend's integer arithmetic: rounded Q15
multiplication, Q4 history, age-based running mean, 3x3 absolute and signed
5x5 motion tests, replicated edges, clipped 3x3 spatial luma, and two clipped
chroma passes at distances 1 and 2. The subsampled 16x9 block histograms,
median interpolation, median across eligible blocks, noise smoothing and
threshold scaling also run in compute shaders. Noise history survives
`reset`, while the next frame reseeds image history and passes through,
just as on CPU. Strength zero still advances history.

`test_gpu` compares the actual plugin against CPU across five sizes up to
1080p, padded strides, moving edges, changing noise, scene cuts, extreme
signed differences, strength ramps and resets. It requires byte equality,
including unchanged output padding, and equal reported noise. A missing
Vulkan device skips this test (exit 77). Set `CDN_TEST_REQUIRE_GPU=1` to turn
that into failure. The test enables software Vulkan explicitly; production
selection rejects CPU devices unless `CDN_GPU_ALLOW_SOFTWARE` is set.
Software Vulkan results establish correctness, not Apple GPU performance.

`test_gpu_dmabuf` exports real Vulkan DMA-BUF pools and calls the import
entry directly. Shared and split fds, independent padded strides, odd byte
offsets, pool reuse, reset and rejected bounds must all remain byte-exact
against CPU. It skips when export support is unavailable; set
`CDN_TEST_REQUIRE_DMABUF=1` to require it.

Validated on 2026-11-01 with Radeon RX 7900 XTX / RADV NAVI31:

- GPU-enabled build with warnings as errors: **7/7 tests passed**.
- GPU-disabled build with warnings as errors: **5/5 tests passed**.
- Upload path: 64 generated frames at each of five geometries through
  1920x1080, maximum byte error **0**, equal noise estimates.
- Import path: 24 frames for each of shared-fd and split-fd pools,
  maximum byte error **0**; the plugin logged imported input with no upload.
- Offline 12-frame 1080p NV12 benchmark: paired Y/UV PSNR **infinite**
  (byte equality), including every frame.

These checks use generated inputs and Vulkan-exported allocations. The
camera-exporter and Apple GPU measurements below remain for the on-device run.

## Buffer transport and synchronization

ABI v1's `cdn_backend_ops` and `cdn_frame` remain unchanged. A plugin can
export the optional `cdn_backend_dmabuf_entry` companion table described in
`src/backend.h`. Old plugins continue to load. The companion's single-upload
flag tells the daemon to omit its CPU-only cached-input copy.

For mapped **linear** camera DMA-BUFs, the daemon forwards borrowed fds,
plane strides and offsets (PipeWire `mapoffset` plus chunk offset). Single-fd
NV12 and separate plane fds are supported. Nonlinear modifiers are rejected
at format negotiation. Import uses `VK_EXT_external_memory_dma_buf`,
`VK_KHR_external_memory_fd` and `VK_EXT_queue_family_foreign`, queries memory
compatibility, duplicates fds before transferring ownership to Vulkan, and
caches up to 16 imports by fd device/inode. Reset releases those references
when a stream stops or passes through. The camera retains its own fds.

Before the mapped-input noise trigger and GPU access, polling the DMA-BUF waits for its implicit writer fence.
Foreign queue ownership is acquired and released with buffer barriers. The
backend sleeps on a Vulkan fence and finishes **all** reads before returning
the PipeWire input buffer, so it needs no exported release fence. It does
not negotiate explicit-sync buffers. A producer-fence timeout drops the
frame without filtering or reading it. This follows the [kernel DMA-BUF
synchronization contract](https://docs.kernel.org/driver-api/dma-buf.html)
and Vulkan's [external memory](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_dma_buf.html)
and [foreign queue ownership](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_queue_family_foreign.html)
interfaces.

If importing a particular allocation is unsupported, `-ENOTSUP` retries the
normal path without changing temporal history. Unsupported pool imports
use uploads until reset/backend recreation, avoiding repeated failed imports. Ordinary mapped NV12 gets
one row-wise copy directly into a persistent mapped Vulkan upload buffer.
No intermediate CPU denoising or noise histogram is performed. Both paths
read the completed output back into the existing PipeWire memory output;
zero-copy output is outside ABI v1. Noncoherent host memory is flushed and
invalidated explicitly. The existing subsampled low-light trigger still
reads mapped input luma. Unmappable DMA-BUFs and explicit-sync transport
need additional PipeWire negotiation and are not supported by this daemon.

The plugin logs the selected Vulkan device and its first imported/uploaded
frame, so a live measurement can confirm which transport actually ran.

## Reproduce parity and timing

Keep recorded camera frames private. Publish only derived statistics.
For an uninstalled build, `CDN_BACKEND_DIR` selects its plugin directory:

```sh
export CDN_BACKEND_DIR="$PWD/build"
build/aurora-camera-denoise-bench -b cpu -o /tmp/cpu.nv12 1920 1080 /private/input.nv12
build/aurora-camera-denoise-bench -b gpu -o /tmp/gpu.nv12 1920 1080 /private/input.nv12
build/aurora-camera-denoise-compare 1920 1080 /tmp/cpu.nv12 /tmp/gpu.nv12
# Timing only: omit output file I/O and run at the camera's cadence.
build/aurora-camera-denoise-bench -b cpu -r 30 -l 4 1920 1080 /private/input.nv12
build/aurora-camera-denoise-bench -b gpu -r 30 -l 4 1920 1080 /private/input.nv12
```

`compare` checks matching frames, reports aggregate and worst-frame Y/UV
PSNR and maximum byte error, and fails if **any** frame/plane is below
40 dB. Do not use `metrics` for this: it averages its reference capture.
The benchmark's latency includes upload/import, submission, all passes,
fence wait and output copy. CPU time includes driver worker threads.
Paced runs report measured CPU percentage as well as the extrapolated
CPU-at-30-fps figure. Initialization/pipeline compilation is outside the
measurement; only the first (seeding) latency is excluded from percentiles.

A redistributable synthetic sequence is available for smoke tests:

```sh
python3 tools/make-test-clip.py /tmp/synthetic.nv12  # requires numpy
```

For the live comparison, use the same 1080p30 scene and one streaming
client with the low-light stage held equal. Run both `--temporal-backend cpu` and
`--temporal-backend gpu`, record the selected device/transport, per-frame callback
latency and process CPU after warmup, and sample GPU busy and system power
using the device's available GPU/PMP/SMC telemetry. Record ambient light,
clock/governor, software revisions, sampling interval, baseline power and
whether the low-light stage ran. A paced offline run cannot substitute for
live camera measurements or establish system power savings.

## Measurement status

Earlier CPU-only live reference rows report ranges of logged means. A
paired CPU/GPU run with per-frame percentiles, GPU busy and system power is
still required on each target.

| Device | Backend | Filter latency (ms; statistic) | Process CPU at 1080p30 | GPU busy | System power (PMP/SMC) | Parity vs CPU |
| --- | --- | --- | --- | --- | --- | --- |
| Neo / G17 | CPU, earlier live reference | 15.1–15.3, logged means | 46% of one core | unmeasured | unmeasured | reference |
| Neo / G17 | CPU, live 11-01 | 14.3, mean | 42% of one core | unmeasured | 2034 mW (+397 mW over camera alone) | reference |
| Neo / G17 | GPU, live 11-01 | 6.1, mean | 3.5% of one core | unmeasured | 2960 mW (+1323 mW over camera alone) | pending recorded frames |
| J616s / G16 | CPU, earlier live reference | 15.1–17.1, logged means | 48% of one core | unmeasured | unmeasured | reference |
| J616s / G16 | GPU | pending lead run | pending | pending | pending | pending recorded frames |
| M1 Pro / G13 | CPU / GPU | pending lead run | pending | pending | pending | pending recorded frames |
| M2 / G14 | CPU / GPU | pending lead run | pending | pending | pending | pending recorded frames |

The Neo rows from 11-01 are 10 s means of the SMC PPMC rail at 1080p30 with
the camera open, at real governor clocks. The camera node alone drew
1637 mW. The GPU path is faster per frame but uses about 3.3 times the
CPU path's extra energy, which is why `auto` stays on the CPU. Likely costs
to look at first are per-frame submit and fence waits holding GPU clocks up,
the output copy and the number of dispatches per frame. Hardware runs belong to the camera-quality lead; this code
change does not install anything or alter device services.
