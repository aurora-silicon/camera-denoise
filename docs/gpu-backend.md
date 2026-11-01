# Vulkan temporal backend

`cdn-backend-gpu.so` implements the ABI v1 backend on Vulkan 1.1 compute.
It needs no display server or window surface. Honeykrisp/Borealis use the
same shaders on G13/G14/G16/G17; actual driver/device validation is recorded
separately below. A working hardware device is preferred automatically for
the always-on temporal stage. Missing plugin/device or a processing error
falls back to CPU. `--backend cpu` forces the CPU baseline, including the
temporal stage. The gain-triggered low-light plugin remains independent.

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
caches up to 16 imports by fd device/inode. The camera retains its own fds.

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
use uploads until the backend is recreated, avoiding repeated failed imports. Ordinary mapped NV12 gets
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
client with the low-light stage held equal. Run both `--backend cpu` and
`--backend auto`, record the selected device/transport, per-frame callback
latency and process CPU after warmup, and sample GPU busy and system power
using the device's available GPU/PMP/SMC telemetry. Record ambient light,
clock/governor, software revisions, sampling interval, baseline power and
whether the low-light stage ran. A paced offline run cannot substitute for
live camera measurements or establish system power savings.

## Measurement status

| Device | Backend | Latency p50 / p95 (ms) | Process CPU at 1080p30 | GPU busy | System power (PMP/SMC) | Parity vs CPU |
| --- | --- | --- | --- | --- | --- | --- |
| Neo / G17 | CPU / GPU | pending lead run | pending | pending | pending | pending recorded frames |
| J616s / G16 | CPU / GPU | pending lead run | pending | pending | pending | pending recorded frames |
| M1 Pro / G13 | CPU / GPU | pending lead run | pending | pending | pending | pending recorded frames |
| M2 / G14 | CPU / GPU | pending lead run | pending | pending | pending | pending recorded frames |

No Apple GPU performance or power claim is made until these paired rows
are populated. Hardware runs belong to the camera-quality lead; this code
change does not install anything or alter device services.
