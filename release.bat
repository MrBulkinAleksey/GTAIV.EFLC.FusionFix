call tools\EmbedPDB\EmbedPDB.exe bin\GTAIV.EFLC.FusionFix.asi
call tools\EmbedPDB\EmbedPDB.exe bin\d3d9.dll

powershell -NoProfile -ExecutionPolicy Bypass -File "sign.ps1" -SearchPaths ".\bin\GTAIV.EFLC.FusionFix.asi .\bin\d3d9.dll .\bin\GTAIV.EFLC.FusionFix.exe"

if not exist data\plugins mkdir data\plugins
copy bin\GTAIV.EFLC.FusionFix.asi data\plugins\GTAIV.EFLC.FusionFix.asi || exit /b 1
rem The 64-bit helper running DLAA, FSR and frame generation, next to the plugin (source/helper)
copy bin\GTAIV.EFLC.FusionFix.exe data\plugins\GTAIV.EFLC.FusionFix.exe || exit /b 1
rem FusionFixHub.exe ships with the fix, at the game's root (updates, settings editor, DLAA/FSR). HUB_EXE picks it;
rem the workflow downloads the signed release next to this script, a local release takes the hub's own build.
if not defined HUB_EXE if exist FusionFixHub.exe set "HUB_EXE=FusionFixHub.exe"
if not defined HUB_EXE set "HUB_EXE=%~dp0..\FusionFixHub-App\bin\Release\FusionFixHub.exe"
if exist "%HUB_EXE%" (
    copy /y "%HUB_EXE%" data\FusionFixHub.exe
) else (
    rem Not needed: our helper runs DLAA and FSR, before_packaging.bat puts DXVK and the vendor runtimes into data
    echo No FusionFixHub.exe: the package is built without the hub
)
rem d3d9.dll is only a pass-through to the system's d3d9.dll (source/d3d9)
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

exit /b 0
