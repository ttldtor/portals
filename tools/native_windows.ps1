param(
  [Parameter(Mandatory)][string]$Supervisor,
  [Parameter(Mandatory)][string]$Child,
  [Parameter(Mandatory)][string]$LogDirectory,
  [string[]]$ChildArguments = @(),
  [ValidateSet('none', 'dashboard')][string]$Drive = 'none',
  [int]$ExpectedExit = 0,
  [int]$DeadlineMs = 15000,
  [int]$QuitMs = 4500,
  [string]$Terminal = 'wt.exe'
)
$ErrorActionPreference = 'Stop'
$Supervisor = (Resolve-Path -LiteralPath $Supervisor).Path
$Child = (Resolve-Path -LiteralPath $Child).Path
if (Test-Path -LiteralPath $LogDirectory) { throw 'Use a fresh log directory to preserve evidence' }
New-Item -ItemType Directory -Path $LogDirectory | Out-Null
$LogDirectory = (Resolve-Path -LiteralPath $LogDirectory).Path
$title = 'portals-native-' + [guid]::NewGuid().ToString()
$log = Join-Path $LogDirectory 'supervisor.log'
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class PortalsNativeWindow {
  public delegate bool Callback(IntPtr window, IntPtr parameter);
  [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback, IntPtr parameter);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
  public static IntPtr[] Find(string title) {
    var result = new List<IntPtr>();
    EnumWindows((window, parameter) => {
      var text = new System.Text.StringBuilder(512);
      GetWindowText(window, text, text.Capacity);
      if (text.ToString() == title) result.Add(window);
      return true;
    }, IntPtr.Zero);
    return result.ToArray();
  }
}
'@
function Get-TerminalIdentity {
  @(Get-Process WindowsTerminal,OpenConsole -ErrorAction SilentlyContinue | ForEach-Object {
    [pscustomobject]@{ Id = $_.Id; Started = $_.StartTime.ToUniversalTime().ToString('o'); Path = $_.Path }
  })
}
function Quote-Argument([string]$value) {
  # Windows CRT quoting, including trailing backslashes and embedded quotes.
  '"' + [regex]::Replace([regex]::Replace($value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
$baseline = @(Get-TerminalIdentity)
$baseline | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $LogDirectory 'baseline.json')
Get-FileHash -Algorithm SHA256 -LiteralPath $Supervisor,$Child | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $LogDirectory 'hashes.json')
$arguments = @('-w', $title, '--size', '120,40', 'new-tab', '--title', $title, '--suppressApplicationTitle',
  '--startingDirectory', (Split-Path -Parent $Supervisor), $Supervisor, '--log', $log, '--title', $title,
  '--deadline-ms', "$DeadlineMs", '--quit-ms', "$QuitMs", '--expected-exit', "$ExpectedExit", '--drive', $Drive,
  '--', $Child) + $ChildArguments
$command = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
$started = [datetime]::UtcNow
$launcher = Start-Process -FilePath $Terminal -ArgumentList $command -WindowStyle Hidden -PassThru
$until = $started.AddMilliseconds($DeadlineMs + 12000)
$runtime = @{}
$ownWindowSeen = $false
do {
  foreach ($window in [PortalsNativeWindow]::Find($title)) {
    $ownWindowSeen = $true
    [uint32]$runtimePid = 0
    [void][PortalsNativeWindow]::GetWindowThreadProcessId($window, [ref]$runtimePid)
    if (-not $runtime.ContainsKey($runtimePid)) {
      $process = Get-Process -Id $runtimePid
      $runtime[$runtimePid] = [pscustomobject]@{ Id = $runtimePid; Path = $process.Path;
        Version = $process.FileVersion; Started = $process.StartTime.ToUniversalTime().ToString('o') }
    }
  }
  $text = if (Test-Path -LiteralPath $log) { Get-Content -Raw -LiteralPath $log } else { '' }
  if ($text -match '(PASS|FAIL): native application supervisor' -and [PortalsNativeWindow]::Find($title).Count -eq 0) { break }
  Start-Sleep -Milliseconds 100
} while ([datetime]::UtcNow -lt $until)
$forcedCleanup = $false
foreach ($window in [PortalsNativeWindow]::Find($title)) {
  $forcedCleanup = $true
  # Close this uniquely named fixture window only; never stop a shared WT process.
  [void][PortalsNativeWindow]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
}
if ($forcedCleanup) { Start-Sleep -Milliseconds 1500 }
$after = @(Get-TerminalIdentity)
$preserved = @($baseline | Where-Object {
  $original = $_
  @($after | Where-Object { $_.Id -eq $original.Id -and $_.Started -eq $original.Started }).Count -eq 1
}).Count -eq $baseline.Count
$ownPids = @([regex]::Matches($text, '(?:harness|child)_pid=(\d+)') | ForEach-Object { [int]$_.Groups[1].Value })
$alive = @($ownPids | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
$gone = [PortalsNativeWindow]::Find($title).Count -eq 0
$passed = $text -match 'PASS: native application supervisor' -and $ownWindowSeen -and $gone -and
  $alive.Count -eq 0 -and $preserved -and -not $forcedCleanup
$runtime.Values | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $LogDirectory 'runtime.json')
$after | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $LogDirectory 'after.json')
[ordered]@{ pass = $passed; title = $title; command = $command; expected_child_exit = $ExpectedExit;
  baseline_preserved = $preserved; own_window_seen = $ownWindowSeen; own_window_gone = $gone;
  own_pid_alive = $alive.Count; forced_cleanup = $forcedCleanup; launcher_pid = $launcher.Id
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $LogDirectory 'launch.json')
Get-Content -LiteralPath $log -ErrorAction SilentlyContinue
if ($passed) { exit 0 }
exit 1
