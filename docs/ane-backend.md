# Neural Engine backend

The filter does not know how a frame gets denoised. It hands each NV12 frame to a backend through `struct cdn_backend_ops` in `src/backend.h` and blends nothing itself, so a model on the Neural Engine replaces the CPU code without touching the trigger, the config or the PipeWire side.

## The interface

    void *create(int width, int height, int threads);
    int   process(void *state, const struct cdn_frame *in, struct cdn_frame *out,
                  const struct cdn_frame_info *info, float strength);
    void  reset(void *state);
    float noise_sigma(void *state);
    void  destroy(void *state);

- `create` returns NULL when the backend cannot run here (no device, no permission, size outside the compiled model). The filter then uses the CPU backend, so a missing or busy Neural Engine never breaks the camera.
- `process` gets an NV12 frame in cached memory and writes the result to `out`. With `temporal = on` (the default) that input is already the temporal filter's output, so the model refines a cleaner frame. `strength` (0 to 1) is the trigger's fade, apply it as `in + strength * (denoised - in)`. `info` carries the sensor gain and exposure of the frame, which a model can take as a noise level input.
- `reset` drops temporal state, called when the filter passes frames through or the stream restarts.
- `noise_sigma` is optional, for logging.

A backend returns a negative errno from `process` to have that frame passed through.

## Loading

With `backend = auto` (the default) the filter tries each name in `prefer` (default `ane`) and loads `cdn-backend-<name>.so` from the library directory the filter was built with (`<libdir>/aurora-camera-denoise/`, so `/usr/lib64/...` on Fedora and `/usr/lib/...` on Arch), which must export

    const struct cdn_backend_ops *cdn_backend_entry(void);

returning an ops table with `abi = CDN_BACKEND_ABI`. `backend = ane` forces it, `backend = cpu` skips it. The plugin is its own package, so the filter never links the Neural Engine runtime.

## The ANE backend

`cdn-backend-ane.so` (src/ane/backend_ane.c) runs a small open CNN on the luma plane:

- The network works on the 2x2 space-to-depth grid of the frame, so a 1920x1080 frame is 4 planes of 960x540 (plane c holds pixel (2y + c/2, 2x + c%2)). Layers: 1x1 4->32, depthwise 3x3, 1x1 32->32, 3x3 32->32, depthwise 3x3, 1x1 32->16, ReLU after all but the last. Output planes 0-3 are a residual; the result is `in + strength * residual`. Chroma passes through.
- The host does the space-to-depth, the residual add and the depth-to-space inside the u8/fp16 conversion it needs anyway, so the program on the ANE is only the six convolutions.
- The program is built by our ANE compiler ([mil-hwx-compiler](https://github.com/aurora-silicon/mil-hwx-compiler), H16G) with `tools/ane/build-model.py` from `data/ane/weights.bin`. It lives in `<datadir>/aurora-camera-denoise/ane/h16g-<W>x<H>/` with the four load sections the Linux driver (accel/ane, ABI 3) needs. `CDN_ANE_MODEL_DIR` points elsewhere for testing.
- `create` returns NULL without an H16 ANE node, without a program for the grid size (only 960x540 ships), or when the load fails; the filter then keeps the temporal stage alone.

The weights were trained on our own camera clips from a MacBook Neo: inputs are single frames and the temporal filter's output, the target is the clip's mean over 60 frames, moving areas are masked out. On held-out frames of those clips, static areas improve from 32.7 to 38.7 dB PSNR after the temporal stage and from 29.7 to 37.9 dB on raw frames. That is two clips of one dim scene, so more scenes are the next step.

On the MacBook Pro (M4 Pro) the program runs a 960x540 grid in 3.8 ms of ANE time. Through `aurora-camera-denoise-bench` on 1080p frames the plugin's output is within one code value of a CPU reference of the same network (98% exact). Where the driver maps the program's surfaces (accel minor 1), the plugin converts the frame straight into the input surface, reads only the four residual planes of the output, and runs each request in place: paced at 30 fps that is 2.7 ms of CPU per frame and 6.4 ms of latency. With copies (older drivers, or `CDN_ANE_COPY=1`) it is 7.9 to 10.1 ms and 12 to 14 ms; both give the same output. The stage only runs above the board's low-light levels.

M1/M2 (H13/H14) and the MacBook Neo (H17) have no program yet; on them `create` returns NULL.
