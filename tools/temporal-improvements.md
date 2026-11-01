# Temporal rendering experiments

The Advanced graphics menu exposes the experimental policies below. The
corresponding INI keys live in `[ANTIALIASING]`; their defaults preserve the
current-frame upscaler and signed AMD frame-generation runtime.

| Key | Default | Behavior |
| --- | --- | --- |
| `VehicleDamageMotionVectors` | 1 | Replay the current vehicle damage field when drawing its motion vectors. Requires a restart. |
| `WheelMotionStabilization` | 0 | Remove wheel spin from the previous pose while retaining steering and hub translation. |
| `SteadyDepthHold` | 0 | Compare the nearer-surface hold policy with the existing depth blend. |
| `UpscalerPipelined` | 0 | Show the preceding SR result while processing the current frame. Adds one frame of scene latency. |
| `FrameGenerationAdaptivePacing` | 0 | Use observed GPU readiness to expire late generated frames without moving the real-frame deadline. Applies to deferred game presentation. |
| `FrameGenerationProtectOverlays` | 1 | Suppress generation while the phone is visible. A missing same-frame HUD-less capture also suppresses generation. |
| `FrameGenerationBackend` | 0 | 0: signed AMD runtime; 1: own reference build; 2: own build with the depth/vector pass fused. |

## Previous-frame SR

Three independent shared input/output sets keep the frame being evaluated
separate from the displayed frame. Their companion logarithmic depth, rain
layer, cloud mask and camera follow the displayed scene. The HUD remains current. Current
inputs must be copied before displaying the old result, since the scene and
output may be the same texture.

Cuts, changes of depth range, size, backend, helper lifetime or pipeline mode
discard the old history. Startup and fallback use the current result. This
mode consumes additional video memory and may not improve performance when
the helper uses CPU synchronization. Frame generation and helper presentation
are disabled while this experiment is active; the log identifies that choice.

## Frame generation

The own library records work on the helper's existing command list. It owns
neither a queue nor a swapchain. Selecting it uses the custom presenter instead
of AMD's swapchain. The signed runtime remains the fallback.

GPU readiness samples measure the interval from handoff to observed completion,
including queueing and polling. They are not execution-time measurements.
Missing frames are dropped without inventing a timing sample. Phone protection
currently skips interpolation rather than providing an exact per-pixel phone
coverage mask.

## Validation required on the target machine

Portable tests and shader compilation check the algorithms and build inputs.
They cannot establish image equivalence, latency improvements or performance
on the player's GPU. Compare the signed runtime, own reference, and fused mode
on the same captured inputs before making a visual-equivalence claim.

In game, compare damaged cars, spinning/steered wheels, phone open/close,
rain transitions, thin silhouettes and depth of field. Include camera cuts,
resolution changes, DLAA/FSR switches, SDR/HDR and helper failure. Measure
frame intervals, scene latency and video memory as well as FPS.

Vehicle replay uses the current damage field for both poses. It does not
reconstruct the historical deformation of a dent created between frames.

Portable checks:

```
uv run --with ziglang python -m ziglang c++ -std=c++20 -fshort-wchar -Isource tests/*.cpp -o /tmp/temporal-tests
/tmp/temporal-tests
python tools/temporal_negative_controls.py --compiler "uv run --with ziglang python -m ziglang c++"
```
