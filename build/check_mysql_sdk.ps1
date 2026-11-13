[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$MySqlRoot
)

$ErrorActionPreference = 'Stop'
$header = Join-Path $MySqlRoot 'include\mysql_version.h'
$runtime = Join-Path $MySqlRoot 'lib\libmysql.dll'
$headerVersion = Select-String -LiteralPath $header -Pattern '^\s*#define\s+LIBMYSQL_VERSION_ID\s+80411\s*$'
if (-not $headerVersion -or (Get-Item -LiteralPath $runtime).VersionInfo.FileVersion -ne '8.4.11.0') {
    throw 'MySqlRoot must contain the MySQL 8.4.11 x64 client headers, library and DLL.'
}
