# How many draws the GPU module sees while the title screen is up, for a given set of SVR_Debug.txt
# switches and VR on or off at launch: the quick check that the D3D11 hooks see the game's frames
# (`ev=vr.gpu stats draws_imm=` -- thousands per report period when they do, tens when blind).
param(
    [string[]]$Prep = @(),
    [switch]$StartOn,
    [int]$Seconds = 75
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$game = 'C:\Games\Sunrise'
$log = Join-Path $game 'bin\x64\Sunrise\logs\sunrise.log'
Clear-MockInput
Stop-Game
if ($Prep.Count -gt 0) {
    [System.IO.File]::WriteAllText((Join-Path $game 'SVR_Debug.txt'), (($Prep -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
} else {
    Remove-Item (Join-Path $game 'SVR_Debug.txt') -ErrorAction SilentlyContinue
}
[System.IO.File]::WriteAllText((Join-Path $game 'SVR_Display.txt'), "shape desktop`n", (New-Object System.Text.UTF8Encoding($false)))
$manifest = Enable-MockXr -StartOn:$StartOn
Start-Game -Fresh | Out-Null
$p = Wait-GameWindow -TimeoutSeconds 240
Start-Sleep -Seconds $Seconds
$fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs); $text = $sr.ReadToEnd(); $sr.Close()
$lines = $text -split "`n" | Where-Object { $_ -match 'ev=vr\.gpu stats|d3d_multithread|ev=vr\.xr init' }
Write-Output ("prep=[{0}] start_on={1}" -f ($Prep -join ';'), [bool]$StartOn)
$lines | Select-Object -Last 6 | ForEach-Object { $_.Substring(0, [Math]::Min(170, $_.Length)) }
Stop-Game
Remove-Item (Join-Path $game 'SVR_Debug.txt') -ErrorAction SilentlyContinue
