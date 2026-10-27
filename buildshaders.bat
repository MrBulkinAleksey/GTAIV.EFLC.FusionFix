@echo on
set shaders_path=%cd%/shaders
set win32_30=%cd%/data/update/common/shaders/win32_30

rem The shaders are built from the submodule and shaders/external only: upstream's in-repo copy
rem (GTAIV.EFLC.FusionShaders-temp) is kept for merging, its motion vector changes reach the build as
rem shaders/patches/aa_motion_vectors.patch.
set fusion_shaders=%shaders_path%/GTAIV.EFLC.FusionShaders
set external_shaders=%shaders_path%/external

rem FusionFix changes to the shader submodule live as patches in shaders/patches and are
rem applied here, so the submodule itself stays on its upstream commit. A patch that is
rem already applied is skipped; one that no longer applies stops the build.
for %%p in ("%shaders_path%\patches\*.patch") do (
    git -C "%fusion_shaders%" apply --reverse --check "%%p" >nul 2>&1 || (
        git -C "%fusion_shaders%" apply "%%p" || (
            echo Failed to apply shader patch %%p
            exit 1
        )
    )
)

cd tools/RageShaderEditor
call :assemble "%fusion_shaders%" || exit 1
call :assemble "%external_shaders%" || exit 1
cd ../..

call :copy "%fusion_shaders%" || exit 1
call :copy "%external_shaders%" || exit 1

echo D | xcopy "%fusion_shaders%/resources" "%cd%/data/update" /K /H /Y /S

del /F /Q "%cd%\data\update\common\shaders\preload.list" 2>nul
exit /b 0

rem RageShaderEditor reports assembly errors but still exits normally and leaves a broken
rem .fxc behind, which crashes the game, so its output is checked here instead. It prints
rem nothing on success; any error, exception or pause prompt fails the build, and the
rem offending lines are repeated as GitHub annotations.
:assemble
for /R "%~1" %%a in (*.xml) do (
    RageShaderEditor.exe "%%a" < nul > "%TEMP%\RageShaderEditor.log" 2>&1
    type "%TEMP%\RageShaderEditor.log"
    findstr /I /C:"error" /C:"exception" /C:"press any key" "%TEMP%\RageShaderEditor.log" >nul && (
        for /f "usebackq delims=" %%l in (`findstr /I /C:"error" /C:"exception" "%TEMP%\RageShaderEditor.log"`) do echo ::error title=Shader assembly::%%l
        echo Shader assembly failed for %%a
        exit /b 1
    )
)

rem Every shader description must have produced its compiled shader.
for /R "%~1" %%a in (*.fxc.xml) do (
    if not exist "%%~dpna" (
        echo ::error title=Shader assembly::No compiled shader was produced for %%a
        exit /b 1
    )
)
exit /b 0

rem 180224 bytes (0x2C000) was taken for a buffer the game reads shaders into; the loader (CE 0x435770) has no such
rem buffer, every part is allocated by the sizes in the file, and a deferred_lighting.fxc of some 184.5 KB ran fine
rem (2026-10-15): up to 240 KB.
:copy
for /R "%~1" %%a in (*.fxc) do (
    if %%~za GTR 245760 (
        echo ::error title=Shader size::%%~nxa is %%~za bytes, more than the 245760 tried so far
        exit /b 1
    )
)
for /R "%~1" %%i in (*.fxc) do (
echo D | xcopy "%%i" "%win32_30%" /K /H /Y
)
exit /b 0
