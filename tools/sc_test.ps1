<#
.SYNOPSIS
  Exercise the iso7816-pico card through any PC/SC reader (Windows winscard.dll).

.DESCRIPTION
  No extra software needed: talks to winscard.dll directly.  Connects to the
  first reader whose name matches -Reader, prints the ATR and protocol, then
  runs the demo card's commands (applet, file system, PINs, channels,
  chaining, extended length) and checks the status words.  61 xx
  (GET RESPONSE) and 6C xx (wrong Le) are handled here in case the reader
  driver does not.

.PARAMETER Reader
  Substring of the reader name to use (default: SCR33).

.PARAMETER Protocol
  T0, T1 or Any (default).  Must match what the card's ATR offers.

.PARAMETER Bootsel
  Also send the REBOOT BOOTSEL APDU (80 E0 00 00 00) at the end.

.PARAMETER Apdu
  Send one arbitrary APDU (hex string, e.g. "80 10 00 00 00") instead of the test suite.
#>
param(
    [string]$Reader = 'SCR33',
    [ValidateSet('T0', 'T1', 'Any')] [string]$Protocol = 'Any',
    [switch]$Bootsel,
    [string]$Apdu
)

$ErrorActionPreference = 'Stop'

if (-not ('WinSCard' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class WinSCard {
    [StructLayout(LayoutKind.Sequential)]
    public struct SCARD_IO_REQUEST { public int dwProtocol; public int cbPciLength; }

    [DllImport("winscard.dll")]
    public static extern int SCardEstablishContext(int scope, IntPtr r1, IntPtr r2, out IntPtr ctx);
    [DllImport("winscard.dll")]
    public static extern int SCardReleaseContext(IntPtr ctx);
    [DllImport("winscard.dll", CharSet = CharSet.Unicode, EntryPoint = "SCardListReadersW")]
    public static extern int SCardListReaders(IntPtr ctx, string groups, byte[] readers, ref int len);
    [DllImport("winscard.dll", CharSet = CharSet.Unicode, EntryPoint = "SCardConnectW")]
    public static extern int SCardConnect(IntPtr ctx, string reader, int share, int protocols,
                                          out IntPtr card, out int proto);
    [DllImport("winscard.dll")]
    public static extern int SCardDisconnect(IntPtr card, int disposition);
    [DllImport("winscard.dll")]
    public static extern int SCardReconnect(IntPtr card, int share, int protocols, int init, out int proto);
    [DllImport("winscard.dll", CharSet = CharSet.Unicode, EntryPoint = "SCardStatusW")]
    public static extern int SCardStatus(IntPtr card, byte[] name, ref int nameLen, out int state, out int proto,
                                         byte[] atr, ref int atrLen);
    [DllImport("winscard.dll")]
    public static extern int SCardTransmit(IntPtr card, ref SCARD_IO_REQUEST sendPci, byte[] send, int sendLen,
                                           IntPtr recvPci, byte[] recv, ref int recvLen);
}
"@
}

function Hex([byte[]]$b) {
    if ($null -eq $b -or $b.Length -eq 0) { return '' }
    ($b | ForEach-Object { $_.ToString('X2') }) -join ' '
}
function FromHex([string]$s) {
    $pairs = ($s -replace '[^0-9A-Fa-f]', '') -split '(..)' | Where-Object { $_ }
    [byte[]]($pairs | ForEach-Object { [Convert]::ToByte($_, 16) })
}
function Check($rc, $what) { if ($rc -ne 0) { throw ("{0} failed: 0x{1:X8}" -f $what, $rc) } }

$protoMask = switch ($Protocol) { 'T0' { 1 } 'T1' { 2 } default { 3 } }

# --- connect -----------------------------------------------------------------
$ctx = [IntPtr]::Zero
Check ([WinSCard]::SCardEstablishContext(0, [IntPtr]::Zero, [IntPtr]::Zero, [ref]$ctx)) 'SCardEstablishContext'

$len = 0
Check ([WinSCard]::SCardListReaders($ctx, $null, $null, [ref]$len)) 'SCardListReaders'
$buf = New-Object byte[] ($len * 2)
Check ([WinSCard]::SCardListReaders($ctx, $null, $buf, [ref]$len)) 'SCardListReaders'
$readers = [Text.Encoding]::Unicode.GetString($buf, 0, $len * 2).Split([char]0) | Where-Object { $_ }
$name = $readers | Where-Object { $_ -like "*$Reader*" } | Select-Object -First 1
if (-not $name) { throw "no reader matching '$Reader' (found: $($readers -join ', '))" }
Write-Host "reader : $name"

$card = [IntPtr]::Zero; $proto = 0
$rc = [WinSCard]::SCardConnect($ctx, $name, 2 <# SHARED #>, $protoMask, [ref]$card, [ref]$proto)
if ($rc -eq 0x80100066 -or $rc -eq 0x80100067) {
    # Windows remembers a "mute" card until it is re-inserted.  A direct
    # connection plus a reconnect-with-reset makes the reader try again,
    # which is what you want after reflashing the Pico.
    Write-Host "card marked mute by Windows; forcing a cold reset ..."
    Check ([WinSCard]::SCardConnect($ctx, $name, 3 <# DIRECT #>, 0, [ref]$card, [ref]$proto)) 'SCardConnect(direct)'
    $rc = [WinSCard]::SCardReconnect($card, 2, $protoMask, 1 <# RESET_CARD #>, [ref]$proto)
    if ($rc -ne 0) {
        [void][WinSCard]::SCardDisconnect($card, 0)
        Start-Sleep -Milliseconds 500
        $rc = [WinSCard]::SCardConnect($ctx, $name, 2, $protoMask, [ref]$card, [ref]$proto)
    }
}
Check $rc 'SCardConnect'

$nameLen = 0; $state = 0; $p = 0; $atrLen = 33
$atr = New-Object byte[] 33
Check ([WinSCard]::SCardStatus($card, $null, [ref]$nameLen, [ref]$state, [ref]$p, $atr, [ref]$atrLen)) 'SCardStatus'
$atr = $atr[0..($atrLen - 1)]
$protoName = switch ($proto) { 1 { 'T=0' } 2 { 'T=1' } default { "0x$($proto.ToString('X'))" } }
Write-Host "ATR    : $(Hex $atr)   ($protoName)"

$pci = New-Object WinSCard+SCARD_IO_REQUEST
$pci.dwProtocol = $proto; $pci.cbPciLength = 8

function Transmit([byte[]]$apdu) {
    # Returns data (byte[]) and SW (int); follows 61 xx / 6C xx itself.
    $data = @()
    for ($hop = 0; $hop -lt 40; $hop++) {
        $recv = New-Object byte[] 4200; $recvLen = $recv.Length
        $rc = [WinSCard]::SCardTransmit($script:card, [ref]$script:pci, $apdu, $apdu.Length, [IntPtr]::Zero,
                                        $recv, [ref]$recvLen)
        Check $rc 'SCardTransmit'
        if ($recvLen -lt 2) { throw "short response" }
        $sw1 = $recv[$recvLen - 2]; $sw2 = $recv[$recvLen - 1]
        if ($recvLen -gt 2) { $data += $recv[0..($recvLen - 3)] }
        if ($sw1 -eq 0x6C) { $apdu = [byte[]]($apdu[0..3] + $sw2); continue }
        if ($sw1 -eq 0x61) { $apdu = [byte[]](0x00, 0xC0, 0x00, 0x00, $sw2); continue }
        return @{ data = [byte[]]$data; sw = ($sw1 * 256 + $sw2) }
    }
    throw "too many 61/6C hops"
}

$script:fail = 0
function Step($label, [string]$apduHex, [int]$expectSw, [string]$expectDataHex, [int]$expectLen = -1) {
    $r = Transmit (FromHex $apduHex)
    $ok = ($r.sw -eq $expectSw)
    if ($expectDataHex) { $ok = $ok -and ((Hex $r.data) -eq ((Hex (FromHex $expectDataHex)))) }
    if ($expectLen -ge 0) { $ok = $ok -and ($r.data.Length -eq $expectLen) }
    $tag = if ($ok) { 'PASS' } else { 'FAIL'; $script:fail++ }
    $shown = Hex $r.data
    if ($shown.Length -gt 60) { $shown = $shown.Substring(0, 60) + " ..($($r.data.Length) bytes)" }
    elseif ($r.data.Length -gt 0 -and ($r.data | Where-Object { $_ -lt 0x20 -or $_ -gt 0x7E }).Count -eq 0) {
        $shown += '  "' + [Text.Encoding]::ASCII.GetString($r.data) + '"'
    }
    $shownApdu = if ($apduHex.Length -gt 44) { $apduHex.Substring(0, 44) + '..' } else { $apduHex }
    Write-Host ("[{0}] {1,-30} {2,-46} -> SW {3:X4} {4}" -f $tag, $label, $shownApdu, $r.sw, $shown)
    return $r
}

try {
    if ($Apdu) {
        $r = Transmit (FromHex $Apdu)
        Write-Host ("-> SW {0:X4} {1}" -f $r.sw, (Hex $r.data))
        return
    }

    $seq200 = (1..200 | ForEach-Object { ($_ % 256).ToString('X2') }) -join ' '
    $hello16 = '50 69 63 6F 21 20 66 72 6F 6D 20 69 73 6F 37 38'
    $rec1 = '72 65 63 6F 72 64 20 31 20 20 20 20 20 20 20 20'
    $rec2 = '72 65 63 6F 72 64 20 32 20 20 20 20 20 20 20 20'
    $rec3 = '74 68 69 72 64 20 72 65 63 6F 72 64 20 20 20 20'

    Write-Host ''
    Write-Host '--- demo applet (CLA 80) ---'
    [void](Step 'SELECT by AID'       '00 A4 04 00 05 F0 50 49 43 4F' 0x9000 '6F 07 84 05 F0 50 49 43 4F')
    [void](Step 'SELECT unknown AID'  '00 A4 04 00 02 11 22'          0x6A82)
    [void](Step 'GET VERSION'         '80 10 00 00 00'                0x9000)
    [void](Step 'ECHO'                '80 20 00 00 04 DE AD BE EF'    0x9000 'DE AD BE EF')
    $c1 = Step 'GET COUNTER'          '80 30 00 00 04'                0x9000
    $c2 = Step 'GET COUNTER (Le=00)'  '80 30 00 00 00'                0x9000
    $n1 = [BitConverter]::ToUInt32([byte[]]($c1.data[3..0]), 0)
    $n2 = [BitConverter]::ToUInt32([byte[]]($c2.data[3..0]), 0)
    if ($n2 -ne $n1 + 1) { Write-Host "[FAIL] counter did not increment ($n1 -> $n2)"; $script:fail++ }
    else { Write-Host "[PASS] counter incremented ($n1 -> $n2)" }
    [void](Step 'ECHO 200 bytes'      "80 20 00 00 C8 $seq200"       0x9000 $seq200)
    [void](Step 'SLOW (keep-alive 2 s)' '80 40 00 00 00'              0x9000 '64 6F 6E 65')
    [void](Step 'unknown INS'         '00 11 00 00 00'                0x6D00)
    [void](Step 'unknown CLA'         '90 10 00 00 00'                0x6E00)

    Write-Host ''
    Write-Host '--- transparent EF 0101 + PIN 01 ---'
    [void](Step 'UPDATE BINARY (no PIN)' '00 D6 00 00 03 41 42 43'   0x6982)
    [void](Step 'VERIFY wrong PIN'    '00 20 00 01 04 30 30 30 30'    0x63C2)
    [void](Step 'VERIFY status'       '00 20 00 01 00'                0x63C2)
    [void](Step 'VERIFY 1234'         '00 20 00 01 04 31 32 33 34'    0x9000)
    [void](Step 'VERIFY status'       '00 20 00 01 00'                0x9000)
    [void](Step 'UPDATE BINARY'       '00 D6 00 00 05 50 69 63 6F 21' 0x9000)
    [void](Step 'READ BINARY 16'      '00 B0 00 00 10'                0x9000 $hello16)
    [void](Step 'READ BINARY 256'     '00 B0 00 00 00'                0x9000 '' 256)
    [void](Step 'READ BINARY via SFI 1' '00 B0 81 05 04'              0x9000 '20 66 72 6F')
    [void](Step 'READ BINARY bad offset' '00 B0 01 00 10'             0x6B00)
    [void](Step 'UPDATE chained (1/2)' '10 D6 00 F0 04 43 48 41 49'   0x9000)
    [void](Step 'UPDATE chained (2/2)' '00 D6 00 F0 04 4E 45 44 21'   0x9000)
    [void](Step 'READ BINARY @F0'     '00 B0 00 F0 08'                0x9000 '43 48 41 49 4E 45 44 21')
    [void](Step 'CHANGE REF DATA 1234->4321' '00 24 00 01 08 31 32 33 34 34 33 32 31' 0x9000)
    [void](Step 'VERIFY 4321'         '00 20 00 01 04 34 33 32 31'    0x9000)
    [void](Step 'CHANGE REF DATA back' '00 24 00 01 08 34 33 32 31 31 32 33 34' 0x9000)
    [void](Step 'RESET RETRY (PUK)'   '00 2C 01 01 08 31 32 33 34 35 36 37 38' 0x9000)

    Write-Host ''
    Write-Host '--- SELECT by FID / path, FCP ---'
    [void](Step 'SELECT MF'           '00 A4 00 00 02 3F 00'          0x9000)
    [void](Step 'SELECT DF 7F10 (FCP)' '00 A4 01 04 02 7F 10'         0x9000)
    [void](Step 'SELECT EF 0102, no data' '00 A4 02 0C 02 01 02'      0x9000 '' 0)
    [void](Step 'SELECT parent'       '00 A4 03 00 00'                0x9000)
    [void](Step 'SELECT path from MF' '00 A4 08 04 04 7F 10 01 01'    0x9000)
    [void](Step 'SELECT by DF name'   '00 A4 04 00 05 F0 50 49 43 4F' 0x9000)
    [void](Step 'SELECT missing FID'  '00 A4 02 00 02 01 99'          0x6A82)

    Write-Host ''
    Write-Host '--- records (EF 0102 linear fixed, 0103 cyclic, 0104 linear variable) ---'
    [void](Step 'READ RECORD 1 (SFI 2)' '00 B2 01 14 10'              0x9000 $rec1)
    [void](Step 'READ RECORD 2'       '00 B2 02 04 10'                0x9000 $rec2)
    [void](Step 'READ RECORD next (3)' '00 B2 00 02 10'               0x9000 '' 16)
    [void](Step 'READ RECORD 5'       '00 B2 05 04 10'                0x6A83)
    [void](Step 'UPDATE RECORD 3'     '00 DC 03 04 10 74 68 69 72 64 20 72 65 63 6F 72 64 20 20 20 20' 0x9000)
    [void](Step 'READ RECORD 3'       '00 B2 03 04 10'                0x9000 $rec3)
    [void](Step 'SEARCH RECORD "rec"' '00 A2 01 04 03 72 65 63'       0x9000 '01 02')
    [void](Step 'APPEND RECORD cyclic' '00 E2 00 18 08 01 02 03 04 05 06 07 08' 0x9000)
    [void](Step 'APPEND RECORD cyclic' '00 E2 00 18 08 11 12 13 14 15 16 17 18' 0x9000)
    [void](Step 'READ RECORD 1 cyclic' '00 B2 01 1C 08'               0x9000 '11 12 13 14 15 16 17 18')
    [void](Step 'READ RECORD 2 cyclic' '00 B2 02 1C 08'               0x9000 '01 02 03 04 05 06 07 08')
    [void](Step 'SELECT EF 0104'      '00 A4 02 0C 02 01 04'          0x9000)
    [void](Step 'READ RECORD (PIN 81)' '00 B2 01 04 00'               0x6982)
    [void](Step 'VERIFY PIN 81'       '00 20 00 81 04 30 30 30 30'    0x9000)
    [void](Step 'UPDATE RECORD 1 (var)' '00 DC 01 04 03 61 62 63'     0x9000)
    [void](Step 'READ RECORD 1 (var)' '00 B2 01 04 00'                0x9000 '61 62 63')

    Write-Host ''
    Write-Host '--- GET DATA, GET CHALLENGE, authentication hooks ---'
    [void](Step 'GET DATA 0100'       '00 CA 01 00 00'                0x9000 '00 11 22 33 44 55 66 77')
    [void](Step 'GET DATA unknown'    '00 CA 03 00 00'                0x6A88)
    $ch = Step 'GET CHALLENGE'        '00 84 00 00 08'                0x9000 '' 8
    $crypto = ($ch.data | ForEach-Object { ($_ -bxor 0x5A).ToString('X2') }) -join ' '
    [void](Step 'EXTERNAL AUTH (good)' "00 82 00 00 08 $crypto"       0x9000)
    [void](Step 'EXTERNAL AUTH (no challenge)' "00 82 00 00 08 $crypto" 0x6985)
    [void](Step 'INTERNAL AUTH'       '00 88 00 00 04 01 02 03 04 00' 0x9000 '5B 58 59 5E')
    [void](Step 'PSO (no hook)'       '00 2A 80 86 02 01 02'          0x6A81)

    Write-Host ''
    Write-Host '--- logical channels ---'
    [void](Step 'MANAGE CHANNEL open' '00 70 00 00 01'                0x9000 '01')
    [void](Step 'SELECT AID on ch 1'  '01 A4 04 00 05 F0 50 49 43 4F' 0x9000)
    [void](Step 'READ BINARY on ch 1' '01 B0 00 00 05'                0x9000 '50 69 63 6F 21')
    [void](Step 'UPDATE on ch 1 (no PIN there)' '01 D6 00 00 01 41'   0x6982)
    [void](Step 'MANAGE CHANNEL close' '00 70 80 01 00'               0x9000)
    [void](Step 'READ BINARY on closed ch' '01 B0 00 00 05'           0x6881)

    Write-Host ''
    Write-Host '--- extended length ---'
    if ($proto -eq 1) {
        [void](Step 'ENVELOPE (ext. ECHO)' '00 C2 00 00 0B 80 20 00 00 00 00 04 DE AD BE EF' 0x9000 'DE AD BE EF')
        $big = ((1..300 | ForEach-Object { ($_ % 256).ToString('X2') }) -join ' ')
        $part1 = ($big -split ' ')[0..247] -join ' '
        [void](Step 'ENVELOPE 300 B (1/2)' "00 C2 00 00 FF 80 20 00 00 00 01 2C $part1" 0x9000)
        [void](Step 'ENVELOPE 300 B (2/2)' "00 C2 00 00 34 $(($big -split ' ')[248..299] -join ' ')" 0x9000 $big)
    } else {
        [void](Step 'ECHO 4E (ext. Lc/Le)' '80 20 00 00 00 00 04 DE AD BE EF 00 00' 0x9000 'DE AD BE EF')
        $big = ((1..1000 | ForEach-Object { ($_ % 256).ToString('X2') }) -join ' ')
        [void](Step 'ECHO 1000 B extended' "80 20 00 00 00 03 E8 $big 00 00" 0x9000 $big)
        [void](Step 'READ BINARY 256 (SFI 1)' '00 B0 81 00 00'        0x9000 '' 256)
    }

    Write-Host ''
    if ($script:fail) { Write-Host "$($script:fail) check(s) FAILED" -ForegroundColor Red }
    else { Write-Host 'all checks passed' -ForegroundColor Green }

    if ($Bootsel) {
        [void](Step 'REBOOT BOOTSEL'  '80 E0 00 00 00'                0x9000)
        Write-Host 'the Pico should now show up as the RPI-RP2 drive'
    }
}
finally {
    # Reset the card on disconnect: Windows otherwise keeps it powered and the next run continues the same session
    # (security status kept, nothing written back to the card's NVM).
    [void][WinSCard]::SCardDisconnect($card, 1 <# RESET_CARD #>)
    [void][WinSCard]::SCardReleaseContext($ctx)
}
