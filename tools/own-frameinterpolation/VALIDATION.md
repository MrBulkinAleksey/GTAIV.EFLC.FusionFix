# Validation

## Move to SDK v2.4.0, 2026-11-01

* The vendored subset was re-taken from the official v2.4.0 tree (commit
  7539ae8b6568ad29ed78a753404351daf3b5cca7): 63 files were unchanged from
  v2.3.0 and kept, 22 changed upstream and were replaced whole, and the 6
  files carrying this repository's edits were merged three-way (their local
  edits re-applied over the 2.4.0 text; two conflicts in
  ffx_frameinterpolation.cpp resolved: upstream's own cast of the permutation
  lookup, and this repository's removal of the reconstruct-previous-depth
  dispatch). The two shader-build .bat files upstream no longer has were
  dropped; the AMD license and the third-party notice were refreshed from the
  2.4.0 tree; provenance.json records the new tag, commit, upstream hashes
  and final vendored hashes, and verify_sources.py passes over 94 files.
* All 380 permutations (11 FI passes x 32, 7 OF passes x 4) compile with a
  development DXC 1.9 on Linux; the pinned 1.8.2502 compilation and its
  byte-compare run in CI (fetch-dxc.ps1, compile_shaders.py --verify).
* The library cross-builds for x64 Windows with Zig C++20 at O2; MSVC
  compilation runs in CI.
* Not verified here: MSVC/pinned-DXC artifacts, any GPU run, image
  equivalence, performance. The 2.3.0-to-2.4.0 upstream changes to the
  frame interpolation passes (setup, optical flow vector field, inpainting
  pyramid, private headers, provider) are AMD's own and unreviewed beyond
  the merge; in-game testing covers them.

## Initial build on SDK v2.3.0, 2026-11-01 (historical)

* Official AMD v2.3.0 source audit: all 73 copied C++/header files and all
  18 copied HLSL files matched commit 60f4ea81909200d8542eca14dccb2628b763a9a3
  before the two documented local patches. 96 final vendored files were hashed.
* Official Windows DXC v1.8.2502 archive downloaded; archive and three extracted
  x64 file hashes verified and pinned in fetch-dxc.ps1.
* Linux DXC v1.8.2502: all 412 permutations (then including the fused variant)
  compiled; a second fresh compilation matched the generated header byte for byte.
* x64 Windows DLL cross-built with Zig C++20 at O2; exports exactly OwnFiCreate,
  OwnFiPrepare, OwnFiGenerate, OwnFiDestroy; Wine smoke: load, exports, invalid
  inputs, null Destroy, helper loader. No GPU run (device probe 0x887A0004).
