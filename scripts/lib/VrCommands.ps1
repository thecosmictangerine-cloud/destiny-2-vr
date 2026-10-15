# Command-file helpers for the VR module: one function per command file the DLL polls.
#
# Every number goes out through InvariantCulture: this machine's locale writes 0,6 and the DLL's
# sscanf stops at the comma, so a plain "$value" makes a command silently do nothing.
#
# Dot-source after GameIO.ps1.

$script:VrGameDir = 'C:\Games\Sunrise'
$script:VrLog = 'C:\Games\Sunrise\bin\x64\Sunrise\logs\sunrise.log'

function ConvertTo-Invariant([object]$Value) {
    if ($Value -is [double] -or $Value -is [single] -or $Value -is [decimal]) {
        return ([double]$Value).ToString([System.Globalization.CultureInfo]::InvariantCulture)
    }
    return "$Value"
}

# Writes lines to a command file, waiting for the DLL to consume the previous one first so two
# commands sent back to back cannot overwrite each other.
function Send-VrFile([string]$Name, [string[]]$Lines, [int]$TimeoutMs = 4000) {
    $path = Join-Path $script:VrGameDir $Name
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Test-Path $path) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, (($Lines -join "`n") + "`n"), $enc)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Test-Path $path) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
    return -not (Test-Path $path)
}

function Send-Gpu([string[]]$Lines) { Send-VrFile 'SVR_Gpu.txt' $Lines }
function Send-Arms([string[]]$Lines) { Send-VrFile 'SVR_Arms.txt' $Lines }
function Send-Weapon([string[]]$Lines) { Send-VrFile 'SVR_Weapon.txt' $Lines }

# Reads the log without fighting the writer for the handle.
function Read-VrLog([int]$Tail = 4000) {
    $fs = [System.IO.File]::Open($script:VrLog, 'Open', 'Read', 'ReadWrite')
    try {
        $sr = New-Object System.IO.StreamReader($fs)
        $all = $sr.ReadToEnd()
    } finally { $fs.Dispose() }
    $lines = $all -split "`n"
    if ($lines.Count -gt $Tail) { $lines = $lines[($lines.Count - $Tail)..($lines.Count - 1)] }
    return $lines
}

function Get-VrLog([string]$Pattern, [int]$Last = 5) {
    Read-VrLog | Where-Object { $_ -match $Pattern } | Select-Object -Last $Last |
        ForEach-Object { $_.Substring(0, [Math]::Min(600, $_.Length)).TrimEnd() }
}

# Arms a one-frame GPU capture and waits until the DLL reports it written.
function Invoke-GpuCapture([int]$TimeoutSeconds = 30) {
    $before = @(Read-VrLog | Where-Object { $_ -match 'ev=vr\.gpu capture done' }).Count
    [void](Send-Gpu @('capture'))
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $now = @(Read-VrLog | Where-Object { $_ -match 'ev=vr\.gpu capture done' })
        if ($now.Count -gt $before) { return $now[-1] }
    }
    return $null
}
