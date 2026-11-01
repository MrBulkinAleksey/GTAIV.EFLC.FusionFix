# Validation on 2026-11-01

* Official AMD v2.3.0 source audit: all 73 copied C++/header files and all
  18 copied HLSL files matched commit 60f4ea81909200d8542eca14dccb2628b763a9a3
  before the two documented local patches. 96 final vendored files are hashed.
* Official Windows DXC v1.8.2502 archive downloaded; archive and three extracted
  x64 file hashes verified and pinned in fetch-dxc.ps1. PowerShell invocation
  itself has not been executed on this Linux host.
* Linux DXC v1.8.2502: all 412 permutations compiled, 100 unique containers.
  A second fresh compilation matched the generated header byte-for-byte.
  Header SHA-256: db7bf2589ac410a4e8c36ecf99a6f4f89b4f8acd6b21451199012537607f3a6b.
* Actual x64 Windows DLL cross-built and linked using Zig C++20 at O2, all
  target sources included. The only diagnostics were AMD's existing Clang
  warning-name pragmas. MSVC compilation has not run here. Premake VS2022 solution generation passed;
  the generated project has eight C++ units, all HLSL marked None, no FxCompile
  entries, and the shader/build/fetch/verification scripts included.
* DLL exports inspected: exactly OwnFiCreate, OwnFiPrepare, OwnFiGenerate,
  OwnFiDestroy. No AMD loader/provider/present export is provided.
* Wine Docker ff-wine: DLL load, all exports, invalid input return values,
  null Destroy and helper absolute-path loader smoke passed. x64 ABI v1 sizes:
  Create=48, Prepare=200, Generate=184 bytes.
* D3D12 device probe in that container returned 0x887A0004
  (DXGI_ERROR_UNSUPPORTED). No effect Create/PSO/dispatch on a GPU ran.

The built cross-compiled DLL is in bin/. Parent's shipping artifact should use
its normal Windows CI build. Rendering, output completeness, visual equivalence,
reference repeatability, performance, GPU teardown and in-game integration remain
unverified. Reset output initialization was inspected in AMD source only.
