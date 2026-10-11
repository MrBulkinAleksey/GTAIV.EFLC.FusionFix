@echo on
set shaders_path=%cd%/shaders
set win32_30=%cd%/data/update/common/shaders/win32_30

rem FusionFix changes to the shader submodule live as patches in shaders/patches and are
rem applied here, so the submodule itself stays on its upstream commit. A patch that is
rem already applied is skipped; one that no longer applies stops the build.
for %%p in ("%shaders_path%\patches\*.patch") do (
    git -C "%shaders_path%/GTAIV.EFLC.FusionShaders" apply --reverse --check "%%p" >nul 2>&1 || (
        git -C "%shaders_path%/GTAIV.EFLC.FusionShaders" apply "%%p" || (
            echo Failed to apply shader patch %%p
            exit 1
        )
    )
)

cd tools/RageShaderEditor
rem RageShaderEditor reports assembly errors but still exits normally and leaves a broken
rem .fxc behind, which crashes the game, so its output is checked here instead. It prints
rem nothing on success; any error, exception or pause prompt fails the build, and the
rem offending lines are repeated as GitHub annotations.
for /R "%shaders_path%" %%a in (*.xml) do (
    RageShaderEditor.exe "%%a" < nul > "%TEMP%\RageShaderEditor.log" 2>&1
    type "%TEMP%\RageShaderEditor.log"
    findstr /I /C:"error" /C:"exception" /C:"press any key" "%TEMP%\RageShaderEditor.log" >nul && (
        for /f "usebackq delims=" %%l in (`findstr /I /C:"error" /C:"exception" "%TEMP%\RageShaderEditor.log"`) do echo ::error title=Shader assembly::%%l
        echo Shader assembly failed for %%a
        exit 1
    )
)

rem Every shader description must have produced its compiled shader.
for /R "%shaders_path%" %%a in (*.fxc.xml) do (
    if not exist "%%~dpna" (
        echo ::error title=Shader assembly::No compiled shader was produced for %%a
        exit 1
    )
)
cd ../..

rem The game reads a shader file into a buffer of 0x2C000 bytes and does not check its length: deferred_lighting.fxc
rem at 180494 bytes broke the game's heap, which stopped it with SMPA50 when the world was torn down on exit.
for /R "%shaders_path%" %%a in (*.fxc) do (
    if %%~za GTR 180224 (
        echo ::error title=Shader size::%%~nxa is %%~za bytes, more than the 180224 the game reads
        exit 1
    )
)

for /R "%shaders_path%" %%i in (*.fxc) do (
echo D | xcopy "%%i" "%win32_30%" /K /H /Y
)

echo D | xcopy "%cd%/shaders/GTAIV.EFLC.FusionShaders/resources" "%cd%/data/update" /K /H /Y /S

del /F /Q "%cd%\data\update\common\shaders\preload.list" 2>nul