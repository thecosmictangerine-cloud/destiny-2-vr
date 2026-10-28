# Installs or removes Destiny 2 VR in a Project Sunrise game folder. Run through Install.bat or
# Uninstall.bat, which pass -Uninstall for the second.
#
# The mod is one file: a build of Sunrise's steam_api64.dll with VR compiled in. Installing backs
# up the DLL that is there (Sunrise's own) and puts ours in its place; uninstalling puts it back.
# Before touching anything the folder is checked to be a Sunrise install of the 2020 game build,
# because the same DLL in a current Steam copy of Destiny 2 (online, anti-cheat) risks a ban.

param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

$here = $PSScriptRoot
$payload = Join-Path $here 'steam_api64.dll'
$settingsTemplate = Join-Path $here 'SVR_Settings.txt'
$backupName = 'steam_api64.dll.sunrise-original'

# destiny2.exe as the Sunrise Launcher downloads it (depot manifest 7180122903232116872, built
# 2020-08-24). The DLL checks the same two fields at startup (vr_game_check.cpp).
$exeTimeDateStamp = 0x5F43138B
$exeSizeOfImage = 0x08A5EA00

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }

function Stop-WithError([string]$text) {
    Say ''
    Say $text 'Red'
    Say ''
    Say 'Nothing was changed.' 'Yellow'
    exit 1
}

function Test-SunriseExe([string]$exe) {
    $stream = [System.IO.File]::OpenRead($exe)
    try {
        $header = New-Object byte[] 4096
        [void]$stream.Read($header, 0, $header.Length)
    } finally {
        $stream.Dispose()
    }
    $pe = [BitConverter]::ToInt32($header, 0x3C)
    if ($pe -lt 0 -or $pe + 0x60 -gt $header.Length) { return $false }
    $stamp = [BitConverter]::ToUInt32($header, $pe + 8)
    $size = [BitConverter]::ToUInt32($header, $pe + 24 + 56)
    return ($stamp -eq $exeTimeDateStamp) -and ($size -eq $exeSizeOfImage)
}

# Every build of this mod carries the VR module's log tags; Sunrise's own DLL does not.
function Test-VrDll([string]$dll) {
    if (-not (Test-Path -LiteralPath $dll)) { return $false }
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($dll))
    return $text.Contains('ev=vr.display')
}

function Get-LauncherFolder {
    $prefs = Join-Path $env:APPDATA 'io.github.stanuwu.sunrise-launcher\preferences.json'
    if (-not (Test-Path -LiteralPath $prefs)) { return $null }
    try {
        $dir = (Get-Content -LiteralPath $prefs -Raw | ConvertFrom-Json).installDirectory
        if ($dir -and (Test-Path -LiteralPath (Join-Path $dir 'destiny2.exe'))) { return $dir }
    } catch { }
    return $null
}

function Select-Folder {
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Select your Project Sunrise game folder (the one that contains destiny2.exe)'
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return $null }
    return $dialog.SelectedPath
}

function Find-GameFolder {
    # Unzipped straight into the game folder.
    if (Test-Path -LiteralPath (Join-Path $here 'destiny2.exe')) { return $here }
    $found = Get-LauncherFolder
    if ($found) {
        Say "Sunrise Launcher install found: $found"
        $answer = Read-Host 'Use this folder? [Y/n]'
        if ($answer -eq '' -or $answer -match '^[yYsS]') { return $found }
    }
    Say 'Choose the Sunrise game folder in the window that just opened...'
    return Select-Folder
}

function Assert-SunriseFolder([string]$root) {
    if ($root -match '\\steamapps\\') {
        Stop-WithError ("This is a Steam library folder:`n  $root`n`n" +
            "Destiny 2 VR must NEVER go into your normal Steam copy of Destiny 2. That game is online and " +
            "has anti-cheat: a modified steam_api64.dll there can get your account banned.`n" +
            "Install Project Sunrise with its Launcher into its own empty folder, then choose that folder.")
    }
    $exe = Join-Path $root 'destiny2.exe'
    if (-not (Test-Path -LiteralPath $exe)) {
        Stop-WithError "There is no destiny2.exe in:`n  $root`nChoose the folder where the Sunrise Launcher installed the game."
    }
    if (-not (Test-SunriseExe $exe)) {
        Stop-WithError ("The destiny2.exe in this folder is not the 2020 build that Project Sunrise uses:`n  $root`n`n" +
            "If this is your normal Destiny 2, do not install the mod here (online game, anti-cheat, ban risk).`n" +
            "Install Project Sunrise with its Launcher and choose that folder instead.")
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'bin\x64\steam_api64.dll'))) {
        Stop-WithError "bin\x64\steam_api64.dll is missing in:`n  $root`nInstall Project Sunrise with its Launcher first."
    }
}

function Wait-GameClosed {
    while (Get-Process -Name destiny2 -ErrorAction SilentlyContinue) {
        Say 'Destiny 2 is running. Close it, then press Enter.' 'Yellow'
        [void](Read-Host)
    }
}

Say ''
Say '  Destiny 2 VR' 'Cyan'
Say ('  ' + $(if ($Uninstall) { 'Uninstall' } else { 'Install' })) 'Cyan'
Say ''

if (-not $Uninstall -and -not (Test-Path -LiteralPath $payload)) {
    Stop-WithError "steam_api64.dll is not next to this script. Unzip the whole download first, then run Install.bat from there."
}

$root = Find-GameFolder
if (-not $root) { Stop-WithError 'No folder was chosen.' }
$root = (Resolve-Path -LiteralPath $root).Path
Assert-SunriseFolder $root
Wait-GameClosed

$dll = Join-Path $root 'bin\x64\steam_api64.dll'
$backup = Join-Path $root "bin\x64\$backupName"

if ($Uninstall) {
    if (-not (Test-VrDll $dll)) {
        Say 'Destiny 2 VR is not installed in this folder. Nothing to do.' 'Green'
        exit 0
    }
    if (-not (Test-Path -LiteralPath $backup)) {
        Stop-WithError ("The backup of Sunrise's own DLL ($backupName) is missing.`n" +
            "Open the Sunrise Launcher and press Update: it puts the official DLL back.")
    }
    Move-Item -LiteralPath $backup -Destination $dll -Force
    Say 'Done. Sunrise is back to its official DLL.' 'Green'
    Say "Your VR settings file (SVR_Settings.txt) was left in the game folder; delete it if you like."
    exit 0
}

if (Test-VrDll $dll) {
    Say 'A version of Destiny 2 VR is already installed: it will be replaced.'
} else {
    Copy-Item -LiteralPath $dll -Destination $backup -Force
    Say "Sunrise's DLL backed up as bin\x64\$backupName"
}
Copy-Item -LiteralPath $payload -Destination $dll -Force
$settings = Join-Path $root 'SVR_Settings.txt'
if (-not (Test-Path -LiteralPath $settings) -and (Test-Path -LiteralPath $settingsTemplate)) {
    Copy-Item -LiteralPath $settingsTemplate -Destination $settings
    Say 'Settings file created: SVR_Settings.txt (in the game folder)'
}

Say ''
Say "Installed in $root" 'Green'
Say ''
Say 'To play: start your PC VR link (Quest Link / Air Link / Virtual Desktop / SteamVR),'
Say 'then start destiny2.exe from that folder.'
Say ''
Say 'The Sunrise Launcher now shows REPAIR instead of PLAY ("Local files changed"): that is' 'Yellow'
Say 'expected. Do NOT press Repair or Update: they put the official DLL back and remove VR' 'Yellow'
Say '(run Install.bat again if that happens). Launch game, in its settings, is fine.' 'Yellow'
exit 0
