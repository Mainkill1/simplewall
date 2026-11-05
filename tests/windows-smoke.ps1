[CmdletBinding()]
param([string]$Root = 'C:\Users\Public\simplewall-build')
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SwNative {
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w,int id);
 [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,IntPtr b,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageTimeoutW",SetLastError=true)] public static extern IntPtr SendText(IntPtr w,uint m,IntPtr a,string b,uint flags,uint timeout,out IntPtr result);
 [DllImport("fwpuclnt.dll",CharSet=CharSet.Unicode)] public static extern uint FwpmEngineOpen0(string server,uint auth,IntPtr identity,IntPtr session,out IntPtr engine);
 [DllImport("fwpuclnt.dll")] public static extern uint FwpmProviderGetByKey0(IntPtr engine,ref Guid key,out IntPtr provider);
 [DllImport("fwpuclnt.dll")] public static extern uint FwpmEngineClose0(IntPtr engine);
 [DllImport("fwpuclnt.dll")] public static extern void FwpmFreeMemory0(ref IntPtr memory);
}
'@
function Send([IntPtr]$Window,[uint32]$Message,[long]$W=0,[long]$L=0) {
 $result=[IntPtr]::Zero
 if([SwNative]::SendMessageTimeout($Window,$Message,[IntPtr]$W,[IntPtr]$L,2,30000,[ref]$result) -eq [IntPtr]::Zero) { throw "Window message $Message timed out or failed" }
 return $result.ToInt64()
}
function Set-Search([IntPtr]$Window,[string]$Text) {
 $result=[IntPtr]::Zero
 if([SwNative]::SendText($Window,12,[IntPtr]::Zero,$Text,2,30000,[ref]$result) -eq [IntPtr]::Zero) { throw 'Search dispatch failed' }
}
$results=@()
try {
 # A synthetic profile must never replace an installed firewall policy.
 $engine=[IntPtr]::Zero
 $open=[SwNative]::FwpmEngineOpen0($null,10,[IntPtr]::Zero,[IntPtr]::Zero,[ref]$engine)
 if($open -ne 0) { throw "Cannot inspect active WFP policy ($open)" }
 $key=[Guid]'B0D553E2-C6A0-4A9A-AEB8-C752483ED62F';$provider=[IntPtr]::Zero
 $found=[SwNative]::FwpmProviderGetByKey0($engine,[ref]$key,[ref]$provider)
 if($found -eq 0) { [SwNative]::FwpmFreeMemory0([ref]$provider);throw 'Existing simplewall WFP provider: synthetic-profile benchmark skipped to preserve policy' }
 if($found -ne 2150760453) { # FWP_E_PROVIDER_NOT_FOUND (0x80320005)
  throw ('Unexpected provider lookup status 0x{0:X8}' -f $found)
 }
 [void][SwNative]::FwpmEngineClose0($engine)
 foreach($variant in @('baseline','optimized')) {
  $folder=Join-Path $Root "smoke-$variant"
  $watch=[Diagnostics.Stopwatch]::StartNew()
  $app=Start-Process (Join-Path $folder 'simplewall.exe') -WorkingDirectory $folder -PassThru
  $window=[IntPtr]::Zero;$list=[IntPtr]::Zero;$count=0
  for($i=0;$i -lt 300;$i++) {
   Start-Sleep -Milliseconds 100
   $app.Refresh()
   if($app.HasExited) {throw "$variant exited during startup ($($app.ExitCode))"}
   $window=$app.MainWindowHandle
   if($window -ne [IntPtr]::Zero) {
    $list=[SwNative]::GetDlgItem($window,106)
    if($list -ne [IntPtr]::Zero) { $count=Send $list 4100; if($count -ge 10000) {break} }
   }
  }
  if($count -lt 10000) {throw "$variant loaded only $count apps"}
  [void](Send $window 0)
  $startup=$watch.Elapsed.TotalMilliseconds
  Start-Sleep -Seconds 2
  $header=[IntPtr](Send $list 4127)
  $sort=@()
  for($i=0;$i -lt 3;$i++) {
   $watch.Restart();[void](Send $header 513 1 655410);[void](Send $header 514 0 655410)
   [void](Send $window 0);$sort+=$watch.Elapsed.TotalMilliseconds
  }
  $rebar=[SwNative]::GetDlgItem($window,102);$search=[SwNative]::GetDlgItem($rebar,104)
  if($search -eq [IntPtr]::Zero){throw 'Search control not found'}
  $watch.Restart()
  foreach($text in @('a','ap','app','app0','app00','app000')) {Set-Search $search $text}
  $typing=$watch.Elapsed.TotalMilliseconds
  Start-Sleep -Milliseconds 600
  [void](Send $window 0)
  Set-Search $search ''
  [void](Send $window 0)
  $results += [PSCustomObject]@{variant=$variant;apps=$count;startup_ms=[math]::Round($startup,1);sort_ms=$sort;typing_six_keys_ms=[math]::Round($typing,1);responding=$app.Responding}
  [void](Send $window 16)
  if(-not $app.WaitForExit(30000)) {throw "$variant failed graceful shutdown"}
 }
 $results|ConvertTo-Json -Depth 5|Set-Content (Join-Path $Root 'smoke-results.json')
 $installed=Join-Path $env:LOCALAPPDATA 'Programs/simplewall-performance/simplewall.exe'
 if(Test-Path -LiteralPath $installed) {
  $live=Start-Process $installed -WorkingDirectory (Split-Path $installed -Parent) -PassThru
  for($i=0;$i -lt 100;$i++) {
   Start-Sleep -Milliseconds 100;$live.Refresh()
   if($live.HasExited){throw 'Installed build exited during launch'}
   if($live.MainWindowHandle -ne [IntPtr]::Zero){break}
  }
  [void](Send $live.MainWindowHandle 0)
  [PSCustomObject]@{pid=$live.Id;path=$live.Path;responding=$live.Responding;sha256=(Get-FileHash $installed).Hash}|ConvertTo-Json|Set-Content (Join-Path $Root 'installed-running.json')
 }
 'PASS'|Set-Content (Join-Path $Root 'smoke-status.txt')
} catch {
 $_|Out-String|Set-Content (Join-Path $Root 'smoke-error.txt')
 'FAIL'|Set-Content (Join-Path $Root 'smoke-status.txt')
 exit 1
}
