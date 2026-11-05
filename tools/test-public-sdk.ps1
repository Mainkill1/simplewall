[CmdletBinding()]
param([string]$VcVars = 'C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$output = Join-Path $project 'temp/public-sdk-tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$mapping = Get-Content (Join-Path $PSScriptRoot 'public-sdk/routine-compat.h') |
    Where-Object { $_ -match '^#define _r_(res_queryversion|str_fromsid|listview_additem)\(' }
$mapping | Set-Content (Join-Path $output 'sdk-adapter-mappings.h') -Encoding ASCII
Copy-Item (Join-Path $project 'tests/windows-sdk-adapter.c') $output
$command = @"
@echo off
call "$VcVars" >nul
if errorlevel 1 exit /b 2
cd /d "$output"
cl /nologo /O2 /W3 /WX /TC /Fe:adapter-test.exe windows-sdk-adapter.c /link version.lib advapi32.lib comctl32.lib user32.lib
if errorlevel 1 exit /b 2
adapter-test.exe
exit /b %errorlevel%
"@
$commandPath = Join-Path $output 'test.cmd'
$command | Set-Content $commandPath -Encoding ASCII
& $env:ComSpec /c $commandPath
if ($LASTEXITCODE -ne 0) { throw 'Native SDK adapter regression failed.' }
