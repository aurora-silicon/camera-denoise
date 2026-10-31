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
- `process` gets an NV12 frame in cached memory and writes the result to `out`. `strength` (0 to 1) is the trigger's fade, apply it as `in + strength * (denoised - in)`. `info` carries the sensor gain and exposure of the frame, which a model can take as a noise level input.
- `reset` drops temporal state, called when the filter passes frames through or the stream restarts.
- `noise_sigma` is optional, for logging.

A backend returns a negative errno from `process` to have that frame passed through.

## Loading

With `backend = auto` (the default) the filter tries each name in `prefer` (default `ane`) and loads `/usr/lib/aurora-camera-denoise/cdn-backend-<name>.so`, which must export

    const struct cdn_backend_ops *cdn_backend_entry(void);

returning an ops table with `abi = CDN_BACKEND_ABI`. `backend = ane` forces it, `backend = cpu` skips it. The plugin is its own package, so the filter never links the Neural Engine runtime.

## What the ANE backend has to do

1. In `create`, open the shared Neural Engine runtime, pick the compiled model for the machine's generation and the frame size (or 64x64-class tiles with a halo, until the compiler tiles full frames), and keep the program resident.
2. In `process`, convert NV12 to the model's input layout (luma plus upsampled chroma, or luma only with the CPU chroma passes), run it, convert back, blend by `strength`.
3. Keep the previous output or frames for a temporal model (FastDVDnet class) and clear them in `reset`.
4. Return NULL from `create` on machines whose Neural Engine Linux cannot drive yet. On the MacBook Neo that is every machine today, so the Neo keeps the CPU path.

The same open weights should also run on the CPU through the same plugin, so results match whichever unit runs them.
