[CmdletBinding()]
param([string]$VcVars='C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
$output=Join-Path $project 'temp/packet-history-tests'
New-Item -ItemType Directory -Path $output -Force|Out-Null
$command=@"
@echo off
call "$VcVars" >nul
if errorlevel 1 exit /b 2
cd /d "$output"
cl /nologo /O2 /std:c11 /DSQLITE_OMIT_LOAD_EXTENSION /I"$project\src" /I"$project\vendor\sqlite" "$project\tests\packet-store-test.c" "$project\src\packet_store.c" "$project\vendor\sqlite\sqlite3.c" /Fe:packet-store-test.exe
if errorlevel 1 exit /b 2
packet-store-test.exe "$output\native-history.db"
if errorlevel 1 exit /b 2
cl /nologo /O2 /std:c11 /I"$project\src" "$project\tests\packet-queue-test.c" /Fe:packet-queue-test.exe
if errorlevel 1 exit /b 2
packet-queue-test.exe
exit /b %errorlevel%
"@
$path=Join-Path $output 'test.cmd'
$command|Set-Content $path -Encoding ASCII
Push-Location $output
try{& $env:ComSpec /c $path;if($LASTEXITCODE -ne 0){throw 'Native packet history tests failed'}}finally{Pop-Location}
