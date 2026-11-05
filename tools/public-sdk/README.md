# Public SDK build

This simplewall revision targets an unpublished routine API. These build helpers
restore the application-side adapter from upstream historical commit
`b72c13a0ecc1d42de8c163958393587e261f9886`, with corrections for Z order,
logging/error argument order, timestamp and toolbar/image-list helpers.
Version-query and SID-conversion calls map their output-first arguments to the
public SDK, and toolbar text updates preserve existing image indexes.
List insertion translates the current append sentinel (`INT_ERROR`) into the
current item count, because the public SDK forwards negative indexes directly
to Windows, where insertion fails. This applies to packet and connection rows.

Use the public routine `v.2.7.11` source (commit
`ff8f811`, also present unchanged in `6e6888c`). `prepare.ps1` verifies its SHA-256
and prepares a private copy with a bounded string-length scan. The source SDK
checkout is not modified. The adapter is included only in application sources.

On Windows with MSVC v143 and a Windows SDK:

```powershell
.\tools\build-public-sdk.ps1 -RoutineRoot ..\routine -MSBuildPath C:\BuildTools\MSBuild\Current\Bin\MSBuild.exe
.\tools\install-performance.ps1 -Launch
```

Run `.\tools\test-public-sdk.ps1` for the native Windows resource/SID/list-insertion ABI
regression (no elevation required). The test protects version-resource memory
as read-only, reproducing the startup crash if the output arguments are swapped.

Restore `Microsoft.Windows.CppWinRT` version `2.0.230706.1` into `packages` first
using NuGet. The normal project/toolset configuration is unchanged.

Run the portable C regression harness on Linux/WSL with:

```sh
python3 tests/test_performance.py
CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 tests/test_performance.py
```

`tests/windows-smoke.ps1` compares prepared `smoke-baseline` and
`smoke-optimized` directories with 10,000 apps and 1,000 rules. It requires an
elevated PowerShell session and refuses synthetic-profile testing if a simplewall
WFP provider already exists. It does not enable filters.
