# Dot-source this file to prepare the current PowerShell session.
# It locates dependencies; CMake remains the only build definition.
param([switch]$InstallToolchain)

$ErrorActionPreference = 'Stop'
$atelierRoot = Split-Path -Parent $PSScriptRoot
$atelierLock = Get-Content -LiteralPath (Join-Path $atelierRoot 'cmake/toolchain-lock.json') -Raw | ConvertFrom-Json
$atelierTools = Join-Path $atelierRoot '.tools'
$atelierLlvm = Join-Path $atelierTools "llvm-$($atelierLock.llvm)"
$atelierLlvmMajor = $atelierLock.llvm.Split('.')[0]
$atelierRequiredFiles = @('bin/clang.exe', 'bin/clang-cl.exe', 'bin/lld-link.exe',
    'bin/llvm-lib.exe', 'bin/llvm-rc.exe', "lib/clang/$atelierLlvmMajor/include/stddef.h",
    'include/llvm/ExecutionEngine/Orc/LLJIT.h', 'include/llvm-c/LLJIT.h',
    'lib/cmake/llvm/LLVMConfig.cmake', 'lib/LLVMCore.lib')
$atelierMissingFiles = @($atelierRequiredFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $atelierLlvm $_)) })

if ($atelierMissingFiles.Count -ne 0) {
    if (-not $InstallToolchain) {
        throw 'Pinned LLVM is missing or incomplete. Run: . ./tools/enter-dev.ps1 -InstallToolchain'
    }
    New-Item -ItemType Directory -Path $atelierLlvm -Force | Out-Null
    $atelierArchive = Join-Path $atelierTools "llvm-$($atelierLock.llvm).tar.zst"
    if (-not (Test-Path -LiteralPath $atelierArchive)) {
        & curl.exe --fail --location --retry 2 --retry-all-errors --connect-timeout 20 `
            --speed-time 30 --speed-limit 1024 --continue-at - --progress-bar `
            --header 'Accept: application/octet-stream' --output "$atelierArchive.partial" $atelierLock.llvmArchiveUrl
        if ($LASTEXITCODE -ne 0) { throw 'LLVM download failed.' }
        Move-Item -LiteralPath "$atelierArchive.partial" -Destination $atelierArchive
    }
    $atelierHashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
    $atelierHashStream = $null
    try {
        $atelierHashStream = [System.IO.File]::OpenRead($atelierArchive)
        $atelierArchiveHash = [BitConverter]::ToString($atelierHashAlgorithm.ComputeHash($atelierHashStream)).Replace('-', '')
    } finally {
        if ($null -ne $atelierHashStream) { $atelierHashStream.Dispose() }
        $atelierHashAlgorithm.Dispose()
    }
    if ($atelierArchiveHash -ne $atelierLock.llvmArchiveSha256) {
        throw "LLVM archive checksum mismatch: $atelierArchive"
    }
    # LLVM's exported CMake targets reference the complete SDK, not only clang.
    & tar.exe -xf $atelierArchive -C $atelierLlvm --strip-components=1
    if ($LASTEXITCODE -ne 0) { throw 'LLVM extraction failed.' }
    $atelierMissingFiles = @($atelierRequiredFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $atelierLlvm $_)) })
    if ($atelierMissingFiles.Count -ne 0) {
        throw "Pinned LLVM archive does not contain the required SDK files: $($atelierMissingFiles -join ', ')"
    }
    Remove-Item -LiteralPath $atelierArchive
}

$atelierVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $atelierVswhere)) { throw 'Visual Studio C++ Build Tools are required.' }
$atelierVsInstances = & $atelierVswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
$atelierVs = $atelierVsInstances | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.installationPath "VC/Tools/MSVC/$($atelierLock.msvc)")
} | Select-Object -First 1
if ($null -eq $atelierVs) { throw "MSVC toolset $($atelierLock.msvc) is required." }

Import-Module (Join-Path $atelierVs.installationPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $atelierVs.installationPath -SkipAutomaticLocation `
    -DevCmdArguments "-arch=x64 -host_arch=x64 -vcvars_ver=$($atelierLock.msvc) -winsdk=$($atelierLock.windowsSdk)" | Out-Null
if ($env:VCToolsVersion.TrimEnd('\') -ne $atelierLock.msvc -or $env:WindowsSDKVersion.TrimEnd('\') -ne $atelierLock.windowsSdk) {
    throw 'Visual Studio did not select the pinned MSVC toolset and Windows SDK.'
}
$atelierCmakeRoot = Join-Path $atelierVs.installationPath 'Common7/IDE/CommonExtensions/Microsoft/CMake'
$env:ATELIER_LLVM_ROOT = $atelierLlvm
$env:ATELIER_NINJA = Join-Path $atelierCmakeRoot 'Ninja/ninja.exe'
$env:PATH = "$(Join-Path $atelierLlvm 'bin');$(Join-Path $atelierCmakeRoot 'CMake/bin');$(Join-Path $atelierCmakeRoot 'Ninja');$env:PATH"

$atelierClangVersion = & (Join-Path $atelierLlvm 'bin/clang-cl.exe') --version
$atelierLinkVersion = & (Join-Path $atelierLlvm 'bin/lld-link.exe') --version
$atelierCmakeVersion = & cmake --version
$atelierNinjaVersion = & $env:ATELIER_NINJA --version
if ($atelierClangVersion[0] -notmatch "clang version $([regex]::Escape($atelierLock.llvm))(\s|$)" -or
    $atelierLinkVersion -notmatch "LLD $([regex]::Escape($atelierLock.llvm))(\s|$)" -or
    $atelierCmakeVersion[0] -ne "cmake version $($atelierLock.cmake)" -or
    $atelierNinjaVersion -ne $atelierLock.ninja) {
    throw 'Tool versions differ from cmake/toolchain-lock.json.'
}
Write-Host "Atelier: Clang/LLD $($atelierLock.llvm), CMake $($atelierLock.cmake), Ninja $($atelierLock.ninja), MSVC $($atelierLock.msvc), SDK $($atelierLock.windowsSdk)"
