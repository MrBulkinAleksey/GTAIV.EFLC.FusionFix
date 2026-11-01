[CmdletBinding()]
param([string]$Destination = (Join-Path $PSScriptRoot '../../build/own-frameinterpolation/dxc-v1.8.2502'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$uri = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2502/dxc_2025_02_20.zip'
$archiveHash = '70b1913a1bfce4a3e1a5311d16246f4ecdf3a3e613abec8aa529e57668426f85'
$files = @{
    'dxc.exe' = 'ae4bf100b8c64ab03cdc0ee5996d5f212e96c59d6d37340359342d714ac6c004'
    'dxcompiler.dll' = 'f79943b02f73b621421b58a569fd6ee120d0023fb6bfa23844ecad6b00d6a295'
    'dxil.dll' = '62736e99aa534a942873f92b881a71b2cb6ad62b8d7481ea7a8ad8e9888a8056'
}
function Assert-Hash([string]$Path, [string]$Expected) {
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Expected) { throw "SHA256 mismatch for $Path : $actual" }
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$Destination = (Resolve-Path -LiteralPath $Destination).Path
$archive = Join-Path $Destination 'dxc_2025_02_20.zip'
if (!(Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $uri -OutFile $archive }
Assert-Hash $archive $archiveHash
# Expand only verified bytes. No downloaded build script or executable is run.
Expand-Archive -LiteralPath $archive -DestinationPath $Destination -Force
foreach ($name in $files.Keys) { Assert-Hash (Join-Path $Destination "bin/x64/$name") $files[$name] }
$env:OWN_FI_DXC = Join-Path $Destination 'bin/x64/dxc.exe'
if ($env:GITHUB_ENV) { Add-Content -LiteralPath $env:GITHUB_ENV -Value "OWN_FI_DXC=$env:OWN_FI_DXC" -Encoding utf8 }
Write-Output "OWN_FI_DXC=$env:OWN_FI_DXC"
