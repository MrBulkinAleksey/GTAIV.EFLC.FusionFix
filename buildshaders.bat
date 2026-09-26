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
rem .fxc behind, which crashes the game, so its output is checked here instead.
for /R "%shaders_path%" %%a in (*.xml) do (
    RageShaderEditor.exe "%%a" < nul > "%TEMP%\RageShaderEditor.log" 2>&1
    type "%TEMP%\RageShaderEditor.log"
    findstr /C:"Error when assembling" "%TEMP%\RageShaderEditor.log" >nul && (
        echo Shader assembly failed for %%a
        exit 1
    )
)
cd ../..

for /R "%shaders_path%" %%i in (*.fxc) do (
echo D | xcopy "%%i" "%win32_30%" /K /H /Y
)

echo D | xcopy "%cd%/shaders/GTAIV.EFLC.FusionShaders/resources" "%cd%/data/update" /K /H /Y /S

del /F /Q "%cd%\data\update\common\shaders\preload.list" 2>nul