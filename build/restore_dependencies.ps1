[CmdletBinding()]
param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$MySqlRoot = $env:MySqlRoot
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$dependencyRoot = Join-Path $repository 'Artifacts\Dependencies'
if (-not $VcpkgRoot) {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -version '[17.0,18.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($installation) { $VcpkgRoot = Join-Path $installation 'VC\vcpkg' }
}
if (-not $VcpkgRoot) { throw 'Set VcpkgRoot to the vcpkg checkout used with v143.' }
$packageManager = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $packageManager)) { throw "vcpkg.exe missing in $VcpkgRoot" }
$installRoot = Join-Path $dependencyRoot 'vcpkg'
$triplets = Join-Path $repository 'triplets'
& $packageManager install --triplet x64-windows-static-md "--x-manifest-root=$repository" "--x-install-root=$installRoot" "--overlay-triplets=$triplets"
if ($LASTEXITCODE) { throw "vcpkg restore failed ($LASTEXITCODE)." }

if (-not $MySqlRoot) { $MySqlRoot = Join-Path $dependencyRoot 'mysql-8.4.11-winx64' }
foreach ($entry in @('include\mysql.h','lib\libmysql.lib','lib\libmysql.dll','bin\libssl-3-x64.dll','bin\libcrypto-3-x64.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $MySqlRoot $entry) -PathType Leaf)) {
        throw "Extract the Oracle MySQL 8.4.11 Windows x64 ZIP or set MySqlRoot. Missing $entry"
    }
}
Write-Output "Native dependencies ready at $dependencyRoot"
