[CmdletBinding()]
param(
    [string]$RoutineRoot,
    [string]$MSBuildPath = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
if (-not $RoutineRoot) { $RoutineRoot = Join-Path $projectPath 'temp/deps/routine' }
$RoutineRoot = (Resolve-Path -LiteralPath $RoutineRoot).Path
$outputPath = Join-Path $projectPath 'temp/public-sdk-build'
$objectPath = Join-Path $projectPath 'temp/public-sdk-obj'
$propsPath = Join-Path $PSScriptRoot 'public-sdk/build.props'
$logPath = Join-Path $projectPath 'temp/public-sdk-build.log'
if (-not (Test-Path -LiteralPath $MSBuildPath)) { throw 'Visual Studio 2022 MSBuild was not found; supply -MSBuildPath.' }
if (-not (Test-Path -LiteralPath (Join-Path $RoutineRoot 'src/routine.c'))) { throw 'RoutineRoot must contain the public routine SDK source.' }
$RoutineRoot = & (Join-Path $PSScriptRoot 'public-sdk/prepare.ps1') -RoutineRoot $RoutineRoot

Write-Host 'Experimental public SDK compatibility build; does not install or launch Simplewall.'
Push-Location $projectPath
try {
    & $MSBuildPath simplewall.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 "/p:ForceImportBeforeCppTargets=$propsPath" "/p:PublicRoutineRoot=$RoutineRoot" "/p:OutDir=$outputPath\" "/p:IntDir=$objectPath\" /v:quiet /nologo *> $logPath
    $buildResult = $LASTEXITCODE
} finally { Pop-Location }
Get-Content -LiteralPath $logPath
if ($buildResult -ne 0) { throw "Build failed; see $logPath" }
Write-Host "Built: $outputPath\simplewall.exe"
