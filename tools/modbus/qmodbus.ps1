# qmodbus.ps1 -- a minimal Modbus RTU master, to drive a quorum board the way a PLC would.
#
#   qmodbus.ps1 -Port COM3 info          identity, fingerprint, and the current decision
#   qmodbus.ps1 -Port COM3 sweep         write every state's presence mask, read the decision back, print one line:
#                                        MODBUS <casebook> states N fnv1a 0x........ -- test_modbus prints the same
#                                        line for the same image, so equal lines mean the board answered every state
#                                        over Modbus exactly as the host did
#   qmodbus.ps1 -Port COM3 guard -State 0x2a  propose each action in the quiet state and in state 0x2a, print the guard
#   qmodbus.ps1 -Port COM3 held          drive the held-demo casebook's raw channels over time (5 s hold, staleness)
#
# Register map: runtime/GUARD_AND_MODBUS.md. Slave address 1. The board switches to Modbus on the first valid request,
# which may arrive mid-report and be lost, so the first request is retried until a well-formed answer arrives.
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [Parameter(Mandatory = $true)][ValidateSet('info', 'sweep', 'guard', 'held')][string]$Command,
    [int]$States = -1,
    [int]$State = -1   # for guard: a second state to try, e.g. one the rulebook decides for its safe action
)
$ErrorActionPreference = 'Stop'

function Get-Crc([byte[]]$b, [int]$n) {
    $c = 0xFFFF
    for ($i = 0; $i -lt $n; $i++) {
        $c = $c -bxor $b[$i]
        for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = ($c -shr 1) -bxor 0xA001 } else { $c = $c -shr 1 } }
    }
    return $c
}
function Add-Crc([byte[]]$b) {
    $c = Get-Crc $b $b.Length
    return [byte[]]($b + @([byte]($c -band 0xFF), [byte]($c -shr 8)))
}

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.DtrEnable = $true; $sp.RtsEnable = $true; $sp.ReadTimeout = 300; $sp.WriteTimeout = 1000
$sp.Open()

function Invoke-Frame([byte[]]$req, [int]$expect) {
    for ($try = 0; $try -lt 40; $try++) {
        $sp.DiscardInBuffer()
        $sp.Write($req, 0, $req.Length)
        $buf = New-Object byte[] 512
        $got = 0
        $deadline = [DateTime]::UtcNow.AddMilliseconds(400)
        while ($got -lt $expect -and [DateTime]::UtcNow -lt $deadline) {
            try { $got += $sp.Read($buf, $got, 512 - $got) } catch [System.TimeoutException] { }
            if ($got -ge 5 -and ($buf[1] -band 0x80)) { $expect = 5 }
        }
        if ($got -ge $expect -and $buf[0] -eq 1) {
            $c = Get-Crc $buf ($expect - 2)
            if ($buf[$expect - 2] -eq ($c -band 0xFF) -and $buf[$expect - 1] -eq ($c -shr 8)) {
                if ($buf[1] -band 0x80) { throw "Modbus exception $($buf[2]) for function $($req[1])" }
                return $buf[0..($expect - 1)]
            }
        }
        Start-Sleep -Milliseconds 30  # lets a board that was mid-report finish switching over
    }
    throw "no valid answer from $Port"
}
function Read-Regs([int]$addr, [int]$count) {
    $r = Invoke-Frame (Add-Crc ([byte[]](1, 3, ($addr -shr 8), ($addr -band 0xFF), 0, $count))) (5 + 2 * $count)
    $v = @()
    for ($i = 0; $i -lt $count; $i++) { $v += ([int]$r[3 + 2 * $i] -shl 8) -bor $r[4 + 2 * $i] }
    return , $v
}
function Write-Regs([int]$addr, [int[]]$vals) {
    $b = [byte[]](1, 16, ($addr -shr 8), ($addr -band 0xFF), 0, $vals.Count, (2 * $vals.Count))
    foreach ($x in $vals) { $b += [byte](($x -shr 8) -band 0xFF); $b += [byte]($x -band 0xFF) }
    $null = Invoke-Frame (Add-Crc $b) 8
}

$id = Read-Regs 0 10
if ($id[0] -ne 0x5155) { throw "not a quorum board (register 0 = $($id[0]))" }
$fp = ('0x{0:x4}{1:x4}' -f $id[2], $id[3])
$obs = $id[4]
"board on $Port  map v$($id[1])  fingerprint $fp  observations $obs  members $($id[5])  channels $($id[6])"

switch ($Command) {
    'info' {
        $m = Read-Regs 16 9
        "decision: outcome $($m[1]), action position $($m[2]), fallback $($m[3]), $($m[8]) actions, $($m[7]) steps"
    }
    'sweep' {
        $n = if ($States -gt 0) { $States } else { [int][Math]::Pow(2, $obs) }
        $fnv = [uint32]2166136261
        $t0 = Get-Date
        for ($s = 0; $s -lt $n; $s++) {
            Write-Regs 8 @(($s -shr 16), ($s -band 0xFFFF))
            $o = Read-Regs 17 2
            $fnv = [uint32](([uint64]($fnv -bxor $o[0]) * 16777619) % 4294967296)
            $fnv = [uint32](([uint64]($fnv -bxor $o[1]) * 16777619) % 4294967296)
            if ($s % 2048 -eq 2047) { Write-Host ("  {0} states, {1:n0} s" -f ($s + 1), ((Get-Date) - $t0).TotalSeconds) }
        }
        ('MODBUS states {0} fnv1a 0x{1:x8}  ({2:n1} s, {3:n1} ms per state, write + read)' -f $n, $fnv,
            ((Get-Date) - $t0).TotalSeconds, (((Get-Date) - $t0).TotalMilliseconds / $n))
    }
    'guard' {
        foreach ($state in @(0, $State)) {
            if ($state -lt 0) { continue }
            Write-Regs 8 @(($state -shr 16), ($state -band 0xFFFF))
            $count = (Read-Regs 16 9)[8]
            for ($k = 0; $k -lt $count; $k++) {
                Write-Regs 16 @($k)
                $o = Read-Regs 16 9
                ('state 0x{0:x4}: rulebook says action {1}; proposal {2} -> act on {3}, accepted {4}, reason {5}' -f $state,
                    $o[2], $k, $o[4], $o[5], $o[6])
            }
            Write-Regs 16 @(0xFFFF)
        }
    }
    'held' {
        $names = @('carry_on', 'inspect', 'trip')
        function Sample([int]$ch, [int]$milli) { Write-Regs (256 + 2 * $ch) @((($milli -shr 16) -band 0xFFFF), ($milli -band 0xFFFF)) }
        function Show([string]$what) { $o = Read-Regs 16 9; "{0,6:n1} s  {1,-46} -> {2}" -f ((Get-Date) - $t0).TotalSeconds, $what, $names[$o[2]] }
        $t0 = Get-Date
        Show 'no samples yet (channels blind)'
        Sample 0 95000; Sample 1 2000; Show 'bearing 95.0 C, vibration 2.0: hold starts'
        for ($i = 1; $i -le 6; $i++) { Start-Sleep -Milliseconds 1000; Sample 0 95000; Sample 1 2000; Show "still 95.0 C after $i s" }
        Sample 1 12000; Show 'vibration 12.0 mm/s (no hold)'
        Sample 1 2000; Sample 0 84000; Show 'bearing 84.0 C: below release'
        Start-Sleep -Milliseconds 6000; Show 'no samples for 6 s: vibration stale'
    }
}
$sp.Close()
