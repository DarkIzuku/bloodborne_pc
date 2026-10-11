# Builds the Windows package from this repository: the runtime (build.sh in MSYS2 CLANG64) and
# BloodborneLauncher.exe. Optionally upgrades an install in place and makes a portable zip.
#
#   .\launcher-windows\build-launcher.ps1                                    build only
#   .\launcher-windows\build-launcher.ps1 -InstallDir D:\Bloodborne          also upgrade that install
#   .\launcher-windows\build-launcher.ps1 -InstallDir D:\Bloodborne -Zip     also make a portable zip
#   -SkipRuntime: reuse the runtime already in out\   -SkipTests: skip the launcher tests
#
# Everything comes from the repository build. The install only provides what the repository
# does not have: tools\python, fsr4_shaders and out\nvngx_dlss.dll.
# Without arguments (e.g. from build-launcher.bat) it asks for the install folder.
param([string]$InstallDir, [switch]$Zip, [switch]$SkipTests, [switch]$SkipRuntime)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$project = Join-Path $PSScriptRoot 'BloodborneLauncher\BloodborneLauncher.csproj'
$dist = Join-Path $repo 'dist'
$msysBash = 'C:\msys64\usr\bin\bash.exe'
$runtimeExes = 'bb-probe.exe', 'bb-gpu-capabilities.exe', 'bb-engine-assets.exe'

function Find-Sdk {
    $onPath = Get-Command dotnet -ErrorAction SilentlyContinue
    foreach ($dotnet in @($(if ($onPath) { $onPath.Source }), "$env:ProgramFiles\dotnet\dotnet.exe")) {
        if ($dotnet -and (Test-Path $dotnet) -and (& $dotnet --list-sdks | Where-Object { [int]($_ -split '\.')[0] -ge 8 })) { return $dotnet }
    }
}
function Invoke-Msys([string]$command) {
    $env:MSYSTEM = 'CLANG64'
    $repoUnix = '/' + $repo.Substring(0, 1).ToLower() + ($repo.Substring(2) -replace '\\', '/')
    & $msysBash -lc "cd '$repoUnix' && $command"
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 command failed: $command" }
}
# Copies one directory's contents into another, merging with what is there.
function Copy-Tree([string]$from, [string]$to) {
    New-Item -ItemType Directory -Force $to | Out-Null
    Copy-Item (Join-Path $from '*') $to -Recurse -Force
}
# Runtime, scripts and patches from the repository build into $dest (an install or a package).
function Copy-Runtime([string]$dest) {
    New-Item -ItemType Directory -Force "$dest\out" | Out-Null
    foreach ($exe in $runtimeExes) { Copy-Item "$repo\out\$exe" "$dest\out" -Force }
    foreach ($dll in $runtimeDlls) { Copy-Item $dll "$dest\out" -Force }
    Copy-Item "$PSScriptRoot\BloodborneLauncher\Assets\bloodborne.ico" "$dest\out" -Force
    foreach ($f in 'run.bat', 'build.sh', 'setup.bat', 'LICENSE', 'README.md') { Copy-Item "$repo\$f" $dest -Force }
    foreach ($d in 'scripts', 'patches', 'licenses') { Copy-Tree "$repo\$d" "$dest\$d" }
    # The shipped MSYS2 DLLs' licenses, as in CI.
    Copy-Tree 'C:\msys64\clang64\share\licenses' "$dest\licenses\msys2"
}

$dotnet = Find-Sdk
if (-not $dotnet) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw 'Install the .NET 8 SDK: https://dotnet.microsoft.com/download/dotnet/8.0' }
    if ((Read-Host 'The .NET 8 SDK is needed. Install it now with winget? (y/n)') -ne 'y') { exit 1 }
    winget install --id Microsoft.DotNet.SDK.8 --accept-package-agreements --accept-source-agreements
    $dotnet = Find-Sdk
    if (-not $dotnet) { throw '.NET 8 SDK still not found; open a new terminal and run this again.' }
}
# Verify-Launcher.ps1 calls plain "dotnet".
$env:PATH = (Split-Path $dotnet) + ';' + $env:PATH
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
if (-not (Test-Path $msysBash)) { throw 'MSYS2 is needed for the runtime (C:\msys64, see README "Windows").' }

if (-not $PSBoundParameters.ContainsKey('InstallDir')) {
    $InstallDir = Read-Host 'Install folder to upgrade, e.g. D:\Bloodborne (empty = only build)'
    if ($InstallDir -and -not $PSBoundParameters.ContainsKey('Zip')) { $Zip = (Read-Host 'Also make a portable zip? (y/n)') -eq 'y' }
}
if ($InstallDir -and -not (Test-Path (Join-Path $InstallDir 'run.bat'))) { throw "$InstallDir is not a Bloodborne PC install (no run.bat)." }
if ($Zip -and -not $InstallDir) { throw '-Zip needs -InstallDir: the package takes tools\python and fsr4_shaders from that install.' }

if (-not $SkipRuntime) {
    Write-Host '== Building the runtime (build.sh)'
    Invoke-Msys './build.sh'
}
foreach ($exe in $runtimeExes) { if (-not (Test-Path "$repo\out\$exe")) { throw "out\$exe is missing: run without -SkipRuntime." } }
# The MSYS2 DLLs the runtime programs load, as in CI (windows-offline-build.yml).
$runtimeDlls = Invoke-Msys ('for e in ' + (($runtimeExes | ForEach-Object { "out/$_" }) -join ' ') +
    '; do ldd "$e"; done | awk ''{print $3}'' | grep -iE ''/clang64/bin/.*\.dll$'' | sort -u | xargs -r cygpath -w')

if (-not $SkipTests) {
    Write-Host '== Testing the launcher'
    & (Join-Path $PSScriptRoot 'BloodborneLauncher\Verify-Launcher.ps1')
}

Write-Host '== Building BloodborneLauncher.exe'
# nuget.org is passed explicitly so the build works even without a configured package source.
& $dotnet publish $project -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true --source https://api.nuget.org/v3/index.json -o $dist
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$exe = Join-Path $dist 'BloodborneLauncher.exe'
Write-Host "Built $exe"

if ($InstallDir) {
    $InstallDir = (Resolve-Path $InstallDir).Path
    if (Get-Process BloodborneLauncher, bb-probe -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$InstallDir\*" }) {
        throw 'Close the launcher and the game in the install folder first.'
    }
    Write-Host "== Upgrading $InstallDir"
    # Previous programs are kept as .bak to go back; settings, saves and the game are not touched.
    foreach ($file in @('BloodborneLauncher.exe') + ($runtimeExes | ForEach-Object { "out\$_" })) {
        if (Test-Path "$InstallDir\$file") { Copy-Item "$InstallDir\$file" "$InstallDir\$file.bak" -Force }
    }
    Copy-Item $exe $InstallDir -Force
    Copy-Runtime $InstallDir
    Write-Host 'Upgraded (previous programs kept as *.exe.bak)'
}

if ($Zip) {
    Write-Host '== Packaging'
    $version = (Get-Date -Format 'yyyy-MM-dd') + ((git -C $repo rev-parse --short HEAD 2>$null) | ForEach-Object { "-$_" })
    $stage = Join-Path ([IO.Path]::GetTempPath()) "bbpc-package-$([Guid]::NewGuid().ToString('N'))"
    $pkg = Join-Path $stage 'Bloodborne-PC'
    try {
        New-Item -ItemType Directory -Force "$pkg\mods", "$pkg\logs" | Out-Null
        Copy-Item $exe $pkg
        Copy-Runtime $pkg
        # Not in the repository: taken from the install. fsr4_411 is left out: it is built from
        # the user's own AMD DLL.
        foreach ($d in 'fsr4_shaders', 'tools') { if (Test-Path "$InstallDir\$d") { Copy-Tree "$InstallDir\$d" "$pkg\$d" } }
        if (Test-Path "$InstallDir\out\nvngx_dlss.dll") { Copy-Item "$InstallDir\out\nvngx_dlss.dll" "$pkg\out" }
        Get-ChildItem $pkg -Recurse -Directory -Filter __pycache__ | Remove-Item -Recurse -Force
        Set-Content -Encoding utf8 "$pkg\README-FIRST.txt" @'
Bloodborne PC - portable Windows package

1. Put your own Bloodborne v1.09 dump (the CUSA03173 folder with eboot.bin and sce_sys)
   next to BloodborneLauncher.exe:
      Bloodborne-PC\BloodborneLauncher.exe
      Bloodborne-PC\CUSA03173\eboot.bin
   The launcher finds it automatically. A dump elsewhere can be chosen with the folder button.
2. Run BloodborneLauncher.exe, check the settings and press PLAY BLOODBORNE.

Upgrading: extract over an existing install. Your game, saves (user\), bbport.ini and
launcher-settings.json are not in this package, so they are kept.
This package contains no game files.
'@
        $zipPath = Join-Path $dist "Bloodborne-PC-$version.zip"
        if (Test-Path $zipPath) { Remove-Item $zipPath }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
        Write-Host ("Packaged {0} ({1:N0} MB)" -f $zipPath, ((Get-Item $zipPath).Length / 1MB))
    }
    finally { Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
