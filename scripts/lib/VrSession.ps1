# Helpers for driving a VR session on the mock by hand or from a battery: dot-source after GameIO.ps1.
#   Compose-Eye <tag> [<outDir>]       what each eye sees, from the mock's compositor (SVR_seen_<tag>_eye0/1.bmp)
#   Log-Now / Log-Since <t> <pattern>   the structured log, by timestamp (never by counting lines)
#   Send-Cmd <file> <line>              one command file beside the game, waiting until the DLL consumed it

$script:svrGame = 'C:\Games\Sunrise'
$script:svrLog = Join-Path $script:svrGame 'bin\x64\Sunrise\logs\sunrise.log'

function Read-SharedLines([string]$Path) {
    try {
        $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $sr.Close()
        return $text -split "`n"
    } catch { return @() }
}

function Get-LineTime([string]$Line) { if ($Line -match ' t=(\d+) ') { return [int64]$Matches[1] } return [int64]-1 }

function Log-Now {
    $l = @(Read-SharedLines $script:svrLog | Where-Object { $_ -match ' t=\d+ ' } | Select-Object -Last 1)
    if ($l.Count -eq 0) { return [int64]0 }
    return Get-LineTime $l[0]
}

function Log-Since([int64]$After, [string]$Pattern) {
    return @(Read-SharedLines $script:svrLog | Where-Object { $_ -match $Pattern -and (Get-LineTime $_) -gt $After })
}

function Wait-LogSince([int64]$After, [string]$Pattern, [int]$Seconds = 20) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $hit = @(Log-Since $After $Pattern | Select-Object -Last 1)
        if ($hit.Count -gt 0) { return $hit[0] }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

function Send-Cmd([string]$Name, [string[]]$Lines) {
    $p = Join-Path $script:svrGame $Name
    $deadline = (Get-Date).AddSeconds(5)
    while ((Test-Path $p) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
    [IO.File]::WriteAllText($p, (($Lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
    $deadline = (Get-Date).AddSeconds(5)
    while ((Test-Path $p) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
}

function Compose-Eye([string]$Tag, [string]$OutDir, [string]$Words = '') {
    Get-ChildItem $script:svrGame -Filter "SVR_seen_$Tag*" -ErrorAction SilentlyContinue | Remove-Item -Force
    $line = if ($Words) { "$Tag $Words" } else { $Tag }
    [IO.File]::WriteAllText((Join-Path $script:svrGame 'SVR_MockCompose.txt'), "$line`n", (New-Object Text.UTF8Encoding($false)))
    $deadline = (Get-Date).AddSeconds(30)
    while (-not (Test-Path (Join-Path $script:svrGame "SVR_seen_$Tag.txt"))) {
        if ((Get-Date) -gt $deadline) { return $false }
        Start-Sleep -Milliseconds 200
    }
    Start-Sleep -Milliseconds 300
    if ($OutDir) {
        New-Item -ItemType Directory -Force $OutDir | Out-Null
        Get-ChildItem $script:svrGame -Filter "SVR_seen_$Tag*" | Copy-Item -Destination $OutDir -Force
    }
    return $true
}

function ConvertTo-PngEye([string]$Dir, [string]$Tag, [int]$Eye = 0) {
    $bmp = Join-Path $Dir "SVR_seen_${Tag}_eye$Eye.bmp"
    $png = Join-Path $Dir "${Tag}_eye$Eye.png"
    & python -c "from PIL import Image; Image.open(r'$bmp').save(r'$png')"
    return $png
}
