#requires -Version 7.0
[CmdletBinding()]
param(
    # Both configurations always build. Each test target runs only where listed:
    # 'target' or 'release:target' runs in release, 'dev:target' in dev. Commas may join items.
    [string[]]$Targets = @(),
    [ValidateSet('dev', 'release')][string[]]$Configs = @('dev', 'release'),
    [ValidateRange(1, 256)][int]$Jobs = 4,
    # Diagnostic only: deliberately time out one registered window target, not a test run.
    [string]$ProbeWindowTarget,
    [ValidateRange(1, 3600)][int]$ProbeTimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$Configs = @($Configs | Select-Object -Unique)
if (-not $Configs.Count) { throw 'At least one configuration is required.' }
$requested = @{ dev = [System.Collections.Generic.List[string]]::new(); release = [System.Collections.Generic.List[string]]::new() }
foreach ($item in @($Targets | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    $config, $target = if ($item -match '^(dev|release):(.+)$') { $Matches[1], $Matches[2] } else { 'release', $item }
    if ($target -eq 'renderer_acceptance') {
        throw 'renderer_acceptance runs every entry under one validation level; list renderer_acceptance_<entry> targets instead.'
    }
    if ($config -notin $Configs) { throw "${item}: configuration $config is not part of this run." }
    if ($target -notin $requested[$config]) { $requested[$config].Add($target) }
}
if ($ProbeWindowTarget -and $Targets.Count) { throw '-ProbeWindowTarget cannot be combined with -Targets.' }
# Default GPU validation scope: GBV for RHI and selected Renderer entries; every other
# window target runs with ATELIER_GPU_VALIDATION=debug (the D3D12 debug layer).
$gbvTargets = @('rhi_acceptance', 'renderer_acceptance_default', 'renderer_acceptance_scene_lit', 'renderer_acceptance_material_programs')
$results = [System.Collections.Generic.List[object]]::new()
$lockOwned = $false
$activator = $null
$run = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$run += "-$PID"
$logRoot = Join-Path $root 'out\acceptance-gate'
$runDirectory = Join-Path $logRoot $run
New-Item -ItemType Directory -Force $runDirectory | Out-Null
$gateLog = Join-Path $runDirectory 'gate.log'
$activeLog = Join-Path $runDirectory 'active-window.txt'
$history = Join-Path $logRoot 'history.csv'
if ((Test-Path $history) -and (Get-Content -LiteralPath $history -TotalCount 1) -notmatch '"Validation"') {
    # Export-Csv -Append cannot add a column, so give earlier rows an empty Validation once.
    $earlier = @(Import-Csv -LiteralPath $history)
    $earlier | Select-Object *, @{ n = 'Validation'; e = { '' } } |
        Export-Csv -LiteralPath $history -NoTypeInformation -Encoding utf8
}
$plannedScope = @($Configs | ForEach-Object {
    if ($ProbeWindowTarget) { "$_=probe:$ProbeWindowTarget" }
    elseif ($requested[$_].Count) { "$_=build+targets:$($requested[$_] -join ',')" }
    else { "$_=build" }
}) -join '; '
$exitCode = 1
$self = [Diagnostics.Process]::GetCurrentProcess()
$originalPriority = $self.PriorityClass

function Write-Gate([string]$Message) {
    $line = "$(Get-Date -Format o) $Message"
    Add-Content -LiteralPath $gateLog -Value $line
    Write-Host $line
}
function Add-Result([string]$Config, [string]$Target, [string]$Layer, [string]$Result, [double]$Seconds, [string]$Log,
    [string]$Validation = 'none') {
    $row = [pscustomobject]@{ Run = $run; Tier = $(if ($ProbeWindowTarget) { 'probe' } else { 'targeted' })
        Config = $Config; Layer = $Layer; Target = $Target; Result = $Result
        Seconds = [math]::Round($Seconds, 2); Log = $Log; Validation = $Validation }
    $results.Add($row)
    $row | Export-Csv -LiteralPath $history -Append -NoTypeInformation -Encoding utf8
    Write-Gate "$Config $Target $Result seconds=$($row.Seconds) validation=$Validation"
}

# The wrapper waits on an event until it belongs to this job. Descendants cannot
# escape between Start-Process and assignment; closing the job kills the whole tree.
$jobApi = @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
public sealed class AcceptanceJob : IDisposable {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr sa, string name);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenJobObject(uint access, bool inherit, string name);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool member);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job, int kind, IntPtr info, uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job, int kind, IntPtr info, uint size, IntPtr length);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateJobObject(IntPtr job, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    delegate bool WindowCallback(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(WindowCallback callback, IntPtr data);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    IntPtr handle;
    public string Name { get; }
    // The capture helper opens only a query handle to the same job.
    public AcceptanceJob(string name) {
        Name = name;
        handle = OpenJobObject(4, false, name);
        if (handle == IntPtr.Zero) throw new Win32Exception();
    }
    public AcceptanceJob(bool window) {
        Name = "Local\\AtelierAcceptanceJob-" + Guid.NewGuid();
        handle = CreateJobObject(IntPtr.Zero, Name);
        if (handle == IntPtr.Zero) throw new Win32Exception();
        // JOBOBJECT_EXTENDED_LIMIT_INFORMATION, Windows x64.
        IntPtr info = Marshal.AllocHGlobal(144);
        try {
            Marshal.Copy(new byte[144], 0, info, 144);
            // KILL_ON_JOB_CLOSE | PRIORITY_CLASS. AboveNormal is NOT automatically
            // inherited by CreateProcess; the job applies it to every descendant.
            Marshal.WriteInt32(info, 16, 0x2000 | 0x20);
            Marshal.WriteInt32(info, 56, window ? 0x8000 : 0x20);
            if (!SetInformationJobObject(handle, 9, info, 144)) throw new Win32Exception();
        } catch { Dispose(); throw; } finally { Marshal.FreeHGlobal(info); }
    }
    public void Assign(IntPtr process) {
        if (!AssignProcessToJobObject(handle, process)) throw new Win32Exception();
    }
    public void Stop() {
        if (!TerminateJobObject(handle, 1)) throw new Win32Exception();
    }
    public bool OwnsWindow(IntPtr window) {
        GetWindowThreadProcessId(window, out uint pid);
        if (pid == 0 || !IsWindowVisible(window)) return false;
        IntPtr process = OpenProcess(0x1000, false, pid);
        if (process == IntPtr.Zero) return false;
        try { return IsProcessInJob(process, handle, out bool member) && member; }
        finally { CloseHandle(process); }
    }
    public IntPtr[] Windows() {
        var windows = new List<IntPtr>();
        EnumWindows((window, data) => { if (OwnsWindow(window)) windows.Add(window); return true; }, IntPtr.Zero);
        return windows.ToArray();
    }
    public int Active {
        get {
            IntPtr info = Marshal.AllocHGlobal(48);
            try {
                if (!QueryInformationJobObject(handle, 1, info, 48, IntPtr.Zero)) throw new Win32Exception();
                return Marshal.ReadInt32(info, 40);
            } finally { Marshal.FreeHGlobal(info); }
        }
    }
    public void Dispose() {
        if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}
'@
Add-Type -TypeDefinition $jobApi

function Save-JobWindows([AcceptanceJob]$Job, [string]$Prefix) {
    $windows = $Job.Windows()
    if (-not $windows.Count) { Write-Gate '没有可截的窗口'; return }
    foreach ($window in $windows) {
        $path = "$Prefix.$($window.ToInt64()).png"
        $quotedPath = $path.Replace("'", "''")
        # PrintWindow renders only the owned window, never private windows covering it.
        # It can hang on an unresponsive UI, so bound it in a hidden, job-owned helper.
        $command = @"
`$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
$jobApi
'@
Add-Type -AssemblyName System.Drawing
`$job = [AcceptanceJob]::new('$($Job.Name)')
try {
    `$window = [IntPtr]::new($($window.ToInt64()))
    if (-not `$job.OwnsWindow(`$window)) { exit 2 }
} finally { `$job.Dispose() }
# Do not keep a job handle during PrintWindow: it would defeat kill-on-close
# if the gate itself died while this helper was blocked.
`$rect = [AcceptanceJob+Rect]::new()
if (-not [AcceptanceJob]::GetWindowRect(`$window, [ref]`$rect)) { exit 2 }
`$image = [Drawing.Bitmap]::new(`$rect.Right - `$rect.Left, `$rect.Bottom - `$rect.Top)
try {
    `$graphics = [Drawing.Graphics]::FromImage(`$image)
    try {
        `$dc = `$graphics.GetHdc()
        try { if (-not [AcceptanceJob]::PrintWindow(`$window, `$dc, 2)) { exit 2 } }
        finally { `$graphics.ReleaseHdc(`$dc) }
    } finally { `$graphics.Dispose() }
    `$image.Save('$quotedPath', [Drawing.Imaging.ImageFormat]::Png)
} finally { `$image.Dispose() }
"@
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
        $capture = $null
        try {
            $capture = Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -WindowStyle Hidden -PassThru `
                -ArgumentList @('-NoProfile', '-EncodedCommand', $encoded)
            $Job.Assign($capture.Handle)
            if (-not $capture.WaitForExit(4000)) {
                Write-Gate "Window capture timed out: $window (no desktop fallback)"
            } elseif ($capture.ExitCode -eq 0) {
                Write-Gate "Window captured: $path"
            } else { Write-Gate "Window capture unavailable: $window (no desktop fallback)" }
        } finally {
            if ($capture) {
                try {
                    if (-not $capture.HasExited) {
                        $capture.Kill($true)
                        if (-not $capture.WaitForExit(5000)) { throw 'Capture helper did not terminate.' }
                    }
                } finally { $capture.Dispose() }
            }
        }
    }
}

function Invoke-Build([string]$Config, [string]$Target, [int]$TimeoutSeconds = 0,
    [switch]$Window, [string]$Layer = 'build', [string]$Name = $Target, [string]$Validation = 'none') {
    $out = Join-Path $runDirectory "$Config-$Name.log"
    $eventName = "Local\AtelierAcceptance-$([guid]::NewGuid())"
    $event = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', $eventName)
    $job = [AcceptanceJob]::new([bool]$Window)
    $process = $null
    $assigned = $false
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $result = 'FAIL'
    try {
        $quotedRoot = $root.Replace("'", "''")
        $quotedOut = $out.Replace("'", "''")
        # Window targets get an explicit GPU validation switch; builds and headless targets create no device.
        $validationLine = if ($Validation -in @('gbv', 'debug')) { "`$env:ATELIER_GPU_VALIDATION = '$Validation'" }
            else { 'Remove-Item Env:ATELIER_GPU_VALIDATION -ErrorAction SilentlyContinue' }
        $command = @"
`$e = [System.Threading.EventWaitHandle]::OpenExisting('$eventName')
try { if (-not `$e.WaitOne(60000)) { exit 1 } } finally { `$e.Dispose() }
Set-Location '$quotedRoot'
$validationLine
& cmake --build --preset $Config --target $Target --parallel $Jobs -- -k 0 *> '$quotedOut'
exit `$LASTEXITCODE
"@
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
        Set-Content -LiteralPath $out -Value "START $Config $Target $(Get-Date -Format o)"
        if ($Window) { Set-Content -LiteralPath $activeLog -Value $out }
        $process = Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -WindowStyle Hidden -PassThru `
            -ArgumentList @('-NoProfile', '-EncodedCommand', $encoded)
        $job.Assign($process.Handle)
        $assigned = $true
        [void]$event.Set()
        while (-not $process.WaitForExit(500)) {
            if ($TimeoutSeconds -and $clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                $result = 'TIMEOUT'
                # Stop activation before capture/termination. Capture failure must not skip cleanup.
                if ($Window) { Remove-Item -LiteralPath $activeLog -ErrorAction SilentlyContinue }
                try { Save-JobWindows $job "$out.timeout" }
                catch { Write-Gate "Screenshot failed for ${Target}: $_" }
                break
            }
        }
        if ($result -ne 'TIMEOUT' -and $process.ExitCode -eq 0) { $result = 'PASS' }
    } finally {
        if ($Window) { Remove-Item -LiteralPath $activeLog -ErrorAction SilentlyContinue }
        try {
            # Also clean orphaned descendants after a parent exits with FAIL or PASS.
            $job.Stop()
            if ($process) {
                # Assignment failure leaves only the event-waiting wrapper outside the job.
                if (-not $assigned -and -not $process.HasExited) { $process.Kill($true) }
                if (-not $process.WaitForExit(15000)) { throw 'Build wrapper did not terminate.' }
            }
            $deadline = [DateTime]::UtcNow.AddSeconds(15)
            while ($job.Active -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
            if ($job.Active) { throw 'Process tree did not drain; refusing to start another target.' }
        } finally {
            $job.Dispose(); $event.Dispose()
            if ($process) { $process.Dispose() }
            $clock.Stop()
        }
    }
    Add-Content -LiteralPath $out -Value "`n$result seconds=$($clock.Elapsed.TotalSeconds) process-tree=empty"
    Add-Result $Config $Name $Layer $result $clock.Elapsed.TotalSeconds $out $Validation
    return $result
}

# Cold-inclusive maxima from recent ec-3 Ninja logs, rounded up to 0.1 minute.
# Release Control reserves its prior full run plus the observed 310s cold preparation.
# Keep these floors until shader caching lands; warm-only history must not shrink them.
# Renderer entries use the default floor: 30 minutes, the runner's own per-entry limit.
$minutes = @{
    editor_control_acceptance = @{ dev = 12.9; release = 11.5 }
    editor_sponza_acceptance = @{ dev = 10.4; release = 7.5 }
    editor_scene_lit_acceptance = @{ dev = 8.7; release = 7.6 }
    editor_dungeons_acceptance = @{ dev = 8.6; release = 2.1 }
}

Push-Location $root
try {
    $self.PriorityClass = 'Normal'
    Write-Gate "Requested scope: $plannedScope"
    . (Join-Path $PSScriptRoot 'enter-dev.ps1')
    $common = & git rev-parse --path-format=absolute --git-common-dir
    if ($LASTEXITCODE -ne 0) { throw 'Cannot locate git common directory.' }
    $lock = Join-Path (Split-Path -Parent $common) '.amp\in\locks\gpu-window.lock'
    New-Item -ItemType Directory -Force (Split-Path -Parent $lock) | Out-Null
    $windowRuns = [System.Collections.Generic.List[object]]::new()
    if ($ProbeWindowTarget) { Write-Gate 'PROBE ONLY: this run checks timeout cleanup and tests no change.' }
    # Within each configuration the requested headless targets run in parallel.
    foreach ($config in $Configs) {
        if (-not $ProbeWindowTarget -and (Invoke-Build $config all) -ne 'PASS') { throw "Prebuild failed: $config" }
        # Prebuild may regenerate CMake after a pull. Never use a stale target list.
        $windowList = @(Get-Content -LiteralPath "out\$config\acceptance-window-targets.txt" | Where-Object { $_ })
        $headlessList = @(Get-Content -LiteralPath "out\$config\acceptance-headless-targets.txt" | Where-Object { $_ })
        if ($ProbeWindowTarget) {
            if ($ProbeWindowTarget -notin $windowList) { throw "Not a registered window target: $ProbeWindowTarget" }
            $windowRuns.Add([pscustomobject]@{ Config = $config; Target = $ProbeWindowTarget })
            continue
        }
        $headless = @()
        foreach ($target in $requested[$config]) {
            if ($target -in $headlessList) { $headless += $target }
            elseif ($target -in $windowList) { $windowRuns.Add([pscustomobject]@{ Config = $config; Target = $target }) }
            else { throw "Not a registered acceptance target: ${config}:$target" }
        }
        if (-not $headless.Count) { continue }
        foreach ($target in $headless) {
            Remove-Item "out\$config\acceptance-headless\$target\result.csv" -ErrorAction SilentlyContinue
        }
        $headlessResult = Invoke-Build $config ($headless -join ' ') -Layer headless -Name headless-targets
        foreach ($target in $headless) {
            $directory = "out\$config\acceptance-headless\$target"
            $destination = Join-Path $runDirectory "$config-$target.log"
            if (Test-Path "$directory\result.csv") {
                $status = Import-Csv "$directory\result.csv"
                Copy-Item "$directory\output.log" $destination
                Add-Result $config $target headless $status.Result ([double]$status.Seconds) $destination
            } else {
                Set-Content $destination 'FAIL: no result; target was blocked or interrupted. See headless-targets log.'
                Add-Result $config $target headless FAIL 0 $destination
            }
        }
        if ($headlessResult -ne 'PASS') { Write-Gate "$config headless failed; continuing to collect window results." }
    }
    if ($windowRuns.Count) {
        Write-Gate 'Waiting for GPU/window lock (existing owner is never evicted).'
        while (-not $lockOwned) {
            if (Test-Path -LiteralPath $lock) {
                Write-Gate "Lock owner: $(Get-Content -LiteralPath $lock -Raw -ErrorAction SilentlyContinue)"
                Start-Sleep -Seconds 60
                continue
            }
            try {
                New-Item -ItemType File -Path $lock -ErrorAction Stop | Out-Null
                $lockOwned = $true
            } catch [System.IO.IOException] { Start-Sleep -Seconds 2 }
        }
        Set-Content -LiteralPath $lock -Value "pid=$PID caller=$env:USERNAME root=$root $(Get-Date -Format o) acceptance-gate windows=$(@($windowRuns | ForEach-Object { "$($_.Config):$($_.Target)" }) -join ',') probe=$ProbeWindowTarget"
        $self.PriorityClass = 'AboveNormal'
        Write-Gate 'Lock acquired.'
        # Timeout probes measure cleanup only; they must not request focus or inject input.
        $activator = if (-not $ProbeWindowTarget) {
            Start-Job -ArgumentList $activeLog, $root -ScriptBlock {
                param($activeLog, $root)
                Add-Type -Namespace Gate -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
'@
                while ($true) {
                    Start-Sleep -Seconds 2
                    $log = Get-Content -LiteralPath $activeLog -ErrorAction SilentlyContinue
                    if (-not $log) { continue }
                    $tail = @(Get-Content -LiteralPath $log -Tail 60 -ErrorAction SilentlyContinue)
                    $waiting = -1; $settled = -1
                    for ($i = 0; $i -lt $tail.Count; $i++) {
                        if ($tail[$i] -like '*click the editor title bar to begin*') { $waiting = $i }
                        elseif ($tail[$i] -like '*activation acquired*' -or $tail[$i] -clike '*FAIL*') { $settled = $i }
                    }
                    if ($waiting -lt 0 -or $settled -gt $waiting) { continue }
                    $editor = Get-Process -Name atelier_editor -ErrorAction SilentlyContinue |
                        Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero -and $_.Path.StartsWith("$root\", [StringComparison]::OrdinalIgnoreCase) } |
                        Sort-Object StartTime -Descending | Select-Object -First 1
                    if (-not $editor -or -not $editor.Responding) { continue }
                    $fg = [Gate.User32]::GetForegroundWindow(); $fgpid = 0
                    [void][Gate.User32]::GetWindowThreadProcessId($fg, [ref]$fgpid)
                    $owner = Get-Process -Id $fgpid -ErrorAction SilentlyContinue
                    if ($owner -and $owner.ProcessName -like 'atelier_*') { continue }
                    # Recheck that timeout/cleanup did not retire this log while polling.
                    if ((Get-Content -LiteralPath $activeLog -ErrorAction SilentlyContinue) -ne $log) { continue }
                    [Gate.User32]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
                    [Gate.User32]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
                    [void][Gate.User32]::SetForegroundWindow($editor.MainWindowHandle)
                }
            }
        }
        $pastWindows = if (Test-Path $history) {
            @(Import-Csv $history | Where-Object { $_.Layer -in @('window', 'dev-window', 'window-subset') -and $_.Result -eq 'PASS' })
        } else { @() }
        foreach ($entry in $windowRuns) {
            $config = $entry.Config; $target = $entry.Target
            $layer = if ($ProbeWindowTarget) { 'probe' } else { 'window' }
            $validation = if ($target -in $gbvTargets) { 'gbv' } else { 'debug' }
            $longest = if ($minutes.ContainsKey($target)) { [double]$minutes[$target][$config] } else { 15.0 }
            # Last three completed runs can raise the floor; probes/timeouts cannot inflate it.
            $recent = @($pastWindows | Where-Object { $_.Config -eq $config -and $_.Target -eq $target } | Select-Object -Last 3)
            foreach ($sample in $recent) { $longest = [math]::Max($longest, [double]$sample.Seconds / 60.0) }
            $timeout = [int][math]::Ceiling([math]::Max(5.0, 2.0 * $longest) * 60)
            if ($ProbeWindowTarget) { $timeout = $ProbeTimeoutSeconds }
            Write-Gate "Starting $config $target timeout=${timeout}s validation=$validation"
            [void](Invoke-Build $config $target $timeout -Window -Layer $layer -Validation $validation)
        }
    }
    $exitCode = if (@($results | Where-Object Result -ne 'PASS').Count) { 1 } else { 0 }
} catch {
    Write-Gate "FAIL: $_"
    $exitCode = 1
} finally {
    try {
        if ($activator) { Stop-Job $activator; Remove-Job $activator -Force }
    } finally {
        if ($lockOwned) {
            Remove-Item -LiteralPath $lock
            Write-Gate 'Lock released. Wait at least 60 seconds before requesting it again.'
        }
        $self.PriorityClass = $originalPriority
        $self.Dispose()
        $results | Export-Csv (Join-Path $runDirectory 'summary.csv') -NoTypeInformation -Encoding utf8
        $executedScope = @($results | ForEach-Object { "$($_.Config)=$($_.Layer)" } | Select-Object -Unique) -join '; '
        if (-not $executedScope) { $executedScope = 'none' }
        $summary = "Run=$(if ($ProbeWindowTarget) { 'probe' } else { 'targeted' }) Exit=$exitCode`nRequested: $plannedScope`nExecuted: $executedScope`n"
        $summary += $results | Format-Table Config, Layer, Target, Validation, Result, Seconds -AutoSize | Out-String -Width 200
        Set-Content (Join-Path $runDirectory 'summary.txt') $summary
        Write-Gate "DONE exit=$exitCode summary=$runDirectory"
        Pop-Location
    }
}
exit $exitCode
