# Build both halves of the monitor sandwich (pre + post) in one go.
# Each .sln produces a separate DLL via $(SolutionName); they share the
# same vcxproj and the same OutDir, so post-build outputs land side-by-side
# in bin\<Platform>\<Configuration>\.
#
# Usage from the repo root:
#     pwsh scripts\Build-All.ps1                          # Release|x64
#     pwsh scripts\Build-All.ps1 -Configuration Debug
#     pwsh scripts\Build-All.ps1 -Platform Win32          # 32-bit pair
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

$msbuild = (Get-Command msbuild -ErrorAction SilentlyContinue)?.Source
if (-not $msbuild) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -property installationPath
        $msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
    }
}
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    throw 'MSBuild not found. Install Visual Studio 2019+ with C++ workload, or add msbuild to PATH.'
}

foreach ($sln in @('XR_APILAYER_MLEDOUR_layer_monitor_pre.sln',
                    'XR_APILAYER_MLEDOUR_layer_monitor_post.sln')) {
    $path = Join-Path $repoRoot $sln
    Write-Host "==> Building $sln ($Configuration|$Platform)" -ForegroundColor Cyan
    & $msbuild $path /m /p:Configuration=$Configuration /p:Platform=$Platform /nologo
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for $sln (exit $LASTEXITCODE)"
    }
}

Write-Host "==> Outputs in $repoRoot\bin\$Platform\$Configuration" -ForegroundColor Green
