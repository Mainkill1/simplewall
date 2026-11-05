[CmdletBinding()]
param(
    [string]$Executable,
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs/simplewall-performance'),
    [switch]$Launch
)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
if (-not $Executable) { $Executable = Join-Path $project 'temp/public-sdk-build/simplewall.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$running = @(Get-Process simplewall -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $Destination 'simplewall.exe') })
if ($running.Count) { throw 'Close this performance build before replacing its executable.' }
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$target = Join-Path $Destination 'simplewall.exe'
$backupSuffix = Get-Date -Format yyyyMMdd-HHmmss
if (Test-Path -LiteralPath $target) {
    Copy-Item -LiteralPath $target -Destination "$target.$backupSuffix.bak"
}
Copy-Item -LiteralPath $Executable -Destination $target
$symbols = [IO.Path]::ChangeExtension($Executable, '.pdb')
$installedSymbols = Join-Path $Destination 'simplewall.pdb'
if (Test-Path -LiteralPath $symbols) {
    if (Test-Path -LiteralPath $installedSymbols) {
        Copy-Item -LiteralPath $installedSymbols -Destination "$installedSymbols.$backupSuffix.bak"
    }
    Copy-Item -LiteralPath $symbols -Destination $installedSymbols
}
foreach ($name in @('simplewall.lng','License.txt','Readme.txt','History.txt')) {
    $source = Join-Path $project "bin/$name"
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $Destination }
}
# No profile or settings files are copied over the user's existing data.
$desktop = [Environment]::GetFolderPath('Desktop')
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut((Join-Path $desktop 'simplewall (performance build).lnk'))
$link.TargetPath = $target
$link.WorkingDirectory = $Destination
$link.Description = 'simplewall with large app list performance improvements'
$link.Save()
[PSCustomObject]@{Executable=$target;SHA256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash;Shortcut=$link.FullName}
if ($Launch) {
    Start-Process -FilePath $target -WorkingDirectory $Destination -Verb RunAs
}
