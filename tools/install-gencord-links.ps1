# Registers gencord:// for the current Windows user (no admin): clicking a timestamp in an
# Obsidian note opens the recording in VLC at that second. Undo: -Uninstall.
param([switch] $Uninstall)

$key = 'HKCU:\Software\Classes\gencord'
$target = Join-Path $env:LOCALAPPDATA 'gencord\gencord-play.ps1'

if ($Uninstall) {
    Remove-Item $key -Recurse -ErrorAction SilentlyContinue
    Remove-Item (Split-Path $target) -Recurse -ErrorAction SilentlyContinue
    'gencord:// links removed'
    return
}

New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'gencord-play.ps1') $target -Force

New-Item $key -Force | Out-Null
Set-ItemProperty $key '(default)' 'URL:gencord recording'
Set-ItemProperty $key 'URL Protocol' ''
New-Item "$key\shell\open\command" -Force | Out-Null
$cmd = "powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$target`" `"%1`""
Set-ItemProperty "$key\shell\open\command" '(default)' $cmd
"gencord:// registered -> $target"
