# Puts a "Destiny 2 VR" shortcut on the desktop: the game launched directly (never through Steam),
# from its own folder. Nothing else is needed: VR comes up with the game, and the runtime is chosen
# by the mod itself (`runtime auto` in SVR_Settings.txt: Meta's when installed, docs/PLAYABLE.md).
param(
    [string]$Game = 'C:\Games\Sunrise',
    [string]$Name = 'Destiny 2 VR'
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Game 'destiny2.exe'
if (-not (Test-Path $exe)) { throw "not found: $exe" }
$desktop = [Environment]::GetFolderPath('Desktop')
$link = Join-Path $desktop "$Name.lnk"
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($link)
$shortcut.TargetPath = $exe
$shortcut.WorkingDirectory = $Game
$shortcut.IconLocation = "$exe,0"
$shortcut.Description = 'Destiny 2 (Sunrise) in VR: put the headset on, Quest Link running, then launch.'
$shortcut.Save()
Write-Output "shortcut: $link -> $exe"
