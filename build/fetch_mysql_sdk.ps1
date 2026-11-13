[CmdletBinding()]
param([string]$Archive)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$specification = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json).mysql
$dependencies = Join-Path $repository 'Artifacts/Dependencies'
$sdk = Join-Path $dependencies $specification.directory
$marker = Join-Path $sdk '.archive-sha256'
$required = @('include/mysql.h', 'include/mysql_version.h', 'lib/libmysql.lib',
              'lib/libmysql.dll', 'bin/libssl-3-x64.dll', 'bin/libcrypto-3-x64.dll', 'LICENSE')
$complete = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $sdk $_) -PathType Leaf) }).Count -eq 0
if ($complete -and (Test-Path -LiteralPath $marker) -and
    (Get-Content -LiteralPath $marker -Raw).Trim() -eq $specification.sha256) {
    & (Join-Path $PSScriptRoot 'check_mysql_sdk.ps1') -MySqlRoot $sdk
    Write-Output "Verified MySQL SDK cache: $sdk"
    return
}

New-Item -ItemType Directory -Path $dependencies -Force | Out-Null
if (-not $Archive) {
    $Archive = Join-Path $dependencies ($specification.directory + '.zip')
    if (-not (Test-Path -LiteralPath $Archive) -or
        (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $specification.sha256) {
        & curl.exe --fail --location --retry 3 --connect-timeout 30 --max-time 600 `
            --output $Archive $specification.url
        if ($LASTEXITCODE) { throw 'MySQL SDK download failed.' }
    }
}
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $specification.sha256) {
    throw 'The MySQL archive does not match the pinned SHA-256.'
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Archive).Path)
try {
    $prefix = $specification.directory + '/'
    foreach ($entry in $zip.Entries) {
        if (-not $entry.FullName.StartsWith($prefix, [StringComparison]::Ordinal)) { continue }
        $relative = $entry.FullName.Substring($prefix.Length)
        if (-not $relative -or $entry.FullName.EndsWith('/')) { continue }
        if ($relative -notlike 'include/*' -and $relative -notin $required) { continue }
        $target = [IO.Path]::GetFullPath((Join-Path $sdk $relative))
        $sdkPrefix = [IO.Path]::GetFullPath($sdk).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        if (-not $target.StartsWith($sdkPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Unexpected path in the MySQL archive.'
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    }
} finally {
    $zip.Dispose()
}
if (@($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $sdk $_) -PathType Leaf) }).Count) {
    throw 'The MySQL archive is missing required SDK files.'
}
& (Join-Path $PSScriptRoot 'check_mysql_sdk.ps1') -MySqlRoot $sdk
[IO.File]::WriteAllText($marker, $specification.sha256 + "`n", [Text.UTF8Encoding]::new($false))
Write-Output "Prepared verified MySQL $($specification.version) SDK: $sdk"
