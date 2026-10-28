# F9 off and on, over and over, in the world (the freeze of the first headset session, 2026-10-18: VR back on
# with F9 and the main loop stalled milliseconds after OpenXR came up). Each cycle: F9 off, a pause, F9 on,
# then up to -Watch seconds for the stall probe; on a stall a small dump is taken at once (stack_dump.py)
# and the run stops. Cycles alternate first person and a sword drawn (third person), and the window's focus
# is taken away and given back while VR is off, as a player in the runtime's home does.
#   -Cycles <n>   -OffSeconds <s>   -Watch <s>   -Dump <path>

param(
    [int]$Cycles = 12,
    [int]$OffSeconds = 4,
    [int]$Watch = 12,
    [string]$Dump = (Join-Path $env:TEMP 'svr_f9_hang.dmp')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\lib\GameIO.ps1')
. (Join-Path $PSScriptRoot '..\lib\VrSession.ps1')
$diag = Join-Path (Split-Path -Parent $PSScriptRoot) 'diagnostics'

Assert-GameFocus -Quiet | Out-Null
for ($i = 1; $i -le $Cycles; $i++) {
    $sword = ($i % 2) -eq 0
    Focus-Game | Out-Null; Start-Sleep -Milliseconds 400
    Send-GameKey $(if ($sword) { '3' } else { '1' }); Start-Sleep 3
    $t0 = Log-Now
    Send-GameKey F9
    $off = Wait-LogSince $t0 'ev=vr\.camera toggle=off' 6
    # The player is in the runtime's home now: the game loses the foreground for a while.
    $away = Start-Process notepad -PassThru
    Start-Sleep $OffSeconds
    Stop-Process -Id $away.Id -ErrorAction SilentlyContinue
    Focus-Game | Out-Null; Start-Sleep -Milliseconds 400
    $t1 = Log-Now
    Send-GameKey F9
    # Meta's runtime takes the foreground when its session starts (the mock does not): the game is behind
    # another window while the session becomes focused, which is when the mod hands the focus back.
    Start-Sleep -Milliseconds 150
    $thief = Start-Process notepad -PassThru
    $init = Wait-LogSince $t1 'ev=vr\.xr init result=ok' 10
    $deadline = (Get-Date).AddSeconds($Watch)
    $stall = $null
    while ((Get-Date) -lt $deadline -and $null -eq $stall) {
        $stall = @(Log-Since $t1 'ev=probe stage=stall result=detected' | Select-Object -First 1)[0]
        $p = Get-Process destiny2 -ErrorAction SilentlyContinue
        if ($null -eq $p) { $stall = 'process gone'; break }
        if ($null -eq $stall -and -not $p.Responding) { Start-Sleep 3; if (-not (Get-Process destiny2).Responding) { $stall = 'window not responding' } }
        Start-Sleep -Milliseconds 500
    }
    if ($thief) { Stop-Process -Id $thief.Id -ErrorAction SilentlyContinue }
    '{0,2} {1,-6} off={2} init={3} {4}' -f $i, $(if ($sword) { 'sword' } else { 'gun' }), [int]($null -ne $off), [int]($null -ne $init), $(if ($stall) { "STALL: $stall" } else { 'ok' })
    if ($stall) {
        if ($stall -ne 'process gone') {
            python (Join-Path $diag 'stack_dump.py') $Dump
            "dump -> $Dump"
        }
        "RESULT stall at cycle $i"
        return
    }
}
Send-GameKey '1'
"RESULT no stall in $Cycles cycles"
