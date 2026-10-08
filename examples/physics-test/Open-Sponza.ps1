param([ValidateSet('dev','release')][string]$Configuration='release')
$ErrorActionPreference='Stop'
$repository=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$editor=Join-Path $repository "out/$Configuration/atelier_editor.exe"
if(-not (Test-Path -LiteralPath $editor)) { throw "Build atelier_editor for $Configuration first." }
Start-Process -FilePath $editor -WorkingDirectory (Split-Path $editor) -WindowStyle Normal -ArgumentList @(
    '--project', ('"'+$PSScriptRoot+'"'), '--scene', 'Content/Scenes/Sponza.scene',
    '--material-camera', '8', '2', '0', '-6', '3', '0')
