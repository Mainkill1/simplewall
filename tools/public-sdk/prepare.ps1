[CmdletBinding()]
param([string]$RoutineRoot)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $RoutineRoot) { $RoutineRoot = Join-Path $projectPath 'temp/deps/routine' }
$RoutineRoot = (Resolve-Path -LiteralPath $RoutineRoot).Path
$sourcePath = Join-Path $RoutineRoot 'src/routine.c'
$expectedHash = 'B94D44652C4D82CDDBEBD284C0E572EF950718B19E0B519071925254998BA588'
$hasher = [Security.Cryptography.SHA256]::Create()
try { $sourceHash = [BitConverter]::ToString($hasher.ComputeHash([IO.File]::ReadAllBytes($sourcePath))).Replace('-','') }
finally { $hasher.Dispose() }
if ($sourceHash -ne $expectedHash) {
    throw 'This adapter requires the unmodified public routine.c from v.2.7.11 (also present at 3020ca9). No SDK files were changed.'
}
$preparedRoot = Join-Path $projectPath 'temp/public-sdk-source'
$preparedSource = Join-Path $preparedRoot 'src'
New-Item -ItemType Directory -Path $preparedSource -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $RoutineRoot 'src') -File | Copy-Item -Destination $preparedSource
Copy-Item -LiteralPath (Join-Path $RoutineRoot 'LICENSE') -Destination $preparedRoot
$sourceText = [IO.File]::ReadAllText($sourcePath)
$pattern = '(?ms)^ULONG_PTR _r_str_getlength_ex \(.*?(?=^BOOLEAN _r_str_isequal \()'
$matchesFound = [regex]::Matches($sourceText,$pattern)
if ($matchesFound.Count -ne 1) { throw 'Could not locate the expected SDK length implementation.' }
$replacement = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'string-length.c'))
$patched = $sourceText.Substring(0,$matchesFound[0].Index) + $replacement + "`r`n" + $sourceText.Substring($matchesFound[0].Index + $matchesFound[0].Length)
[IO.File]::WriteAllText((Join-Path $preparedSource 'routine.c'),$patched,[Text.UTF8Encoding]::new($false))
Write-Output $preparedRoot
