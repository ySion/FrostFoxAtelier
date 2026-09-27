param([string]$SharedToolsRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$lock = Get-Content (Join-Path $root 'cmake/toolchain-lock.json') -Raw | ConvertFrom-Json
$localTools = Join-Path $root '.tools'
$llvm = Join-Path $localTools "llvm-$($lock.llvm)"
if (-not (Test-Path (Join-Path $llvm 'bin/clang-cl.exe'))) {
    if (-not $SharedToolsRoot) { throw 'Supply -SharedToolsRoot pointing to the existing pinned .tools directory.' }
    $shared = Join-Path $SharedToolsRoot "llvm-$($lock.llvm)"
    if (-not (Test-Path (Join-Path $shared 'bin/clang-cl.exe'))) { throw 'Shared pinned LLVM is missing.' }
    New-Item -ItemType Directory -Path $localTools -Force | Out-Null
    New-Item -ItemType Junction -Path $llvm -Target $shared | Out-Null
}
. (Join-Path $root 'tools/enter-dev.ps1')
