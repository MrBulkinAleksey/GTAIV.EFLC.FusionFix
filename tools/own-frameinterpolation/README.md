# Own D3D12 frame interpolation

Build target: `GTAIV.EFLC.FusionFix.FrameInterpolation`, separate x64 workspace
`build/GTAIV.EFLC.FusionFix.FrameInterpolation.slnx` (or `.sln`, depending on premake action).
Output: `bin/GTAIV.EFLC.FusionFix.FrameInterpolation.dll`.
No queue, fence, presentation, SR provider, signed runtime patch, or AMD swapchain is linked.
The independent four-export C ABI and helper loader are in
`source/frameinterpolation/own_frameinterpolation.h` and
`source/helper/own_frameinterpolation.hpp`.

## Provenance and licensing

AMD source is SDK **v2.3.0**, commit
`60f4ea81909200d8542eca14dccb2628b763a9a3`, from
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/60f4ea81909200d8542eca14dccb2628b763a9a3/Kits/FidelityFX .
All 73 original C++/header files and 18 HLSL files were compared byte-for-byte
against that official commit before local patches. `provenance.json` records
upstream SHA-256 hashes, local exceptions and final vendored hashes. Local
verification normalizes CRLF to LF only, so Windows checkout line endings do
not cause false source-drift failures. AMD's full
original kit license and repository third-party notice are preserved. Individual
source copyright notices, including Microsoft's d3dx12 notice, are retained.
The source subset has AMD MIT notices; the full kit license also describes other
binary-only components that this target does not build or replace.

The imported precompiled shader headers have been superseded entirely. No input
binary container or external generator is used. `compile_shaders.py` independently
compiles AMD HLSL and the local fused pass, parses DXC's resource reflection and
emits `build/own-frameinterpolation/own_shader_blobs.h` plus `shaders.json`.

## Reproducible build

Use the official DXC **v1.8.2502**, compiler commit **4789-b4711839**:
https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2502 .
Linux archive `linux_dxc_2025_02_20.x86_64.tar.gz` SHA-256:
`e0580d90dbf6053a783ddd8d5153285f0606e5deaad17a7a6452f03acdf88c71`.
Linux compiler executable SHA-256:
`0e8f751328f3526b0339b0a52e51b3b2bdb0d07bf41b6d66b1e14d0150c53a0b`.
`libdxcompiler.so` SHA-256:
`92b8cdba6ee3bd0d3db0bd4c52e37d344ea939fa8675de5eaa5382aabc486311`.
Use the matching Windows archive for a Windows build; record its own hashes rather
than substituting a different compiler version. Keep compiler notices with any
redistributed compiler tools. The compiler is not part of the game package.

From an x64 VS developer prompt, with Python 3 installed:

```
set OWN_FI_DXC=C:\tools\dxc\bin\x64\dxc.exe
python tools/own-frameinterpolation/verify_sources.py
python tools/own-frameinterpolation/build.py
```

Alternatively generate premake solutions and build the FG workspace. The target
prebuild compiles the shaders using `OWN_FI_DXC`; no unpublished AMD compiler is
required. For Linux cross compilation with Python's ziglang package:

```
uv run --with ziglang python tools/own-frameinterpolation/build.py --zig --dxc /path/to/bin/dxc
python tools/own-frameinterpolation/compile_shaders.py --dxc /path/to/bin/dxc --verify
```

There are 412 compiled combinations (32 each for 11 reference FI passes and the
fused pass, four each for seven OF passes). FI permutation bits are low-res MV,
jittered MV, inverted depth, wave64 and half; OF bits are wave64 and half. Normal
kernels target cs_6_2, wave64 variants cs_6_6. Compiler flags are O3/HLSL 2021,
16-bit types only for half permutations, no fast-math approximation added to
fusion. Byte-identical DXIL is deduplicated with matching reflected bindings.
The manifest includes compiler identity and output hash. `--verify` always runs
fresh compilations and compares the resulting header, byte for byte.

## Parent integration contract (ABI v1)

Include `own_frameinterpolation.hpp` after the existing AMD SDK headers (or with
the same API include directory). The public header consumes the helper's existing
`ffx_api_types.h`; it does not include a second copy of the SDK. The vendored
and helper API type headers were checked byte-identical for this build.

* Backend 0: existing stock behavior, unchanged signed DLL loader and fallback.
* Backend 1: `OwnFiMode::Reference`.
* Backend 2: `OwnFiMode::Fused` (implemented, compiled; visual validation pending).
* No quality/performance/guide-cap modes exist; do not expose extra choices.
* Protocol selection: OwnInterpolation = 1<<10, FusedInterpolation = 1<<11.
  Parent owns these protocol flags and their validation. Fused requires Own.
* Keep AMD helper-present on stock. Own modes support caller-controlled explicit
  Generate/game-present or a separately validated custom presenter only.

Create once with the caller device, max render/display sizes, backbuffer format,
history source format, and documented depth/HDR/MV flags. Flags exclude async:
one resource set is intentionally used with explicit retirement between frames.
The caller can retain SR overlap outside this FG context; do not overwrite its
input/output resources until corresponding GPU work has retired.

For each displayed scene slot, call `Prepare(list, p)` with its frame ID, guides,
render size, jitter/MV scale and camera snapshot. MV scale follows AMD's pixel
conversion convention; camera FOV is vertical radians and delta is milliseconds.
Supply camera position and normalized basis vectors, and view-space metre scale.
Then call `Generate(list, g)` with the SAME frame ID, output, color, optional
HUD-less color, transfer function, luminance limits and interpolation rect.
Zero rectangle width/height uses AMD's default full display rectangle. Output
must support UAV and match display dimensions. Resource structs must accurately
describe their actual entry state; AMD restores registered resource states.
Rebind D3D12 heaps/root state after each call before recording unrelated work.

All calls per context are serial. Before the next Prepare, retire ALL previous
commands, including Prepare-only submissions. Prepare replaces an abandoned
prepared frame, invalidates history and seeds a new history on next Generate.
An ID gap or render-size change also invalidates history; reset/camera-cut must
be passed explicitly. Create a fresh context for display/format/backend changes.
There is no independent cancellation export: abandon a prepared frame by retiring
its work and replacing it with the next Prepare. If ANY recorded list is discarded,
destroy the context after prior submissions retire; do not reuse CPU state that
assumes those GPU commands executed.

Return handling:

* `OwnFiOk` (0): submit/fence work, output is an interpolation candidate.
* `OwnFiNoFrame` (1): SUCCESSFUL recording. Submit/fence work to initialize/update
  histories, suppress ShowGenerated. Never mark this as permanent FG failure.
  AMD's interpolation reset path writes current-source RGB across the output;
  its normal inpainting/HUD composition follows. Alpha is AMD's internal inpaint
  weight, not a promise of opaque presentation alpha. This is source-verified,
  not a GPU visual test.
* Negative: reject/failure. Do not present generated output. A backend failure
  latches the context unusable; retire earlier submissions and destroy it. If
  Prepare shares a list with SR, a failure can invalidate that combined list:
  parent should isolate FG recording or reconfigure/retry a stock scene frame
  without claiming the discarded SR work completed.

The loader resolves all four exports from an absolute sibling DLL path using
system/DLL-directory search restrictions. It never binds the signed runtime's
exports. Destroy only after GPU retirement; the loader destructor has the same
requirement. Module lifetime must exceed all contexts and recorded GPU work.

## Fusion and validation limits

Reference preserves AMD's separate reconstruct-depth and game-vector passes.
Fused omits the separate dispatch and performs `ReconstructPrevDepth` from the
same loaded depth/MV pair inside the game-vector function. The original depth
bilinear weights/thresholds, depth atomics, vector priorities and secondary loop
remain. Both passes read the same guides/distortion; the vector pass never reads
the reconstructed depth UAV, and its consumers execute later with normal backend
UAV barriers. The common loads are removed; no guide cap or flow approximation
is made. Padding-thread behavior is left as AMD implements it.

The secondary-vector loop already reads atomic return values. Removing a dispatch
can change interleavings/occupancy; mathematical preservation is NOT a proof of
bit identity or visual equivalence. Replay must establish reference-vs-reference
repeatability before interpreting reference-vs-fused differences. Test SDR/HDR,
odd/resized render sizes, display MV, jitter/inverted depth, cuts, dropped Generate,
and actual wave32/wave64 GPUs. Capture intermediate depth and vector UAVs as well
as final frames; compare output and timings. No GPU speedup or game correctness
is claimed from compilation or the loader smoke test.

## Packaging handoff

Parent owns release/CI scripts. Add the FG workspace build and place
`GTAIV.EFLC.FusionFix.FrameInterpolation.dll` beside the helper executable. Include
`external/fidelityfx-src/LICENSE-AMD-FidelityFX-SDK.txt`,
`LICENSE-Microsoft-D3DX12.txt`, `3rdpartynotice.md`, and
`provenance.json` in an `OwnFrameInterpolation` notices directory. Keep existing
signed stock runtime files and notices. Include `shaders.json` in build artifacts
for compiler/permutation traceability; generated C++ headers are not game assets.

`smoke.cpp` validates DLL loading, all exports, invalid inputs and the helper
loader. It requires the normal helper AMD API include directory when compiled.
It creates no GPU device and is not a rendering test.

### Windows CI fetch

```
pwsh -File tools/own-frameinterpolation/fetch-dxc.ps1
```

The script verifies the official archive and the x64 compiler, dxcompiler and
dxil files, then sets `OWN_FI_DXC` in the current process and in `GITHUB_ENV`.
In a separate local PowerShell process use the printed path (or dot-source the
script so its environment change remains in your shell). A GitHub Actions step
can invoke it normally; subsequent steps get the environment via `GITHUB_ENV`.
Official Windows archive URL:
https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2502/dxc_2025_02_20.zip
SHA-256 `70b1913a1bfce4a3e1a5311d16246f4ecdf3a3e613abec8aa529e57668426f85`.
The fetch does not execute any downloaded file. Compile only through the local
build target/script. Do not ship this compiler in the game package.
