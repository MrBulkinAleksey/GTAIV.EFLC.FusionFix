call tools\EmbedPDB\EmbedPDB.exe bin\GTAIV.EFLC.FusionFix.asi
call tools\EmbedPDB\EmbedPDB.exe bin\d3d9.dll

if not exist bin\GTAIV.EFLC.FusionFix.FrameInterpolation.dll exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "sign.ps1" -SearchPaths ".\bin\GTAIV.EFLC.FusionFix.asi .\bin\d3d9.dll .\bin\GTAIV.EFLC.FusionFix.exe .\bin\GTAIV.EFLC.FusionFix.FrameInterpolation.dll"

copy bin\GTAIV.EFLC.FusionFix.asi data\plugins\GTAIV.EFLC.FusionFix.asi
copy bin\GTAIV.EFLC.FusionFix.exe data\plugins\GTAIV.EFLC.FusionFix.exe
copy /Y bin\GTAIV.EFLC.FusionFix.FrameInterpolation.dll data\plugins\GTAIV.EFLC.FusionFix.FrameInterpolation.dll || exit /b 1
if not exist data\OwnFrameInterpolation mkdir data\OwnFrameInterpolation
copy /Y external\fidelityfx-src\LICENSE-AMD-FidelityFX-SDK.txt data\OwnFrameInterpolation\LICENSE-AMD-FidelityFX-SDK.txt || exit /b 1
copy /Y external\fidelityfx-src\LICENSE-Microsoft-D3DX12.txt data\OwnFrameInterpolation\LICENSE-Microsoft-D3DX12.txt || exit /b 1
copy /Y external\fidelityfx-src\3rdpartynotice.md data\OwnFrameInterpolation\3rdpartynotice.md || exit /b 1
copy /Y external\fidelityfx-src\provenance.json data\OwnFrameInterpolation\provenance.json || exit /b 1
copy /Y build\own-frameinterpolation\shaders.json data\OwnFrameInterpolation\shaders.json || exit /b 1
copy bin\d3d9.dll data\d3d9.dll

call buildimg.bat
call buildwtd.bat
call buildshaders.bat
call buildgxt.bat

7z a "GTAIV.EFLC.FusionFix.zip" ".\data\*" ^
-x!update\GTAIV.EFLC.FusionFix\GTAIV.EFLC.FusionFix ^
-x!update\GTAIV.EFLC.FusionFix\GTAIV.FusionFix ^
-x!update\GTAIV.EFLC.FusionFix\TLAD.FusionFix ^
-x!update\GTAIV.EFLC.FusionFix\TBOGT.FusionFix ^
-x!update\GTAIV.EFLC.FusionFix\FusionTrees ^
-x!update\GTAIV.EFLC.FusionFix\FusionLights ^
-x!update\GTAIV.EFLC.FusionFix\cdimagemake.ims ^
-xr!*\.gitkeep

copy /Y bin\Release\GTAIV.EFLC.FusionFixInstaller.exe GTAIV.EFLC.FusionFixWebInstaller.exe
start /B /WAIT GTAIV.EFLC.FusionFixWebInstaller.exe GTAIV.EFLC.FusionFix.zip
:RETRY
move /Y GTAIV.EFLC.FusionFixWebInstaller_with_GTAIV.EFLC.FusionFix.exe GTAIV.EFLC.FusionFixOfflineInstaller.exe
if ERRORLEVEL 1 (
    echo File locked, retrying...
    timeout /t 2 > nul
    goto RETRY
)

powershell -NoProfile -ExecutionPolicy Bypass -File "Sign.ps1" -SearchPaths ".\GTAIV.EFLC.FusionFixOfflineInstaller.exe .\GTAIV.EFLC.FusionFixWebInstaller.exe"

