# Watches one field of the player camera block and disassembles the sites from the live process.
#
#   .\Watch-Camera.ps1                      # forward.x, 400 ms, DR0
#   .\Watch-Camera.ps1 -Field fov -Ms 300 -Slot 3
#   .\Watch-Camera.ps1 -Address 0x1F5... -Length 4 -Mode w
#
# Needs the module on (F9) so the probe has logged `ev=vr.probe block addr=`; pass -Toggle to press
# F9 first. Prints the watch summary, each site with registers, and the live disassembly.
param(
    [ValidateSet('pos', 'fwd', 'up', 'fov', 'aspect')][string]$Field = 'fwd',
    [long]$Address = 0,
    [int]$Length = 4,
    [string]$Mode = 'rw',
    [int]$Ms = 400,
    [int]$Slot = 0,
    [switch]$Toggle
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
. (Join-Path (Split-Path -Parent $here) 'lib\GameIO.ps1')
. (Join-Path $here 'MemScan.ps1')
$log = 'C:\Games\Sunrise\bin\x64\Sunrise\logs\sunrise.log'

if ($Toggle) { [void](Focus-Game); Send-GameKey F9; Start-Sleep -Seconds 4 }

if ($Address -eq 0) {
    $line = (Select-String -Path $log -Pattern 'ev=vr.probe block' | Select-Object -Last 1).Line
    if (-not $line) { throw 'no block address in the log yet (module off?)' }
    $block = [Convert]::ToInt64(([regex]::Match($line, 'addr=0x([0-9A-Fa-f]+)').Groups[1].Value), 16)
    $offsets = @{ pos = 0x594; fwd = 0x5BC; up = 0x5C8; fov = 0x5D4; aspect = 0x64C }
    $Address = $block + $offsets[$Field]
    Write-Output ('block 0x{0:X}  {1} at 0x{2:X}' -f $block, $Field, $Address)
}

$report = Start-Watch -Address $Address -Length $Length -Mode $Mode -Ms $Ms -Slot $Slot
$report | Select-String '^watch|^site|^  regs|timeout'
Write-Output '--- log'
Select-String -Path $log -Pattern 'ev=vr.watch (done|foreign)' | Select-Object -Last 9 | ForEach-Object { $_.Line -replace 'client level=warn ', '' }
Write-Output '--- disassembly (live)'
& python (Join-Path $here 'disasm_sites.py') 2>&1
Write-Output ('alive: {0}' -f [bool](Get-Process destiny2 -ErrorAction SilentlyContinue))
