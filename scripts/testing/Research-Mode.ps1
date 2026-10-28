# Research for docs/SWORD-SPARROW.md: what the engine does in first person, with a sword and on a sparrow.
# Runs on a session already in the world (Test-Lifecycle.ps1 -SkipEvents), sword in the power slot.
# For each phase: a vr_mode trace (SVR_ModeTrace.csv: engine camera, physics component and body, first-person
# signals, one line per camera frame) and dumps of the component and the body (SVR_ModeDump_*.txt).

param(
    [string]$Out = (Join-Path $env:TEMP 'svr_mode_research'),
    [string[]]$Phases = @('fp', 'sword', 'sparrow')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\lib\GameIO.ps1')
New-Item -ItemType Directory -Force $Out | Out-Null
$game = 'C:\Games\Sunrise'
$log = Join-Path $game 'bin\x64\Sunrise\logs\sunrise.log'

function Read-Shared([string]$Path) {
    $f = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try { (New-Object IO.StreamReader($f)).ReadToEnd() } finally { $f.Close() }
}

function Wait-LogAfter([string]$Pattern, [long]$Length, [int]$Seconds = 30) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $text = Read-Shared $log
        if ($text.Length -gt $Length) {
            $m = [regex]::Matches($text.Substring($Length), $Pattern)
            if ($m.Count -gt 0) { return $m[$m.Count - 1].Value }
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Send-Mode([string]$Line) {
    $p = Join-Path $game 'SVR_Mode.txt'
    $deadline = (Get-Date).AddSeconds(5)
    while ((Test-Path $p) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
    [IO.File]::WriteAllText($p, "$Line`n", (New-Object Text.UTF8Encoding($false)))
}

function Trace([string]$Tag, [int]$Frames, [scriptblock]$During) {
    $len = (Read-Shared $log).Length
    Send-Mode "trace $Frames"
    if ($During) { & $During }
    $done = Wait-LogAfter 'ev=vr\.mode trace written rows=\d+' $len 60
    Copy-Item (Join-Path $game 'SVR_ModeTrace.csv') (Join-Path $Out "trace_$Tag.csv") -Force
    "trace $Tag : $done"
}

function Dump([string]$Tag) {
    foreach ($what in @('comp 20000', 'body 1024')) {
        $len = (Read-Shared $log).Length
        Send-Mode "dump $what"
        $r = Wait-LogAfter 'ev=vr\.mode dump result=\w+ what=\d [^\r\n]*' $len 20
        "dump $Tag $what : $r"
    }
    Copy-Item (Join-Path $game 'SVR_ModeDump_comp.txt') (Join-Path $Out "comp_$Tag.txt") -Force -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $game 'SVR_ModeDump_body.txt') (Join-Path $Out "body_$Tag.txt") -Force -ErrorAction SilentlyContinue
}

$rest = @{ HandY = 0.25; HandZ = -0.1 }
Assert-GameFocus -Quiet | Out-Null
Set-MockInput @rest | Out-Null

if ($Phases -contains 'fp') {
    Send-GameKey '1'; Start-Sleep 3
    Dump 'fp'
    Trace 'fp' 500 { Start-Sleep -Milliseconds 1500; Set-MockInput @rest -MoveY 1 | Out-Null; Start-Sleep 3; Set-MockInput @rest | Out-Null }
    Save-Shot (Join-Path $Out 'fp.png') | Out-Null
}
if ($Phases -contains 'sword') {
    Send-GameKey '3'; Start-Sleep 4
    Dump 'sword'
    Trace 'sword' 500 { Start-Sleep -Milliseconds 1500; Set-MockInput @rest -MoveY 1 | Out-Null; Start-Sleep 3; Set-MockInput @rest | Out-Null }
    Save-Shot (Join-Path $Out 'sword.png') | Out-Null
    Send-GameKey '1'; Start-Sleep 3
}
if ($Phases -contains 'sparrow') {
    Send-GameKey TAB -HoldMs 100; Start-Sleep -Milliseconds 600
    Send-GameKey E -HoldMs 1800; Start-Sleep 5
    Save-Shot (Join-Path $Out 'sparrow.png') | Out-Null
    Dump 'sparrow'
    # Straight, then a turn the body servo makes by itself (the head turned 40 degrees left).
    Trace 'sparrow' 900 {
        Start-Sleep 1; Set-MockInput @rest -MoveY 1 | Out-Null; Start-Sleep 3
        Set-MockInput @rest -MoveY 1 -HeadYaw 40 | Out-Null; Start-Sleep 3
        Set-MockInput @rest | Out-Null
    }
    Save-Shot (Join-Path $Out 'sparrow_after.png') | Out-Null
    Send-GameKey SPACE; Start-Sleep 3
}
"done -> $Out"
