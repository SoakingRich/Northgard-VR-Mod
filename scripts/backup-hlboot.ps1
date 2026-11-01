# Backs up the game's original bytecode once, tagged with the game version.
# Run in PowerShell. Never modifies game files.
$game = "C:\Program Files (x86)\Steam\steamapps\common\Northgard"
$ver  = (Get-Content "$game\shiro.json" -Raw | ConvertFrom-Json).version
$dst  = Join-Path $PSScriptRoot "..\backups\$ver"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item "$game\hlboot.dat" $dst -Force
Get-FileHash "$dst\hlboot.dat" -Algorithm SHA256 | Format-List
Write-Host "Backed up hlboot.dat for version $ver to $dst"
