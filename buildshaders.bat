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
for /R "%shaders_path%" %%a in (*.xml) do (
    RageShaderEditor.exe "%%a"
)
cd ../..

for /R "%shaders_path%" %%i in (*.fxc) do (
echo D | xcopy "%%i" "%win32_30%" /K /H /Y
)

echo D | xcopy "%cd%/shaders/GTAIV.EFLC.FusionShaders/resources" "%cd%/data/update" /K /H /Y /S

del /F /Q "%cd%\data\update\common\shaders\preload.list" 2>nul