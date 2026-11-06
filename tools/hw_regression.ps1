<#
.SYNOPSIS
  Hardware regression: build, flash and test the demo card with each of its three ATRs on a real PC/SC reader.

.DESCRIPTION
  For each ATR configuration of examples/demo/card_profile.c (t0, t1, negotiable):

    1. builds the demo with -DDEMO_ATR=<name> in build-atr-<name>/ (tools/flash.ps1; rebuilds are incremental),
    2. flashes it with picotool (no button needed),
    3. waits for the Pico's USB serial console and checks the boot self-test line
       ("selftest(loopback OK)" and "parity error: detected"),
    4. runs tools/sc_test.ps1 with the matching protocol (t0: T0, t1: T1, negotiable: Any and then T0); a run that
       fails before any check, or sees another ATR than expected (the reader still holding the session of the previous
       firmware), is retried once,
    5. reads the console's `status` and requires every error counter and the repetition counter to be 0; for the
       negotiable ATR the log must also show the PPS to T=1 at F/D 372/4.

  Prints a summary table and exits with 1 if anything failed.  Finally the t0 build (identical to the default demo)
  is flashed again so the board is left in its standard state.

  The console is only ever opened in a child process with a timeout: a .NET SerialPort opened in this process can hang
  it when the Pico disappears.  Takes about 1.5 minutes with up-to-date builds (fresh build directories add
  about 10 s each).

.PARAMETER Only
  Run only these configurations (t0, t1, negotiable; default: all three).

.PARAMETER NoBuild
  Flash the existing UF2s in build-atr-<name>/ without running cmake/ninja.

.PARAMETER Reader
  Substring of the PC/SC reader name (default: SCR33), passed to sc_test.ps1.

.PARAMETER Port
  The Pico's console COM port.  Default: found by USB vendor ID (2E8A, Raspberry Pi).

.EXAMPLE
  .\tools\hw_regression.ps1                  # all three ATRs
  .\tools\hw_regression.ps1 -Only t1         # the T=1 ATR only
  .\tools\hw_regression.ps1 -NoBuild -Reader ACS
#>
param(
    [ValidateSet('t0', 't1', 'negotiable')] [string[]]$Only = @('t0', 't1', 'negotiable'),
    [switch]$NoBuild,
    [string]$Reader = 'SCR33',
    [string]$Port
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pwsh = (Get-Process -Id $PID).Path

# What each configuration must look like.  Atr is the expected ATR without its TCK (the check that the reader really
# talks to the new firmware); each run is one sc_test.ps1 invocation.
$configs = [ordered]@{
    t0         = @{ Atr = '3B 08 50 69 63 6F 37 38 31 36'
                    Runs = @(@{ Protocol = 'T0'; Expect = 'T=0' }) }
    t1         = @{ Atr = '3B 88 81 31 FE 45 50 69 63 6F 37 38 31 36'
                    Runs = @(@{ Protocol = 'T1'; Expect = 'T=1' }) }
    negotiable = @{ Atr = '3B 98 13 80 31 FE 45 50 69 63 6F 37 38 31 36'
                    Runs = @(@{ Protocol = 'Any'; Expect = 'T=1'; Log = 'PPS: T=1 F/D 372/1 -> 372/4' },
                             @{ Protocol = 'T0'; Expect = 'T=0' }) }
}

# --- helpers -----------------------------------------------------------------

# Runs pwsh with the given arguments in a child process; kills it after $TimeoutSec.
function Invoke-Child([string[]]$ArgList, [int]$TimeoutSec) {
    $psi = [Diagnostics.ProcessStartInfo]::new($pwsh)
    foreach ($a in @('-NoProfile', '-NonInteractive') + $ArgList) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $root
    $p = [Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    $timedOut = -not $p.WaitForExit($TimeoutSec * 1000)
    if ($timedOut) { try { $p.Kill($true) } catch {} }
    $p.WaitForExit()
    $text = ($out.Result + $err.Result) -replace "`r", '' -replace "`e\[[0-9;]*m", ''
    [pscustomobject]@{ Text = $text; ExitCode = $p.ExitCode; TimedOut = $timedOut }
}

function Find-Port {
    $names = [IO.Ports.SerialPort]::GetPortNames()
    if ($Port) { return $(if ($names -contains $Port) { $Port } else { $null }) }
    foreach ($dev in Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue) {
        if ($dev.DeviceID -notlike 'USB\VID_2E8A*' -or $dev.Name -notmatch '\((COM\d+)\)') { continue }
        if ($names -contains $Matches[1]) { return $Matches[1] }
    }
    return $null
}

function Wait-Port([int]$TimeoutSec = 30) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $found = Find-Port
        if ($found) { return $found }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

# Opens the console in a child process, sends $Send, collects everything for $ReadMs and returns it (the firmware
# first replays the log lines collected since the last read).
function Read-Console([string]$ComPort, [string]$Send, [int]$ReadMs = 2500) {
    $sendB64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($Send))
    $code = @"
`$p = New-Object System.IO.Ports.SerialPort '$ComPort', 115200, 'None', 8, 'One'
`$p.ReadTimeout = 300; `$p.DtrEnable = `$true; `$p.RtsEnable = `$true
`$p.Open()
Start-Sleep -Milliseconds 600
`$sb = New-Object System.Text.StringBuilder
try { [void]`$sb.Append(`$p.ReadExisting()) } catch {}
`$send = [Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('$sendB64'))
if (`$send) { `$p.Write(`$send) }
`$deadline = (Get-Date).AddMilliseconds($ReadMs)
while ((Get-Date) -lt `$deadline) {
    Start-Sleep -Milliseconds 100
    try { if (`$p.BytesToRead -gt 0) { [void]`$sb.Append(`$p.ReadExisting()) } } catch {}
}
[Console]::Out.Write(`$sb.ToString())
try { `$p.DtrEnable = `$false } catch {}
try { `$p.Dispose() } catch {}
[Environment]::Exit(0)
"@
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $r = Invoke-Child @('-EncodedCommand', $enc) ([int]($ReadMs / 1000) + 15)
    if ($r.TimedOut) { return '' }
    return $r.Text
}

# Builds (unless -NoBuild) and flashes one configuration; returns $null on success or an error text.
function Install-Config([string]$Name) {
    $flash = Join-Path $PSScriptRoot 'flash.ps1'
    $cmd = "& '$flash' -BuildDir 'build-atr-$Name' -CMakeArgs '-DDEMO_ATR=$Name'"
    if ($NoBuild) { $cmd += ' -NoBuild' }
    $r = Invoke-Child @('-Command', "$cmd; exit `$LASTEXITCODE") 900
    $ok = -not $r.TimedOut -and $r.ExitCode -eq 0 -and $r.Text -match 'flashed '
    if ($ok) { return $null }
    $tail = (($r.Text -split "`n") | Where-Object { $_ -and $_ -notmatch 'Loading into Flash' } |
             Select-Object -Last 8) -join "`n    "
    return "flash.ps1 failed$(if ($r.TimedOut) { ' (timed out)' }):`n    $tail"
}

# Waits for the rebooted firmware's console and checks its self-test line; returns @(port, error text or $null, log).
function Test-Boot {
    $com = Wait-Port 30
    if (-not $com) { return @($null, 'console port did not come back within 30 s', '') }
    Start-Sleep -Milliseconds 1000
    $log = Read-Console $com '' 1500
    $line = ($log -split "`n") | Where-Object { $_ -match 'selftest' } | Select-Object -First 1
    if (-not $line) { return @($com, 'no selftest line in the boot log', $log) }
    if ($line -notmatch 'selftest\(loopback OK\)' -or $line -notmatch 'parity error: detected') {
        return @($com, "self-test: $line", $log)
    }
    return @($com, $null, $log)
}

function Invoke-ScTest([string]$Protocol, [string]$Apdu) {
    $scArgs = @('-File', (Join-Path $PSScriptRoot 'sc_test.ps1'), '-Protocol', $Protocol, '-Reader', $Reader)
    if ($Apdu) { $scArgs += @('-Apdu', $Apdu) }
    $r = Invoke-Child $scArgs 180
    $atr = $proto = $null
    if ($r.Text -match '(?m)^ATR\s*:\s*([0-9A-F ]+?)\s+\((T=\d|0x[0-9A-F]+)\)') {
        $atr = $Matches[1]
        $proto = $Matches[2]
    }
    [pscustomobject]@{
        Text   = $r.Text
        Atr    = $atr
        Proto  = $proto
        Pass   = ([regex]::Matches($r.Text, '(?m)^\[PASS\]')).Count
        Fail   = ([regex]::Matches($r.Text, '(?m)^\[FAIL\]')).Count
        Passed = (-not $r.TimedOut) -and $r.Text -match 'all checks passed'
    }
}

function Get-Status([string]$Text) {
    $s = @{}
    if ($Text -match 'rx/tx chars:\s*(\d+)\s*/\s*(\d+)') { $s.Rx = [long]$Matches[1]; $s.Tx = [long]$Matches[2] }
    if ($Text -match 'errors\s*:\s*rx parity (\d+), rx overrun (\d+), tx echo (\d+), tx timeout (\d+)') {
        $s.Errors = $Matches[0]
        $s.ErrorSum = [long]$Matches[1] + [long]$Matches[2] + [long]$Matches[3] + [long]$Matches[4]
    }
    if ($Text -match 'repetition\s*:\s*(\d+)') { $s.Repeats = [long]$Matches[1] }
    return $s
}

# --- main --------------------------------------------------------------------

$results = [Collections.Generic.List[object]]::new()
function Add-Result($atr, $proto, $ok, $checks, $chars, $errors, $secs, $note) {
    $results.Add([pscustomobject]@{
        ATR = $atr; Protocol = $proto; Result = $(if ($ok) { 'PASS' } else { 'FAIL' }); Checks = $checks
        'Chars rx/tx' = $chars; Errors = $errors; 'Time [s]' = [math]::Round($secs, 1); Note = $note
    })
}

$started = Get-Date
$aborted = $false
$lastFlashed = $null
foreach ($name in $configs.Keys) {
    if ($Only -notcontains $name) { continue }
    $cfg = $configs[$name]
    $what = if ($NoBuild) { 'flash' } else { 'build + flash' }
    Write-Host "=== $name : $what (build-atr-$name) ===" -ForegroundColor Cyan
    $t = Get-Date
    $err = Install-Config $name
    if ($err) {
        Write-Host $err -ForegroundColor Red
        Add-Result $name '-' $false '' '' '' ((Get-Date) - $t).TotalSeconds 'build/flash failed'
        # A failed picotool run usually needs a human (replug); do not keep flashing.
        if ($err -match 'picotool') { $aborted = $true; break }
        continue
    }
    $lastFlashed = $name
    # $console collects the console output since the boot (log lines and status answers).
    $com, $err, $console = Test-Boot
    Write-Host ("flashed in {0:N0} s; console {1}" -f ((Get-Date) - $t).TotalSeconds, $com)
    if ($err) {
        Write-Host $err -ForegroundColor Red
        Add-Result $name '-' $false '' '' '' ((Get-Date) - $t).TotalSeconds $err
        continue
    }

    $prev = @{ Rx = 0; Tx = 0 }
    foreach ($run in $cfg.Runs) {
        $t = Get-Date
        $notes = @()
        if ($run.Log) {
            # The suite logs far more than the console's log ring holds, so look for the line in a short session of
            # its own first: one APDU (GET VERSION), then the log.  Its characters count in the status read below.
            $probe = Invoke-ScTest $run.Protocol '80 10 00 00 00'
            if (-not $probe.Atr -or -not $probe.Atr.StartsWith($cfg.Atr)) {
                Start-Sleep -Seconds 3
                [void](Invoke-ScTest $run.Protocol '80 10 00 00 00')
            }
            $console += Read-Console $com '' 1000
        }
        $sc = Invoke-ScTest $run.Protocol
        $stale = $sc.Pass -eq 0 -or -not $sc.Atr -or -not $sc.Atr.StartsWith($cfg.Atr)
        if (-not $sc.Passed -and $stale) {
            # Right after a flash the reader may still hold the old session (stale ATR) or fail the first connect.
            $notes += 'retried'
            Start-Sleep -Seconds 3
            $sc = Invoke-ScTest $run.Protocol
        }
        $ok = $sc.Passed
        if (-not $sc.Passed) {
            # The failed checks, or else the last line (an exception's message ends the output).
            $lines = ($sc.Text -split "`n") | Where-Object { $_.Trim() }
            $why = @($lines | Where-Object { $_ -match '^\[FAIL\]' } | Select-Object -First 3)
            if (-not $why -and $lines) { $why = @(($lines | Select-Object -Last 1) -replace '^[\s|]+', '') }
            $notes += $(if ($why) { $why -join ' / ' } else { 'sc_test did not pass' })
        }
        if ($sc.Atr -and -not $sc.Atr.StartsWith($cfg.Atr)) { $ok = $false; $notes += "ATR $($sc.Atr)" }
        if ($sc.Proto -ne $run.Expect) { $ok = $false; $notes += "protocol $($sc.Proto), expected $($run.Expect)" }

        $text = Read-Console $com "status`r" 2500
        $console += $text
        $st = Get-Status $text
        if ($null -eq $st.ErrorSum -or $null -eq $st.Repeats -or $null -eq $st.Rx) {
            $ok = $false; $notes += 'no status output'
        } else {
            if ($st.ErrorSum -ne 0) { $ok = $false; $notes += $st.Errors }
            if ($st.Repeats -ne 0) { $ok = $false; $notes += "repetition $($st.Repeats)" }
        }
        if ($run.Log -and $console -notmatch [regex]::Escape($run.Log)) { $ok = $false; $notes += "no '$($run.Log)'" }
        if ($run.Log -and $ok) { $notes += $run.Log }

        $chars = if ($null -ne $st.Rx) { "$($st.Rx - $prev.Rx) / $($st.Tx - $prev.Tx)" } else { '?' }
        if ($null -ne $st.Rx) { $prev = $st }
        $errors = if ($null -ne $st.ErrorSum) { $st.ErrorSum + $st.Repeats } else { '?' }
        $proto = "$($run.Protocol) -> $($sc.Proto)"
        Add-Result $name $proto $ok "$($sc.Pass)/$($sc.Pass + $sc.Fail)" $chars $errors ((Get-Date) - $t).TotalSeconds `
            ($notes -join '; ')
        Write-Host ("{0,-10} {1,-12} {2}  {3}" -f $name, $proto, $(if ($ok) { 'PASS' } else { 'FAIL' }),
                    ($notes -join '; ')) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
    }
}

# Leave the board running the default demo (the t0 build is the same firmware as build/ with the #define default).
if (-not $aborted -and $lastFlashed -ne 't0') {
    Write-Host '=== restoring the default (t0) demo ===' -ForegroundColor Cyan
    $err = Install-Config 't0'
    if (-not $err) { $null, $err, $null = Test-Boot }
    if ($err) {
        Write-Host $err -ForegroundColor Red
        Add-Result 't0' 'restore' $false '' '' '' 0 $err
    } else {
        Write-Host 'board is running the t0 demo again'
    }
}

Write-Host ''
$results | Format-Table -AutoSize -Wrap | Out-String -Width 200 | Write-Host
$failed = @($results | Where-Object Result -ne 'PASS').Count
$total = ((Get-Date) - $started).TotalSeconds
if ($failed -or $results.Count -eq 0) {
    Write-Host ("hardware regression FAILED ({0} of {1} rows, {2:N0} s)" -f $failed, $results.Count, $total) `
        -ForegroundColor Red
    exit 1
}
Write-Host ("hardware regression passed ({0} runs, {1:N0} s)" -f $results.Count, $total) -ForegroundColor Green
exit 0
