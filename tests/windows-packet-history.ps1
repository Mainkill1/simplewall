[CmdletBinding()]
param([int]$TargetProcessId,[switch]$RequireHistory,[string]$ResultPath='C:\Users\Public\simplewall-build\packet-history-ui-results.json')
$ErrorActionPreference='Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PacketHistoryUi {
 public delegate bool Callback(IntPtr w,IntPtr p);
 [DllImport("user32.dll")]public static extern bool EnumWindows(Callback c,IntPtr p);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll")]public static extern IntPtr GetDlgItem(IntPtr w,int id);
 [DllImport("user32.dll")]public static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,IntPtr b,uint flags,uint timeout,out IntPtr result);

 [DllImport("kernel32.dll",SetLastError=true)]static extern IntPtr OpenProcess(uint access,bool inherit,uint id);
 [DllImport("kernel32.dll")]static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll")]static extern IntPtr VirtualAllocEx(IntPtr p,IntPtr address,UIntPtr bytes,uint type,uint protect);
 [DllImport("kernel32.dll")]static extern bool VirtualFreeEx(IntPtr p,IntPtr address,UIntPtr bytes,uint type);
 [DllImport("kernel32.dll")]static extern bool WriteProcessMemory(IntPtr p,IntPtr address,byte[] data,int size,out UIntPtr written);
 [DllImport("kernel32.dll")]static extern bool ReadProcessMemory(IntPtr p,IntPtr address,byte[] data,int size,out UIntPtr read);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageTimeoutW")]static extern IntPtr SendText(IntPtr w,uint m,IntPtr a,System.Text.StringBuilder b,uint flags,uint timeout,out IntPtr result);
 public static string Text(IntPtr w){var b=new System.Text.StringBuilder(8192);IntPtr r;if(SendText(w,13,(IntPtr)b.Capacity,b,2,2000,out r)==IntPtr.Zero)throw new Exception("Text query timeout");return b.ToString();}
 public static void Select(IntPtr w,int index){uint pid;GetWindowThreadProcessId(w,out pid);IntPtr p=OpenProcess(0x38,false,pid);if(p==IntPtr.Zero)throw new Exception("OpenProcess failed");IntPtr mem=VirtualAllocEx(p,IntPtr.Zero,(UIntPtr)88,0x3000,4);bool completed=false;try{byte[] item=new byte[88];Array.Copy(BitConverter.GetBytes(3),0,item,12,4);Array.Copy(BitConverter.GetBytes(3),0,item,16,4);UIntPtr written;if(!WriteProcessMemory(p,mem,item,item.Length,out written))throw new Exception("WriteProcessMemory failed");IntPtr r;completed=SendMessageTimeout(w,4139,(IntPtr)index,mem,2,2000,out r)!=IntPtr.Zero;if(!completed)throw new Exception("Selection timeout");}finally{if(completed)VirtualFreeEx(p,mem,UIntPtr.Zero,0x8000);CloseHandle(p);}}
 public static string Cell(IntPtr w,int index,int column){uint pid;GetWindowThreadProcessId(w,out pid);IntPtr p=OpenProcess(0x38,false,pid);if(p==IntPtr.Zero)throw new Exception("OpenProcess failed");IntPtr mem=VirtualAllocEx(p,IntPtr.Zero,(UIntPtr)8280,0x3000,4);bool completed=false;try{byte[] item=new byte[88];Array.Copy(BitConverter.GetBytes(column),0,item,8,4);Array.Copy(BitConverter.GetBytes(mem.ToInt64()+88),0,item,24,8);Array.Copy(BitConverter.GetBytes(4096),0,item,32,4);UIntPtr written;if(!WriteProcessMemory(p,mem,item,item.Length,out written))throw new Exception("WriteProcessMemory failed");IntPtr r;completed=SendMessageTimeout(w,4211,(IntPtr)index,mem,2,2000,out r)!=IntPtr.Zero;if(!completed)throw new Exception("Cell query timeout");byte[] text=new byte[8192];if(!ReadProcessMemory(p,(IntPtr)(mem.ToInt64()+88),text,text.Length,out written))throw new Exception("ReadProcessMemory failed");return System.Text.Encoding.Unicode.GetString(text).Split('\0')[0];}finally{if(completed)VirtualFreeEx(p,mem,UIntPtr.Zero,0x8000);CloseHandle(p);}}
 public static IntPtr Find(int pid){IntPtr result=IntPtr.Zero;EnumWindows((w,p)=>{uint id;GetWindowThreadProcessId(w,out id);if(id==pid&&GetDlgItem(w,113)!=IntPtr.Zero)result=w;return true;},IntPtr.Zero);return result;}
}
'@
function Send([IntPtr]$w,[uint32]$m,[long]$a=0,[long]$b=0){$r=[IntPtr]::Zero;if([PacketHistoryUi]::SendMessageTimeout($w,$m,[IntPtr]$a,[IntPtr]$b,2,2000,[ref]$r) -eq [IntPtr]::Zero){throw "UI message $m timed out"};$r.ToInt64()}
if($TargetProcessId){$app=Get-Process -Id $TargetProcessId}else{$app=Get-Process simplewall|Select-Object -First 1}
$main=[PacketHistoryUi]::Find($app.Id)
$panel=[PacketHistoryUi]::GetDlgItem($main,113)
foreach($id in @(6100,6101,6102,6103,6104,6105,6106,6107,6108,6109,6110,6111,6112,6115,6116,6117)){
 if([PacketHistoryUi]::GetDlgItem($panel,$id) -eq [IntPtr]::Zero){throw "Split-view control $id missing"}
}
# The host panel must terminate legacy list traversal instead of returning row 0 forever.
if((Send $panel 4108 -1 2) -ne -1){throw 'Packet host accepted a legacy list traversal'}
if($RequireHistory){
 [void](Send $panel 273 6102)
 for($wait=0;$wait -lt 50;$wait++){
  Start-Sleep -Milliseconds 100
  $appRows=Send ([PacketHistoryUi]::GetDlgItem($panel,6100)) 4100
  $packetRows=Send ([PacketHistoryUi]::GetDlgItem($panel,6101)) 4100
  if($appRows -gt 1 -and $packetRows -gt 0){break}
 }
 if($appRows -le 1 -or $packetRows -eq 0){throw "Live history missing: apps=$appRows packetRows=$packetRows status=$([PacketHistoryUi]::Text([PacketHistoryUi]::GetDlgItem($panel,6117)))"}
 $apps=[PacketHistoryUi]::GetDlgItem($panel,6100)
 [PacketHistoryUi]::Select($apps,1)
 Start-Sleep -Milliseconds 700
 $header=[PacketHistoryUi]::Text([PacketHistoryUi]::GetDlgItem($panel,6115))
 if(-not $header -or $header.StartsWith('All apps')){throw 'Selecting an app did not load its details'}
 Start-Process 'C:\Windows\System32\curl.exe' -ArgumentList '--noproxy * --connect-timeout 2 --max-time 3 -k -I https://1.1.1.1' -RedirectStandardOutput 'C:\Users\Public\simplewall-build\history-selection-curl-out.txt' -RedirectStandardError 'C:\Users\Public\simplewall-build\history-selection-curl-error.txt' -NoNewWindow -Wait
 Start-Sleep -Milliseconds 1200
 $after=[PacketHistoryUi]::Text([PacketHistoryUi]::GetDlgItem($panel,6115))
 if($after -ne $header){throw 'Incoming traffic changed the selected app'}
 [void](Send $main 273 329) # Copy: legacy traversal must not hang.
 [void](Send $main 273 326) # Legacy disable-notifications must be harmless in this view.
 [void](Send $main 0)
 [PacketHistoryUi]::Select($apps,0)
}
$group=[PacketHistoryUi]::GetDlgItem($panel,6105)
$original=Send $group 240
$timings=@()
try{
 for($i=0;$i -lt 20;$i++){
  [void](Send $group 241 ($i%2))
  [void](Send $panel 273 6105)
  [void](Send $panel 273 (6102+($i%2)))
  $watch=[Diagnostics.Stopwatch]::StartNew();[void](Send $main 0);$timings+=$watch.Elapsed.TotalMilliseconds
 }
 Start-Sleep -Seconds 2
 $rows=Send ([PacketHistoryUi]::GetDlgItem($panel,6101)) 4100
 if($rows -gt 500){throw "Page contains $rows rows; expected at most 500"}
 $max=($timings|Measure-Object -Maximum).Maximum
 if($max -ge 100){throw "UI probe exceeded target: $max ms"}
 [PSCustomObject]@{process=$app.Id;split_controls=$true;page_rows=$rows;rapid_filter_switches=20;live_history_checked=[bool]$RequireHistory;selection_stable=[bool]$RequireHistory;probe_max_ms=$max;sha256=(Get-FileHash $app.Path).Hash}|ConvertTo-Json|Set-Content $ResultPath
 Get-Content $ResultPath
}finally{[void](Send $group 241 $original);[void](Send $panel 273 6105);[void](Send $panel 273 6102)}
