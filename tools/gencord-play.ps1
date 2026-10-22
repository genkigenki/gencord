# Opens a gencord:// link from an Obsidian note in VLC at the given second.
#   gencord://play?t=90&f=%5C%5Cmynas%5C...%5Cx.ogg
# Registered for the current user by install-gencord-links.ps1.
param([Parameter(Mandatory)] [string] $Url, [switch] $DryRun)

$query = ($Url -split '\?', 2)[1]
$q = @{}
foreach ($pair in ($query -split '&')) {
    $k, $v = $pair -split '=', 2
    $q[$k] = [uri]::UnescapeDataString($v)
}
$seconds = [int]($q["t"])
$file = $q["f"]
if (-not $file -or -not $file.StartsWith('\\')) { throw "gencord link without a network file: $Url" }

$vlc = @("$env:ProgramFiles\VideoLAN\VLC\vlc.exe", "${env:ProgramFiles(x86)}\VideoLAN\VLC\vlc.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vlc) { throw "VLC not found" }

if ($DryRun) { "$vlc --start-time=$seconds `"$file`""; return }
Start-Process -FilePath $vlc -ArgumentList "--one-instance", "--start-time=$seconds", "`"$file`""
