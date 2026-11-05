[CmdletBinding()]
param(
    [int]$TargetProcessId,
    [string]$ResultPath = 'C:\Users\Public\simplewall-build\ui-regression-results.json'
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SwUiRegressionNative {
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window,int id);
 [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr SendMessageTimeout(IntPtr window,uint message,IntPtr wparam,IntPtr lparam,uint flags,uint timeout,out IntPtr result);
}
'@
function Send([IntPtr]$Window,[uint32]$Message,[long]$W=0,[long]$L=0) {
 $value=[IntPtr]::Zero
 if([SwUiRegressionNative]::SendMessageTimeout($Window,$Message,[IntPtr]$W,[IntPtr]$L,2,5000,[ref]$value) -eq [IntPtr]::Zero){throw "Window message $Message failed or timed out"}
 return $value.ToInt64()
}
if($TargetProcessId){$app=Get-Process -Id $TargetProcessId}else{$app=Get-Process simplewall|Select-Object -First 1}
$main=$app.MainWindowHandle
$rebar=[SwUiRegressionNative]::GetDlgItem($main,102)
$toolbar=[SwUiRegressionNative]::GetDlgItem($rebar,103)
$tabs=[SwUiRegressionNative]::GetDlgItem($main,105)
if($toolbar -eq [IntPtr]::Zero){throw 'Toolbar not found'}
$initial=Send $toolbar 1042 311 # TB_GETSTATE(notification)
if($initial -lt 0){throw 'Notification button not found'}
$initialChecked=($initial -band 1) -ne 0
$states=@($initial)
try {
 for($i=0;$i -lt 2;$i++){
  # Mimic BTNS_CHECK's automatic click state before WM_COMMAND.
  $current=Send $toolbar 1042 311
  $nextChecked=($current -band 1) -eq 0
  [void](Send $toolbar 1026 311 ([int]$nextChecked)) # TB_CHECKBUTTON
  [void](Send $main 273 311) # WM_COMMAND
  $after=Send $toolbar 1042 311
  if((($after -band 1) -ne 0) -ne $nextChecked -or ($after -band 2)){
   throw "Notification state did not follow click: $after"
  }
  $states+=$after
 }
 $tabBefore=Send $tabs 4875 # TCM_GETCURSEL
 $lists=@()
 for($id=106;$id -le 113;$id++){
  $list=[SwUiRegressionNative]::GetDlgItem($main,$id)
  $lists+=@{id=$id;window=$list;count=(Send $list 4100);selection=(Send $list 4108 -1 2)}
 }
 $times=@()
 for($i=0;$i -lt 20;$i++){
  $watch=[Diagnostics.Stopwatch]::StartNew()
  [void](Send $main 273 262) # Refresh
  $times+=$watch.Elapsed.TotalMilliseconds
 }
 Start-Sleep -Seconds 1
 [void](Send $main 0)
 $tabAfter=Send $tabs 4875
 if($tabAfter -ne $tabBefore){throw 'Refresh changed the current tab'}
 foreach($entry in $lists){
  if((Send $entry.window 4100) -ne $entry.count -or (Send $entry.window 4108 -1 2) -ne $entry.selection){
   throw "Refresh changed row count or selection in control $($entry.id)"
  }
 }
 $max=($times|Measure-Object -Maximum).Maximum
 if($max -gt 250){throw "Refresh dispatch exceeded 250 ms: $max"}
 $app.Refresh()
 [PSCustomObject]@{process=$app.Id;responding=$app.Responding;notification_states=$states;notification_restored=$true;refresh_clicks=$times.Count;refresh_max_ms=[math]::Round($max,3);refresh_mean_ms=[math]::Round(($times|Measure-Object -Average).Average,3);selection_preserved=$true;tab_preserved=$true;sha256=(Get-FileHash $app.Path).Hash}|ConvertTo-Json -Depth 4|Set-Content $ResultPath
 Get-Content $ResultPath
} finally {
 $currentChecked=((Send $toolbar 1042 311) -band 1) -ne 0
 if($currentChecked -ne $initialChecked){[void](Send $main 273 311)}
}
