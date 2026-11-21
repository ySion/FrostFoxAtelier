param([Parameter(Mandatory)][string]$Label, [string]$OutputDirectory = 'out/foundation-hash')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
Push-Location $root
try {
    . ./tools/enter-dev.ps1
    cmake --build --preset release --target atelier_renderer_acceptance --parallel 12
    if ($LASTEXITCODE) { throw 'Renderer consumer build failed.' }
    $out = (New-Item -ItemType Directory -Force $OutputDirectory).FullName
    New-Item -ItemType Directory -Force "$out/ankerl" | Out-Null
    $refs = @{
        'ankerl/unordered_dense.h' = 'https://raw.githubusercontent.com/martinus/unordered_dense/v5.3.0/include/ankerl/unordered_dense.h'
        'ankerl/stl.h' = 'https://raw.githubusercontent.com/martinus/unordered_dense/v5.3.0/include/ankerl/stl.h'
        'wyhash.h' = 'https://raw.githubusercontent.com/wangyi-fudan/wyhash/ea3b25e1aef55d90f707c3a292eeb9162e2615d8/wyhash.h'
        'xxhash.h' = 'https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.h'
    }
    foreach ($name in $refs.Keys) {
        if (!(Test-Path "$out/$name")) { Invoke-WebRequest -Uri $refs[$name] -OutFile "$out/$name" }
    }
    $unit = Get-Content out/release/compile_commands.json -Raw | ConvertFrom-Json |
        Where-Object { $_.file.Replace('\','/').EndsWith('acceptance/renderer/main.cpp') } | Select-Object -First 1
    if (!$unit -or $unit.command -notmatch '(/nologo.*?) /Fo') { throw 'Missing renderer compiler flags.' }
    Set-Content "$out/$Label.rsp" $Matches[1] -Encoding utf8NoBOM
    $legacy = @(if ($Label -eq 'before') { '/DATELIER_HASH_REFERENCE_BUILD' })
    clang-cl "@$out/$Label.rsp" @legacy "/I$root/modules/render/renderer/src" "-imsvc$out" /c "$PSScriptRoot/profile.cpp" "/Fo$out/$Label.obj"
    if ($LASTEXITCODE) { throw 'Hash study compilation failed.' }
    Set-Location out/release
    $lines = Get-Content build.ninja
    $n = ($lines | Select-String '^build atelier_renderer_acceptance\.exe:' | Select-Object -First 1).LineNumber
    $link = $lines[$n..($n+12)] | Select-String '^  LINK_LIBRARIES =' | Select-Object -First 1
    if (!$link) { throw 'Missing renderer link rule.' }
    Set-Content "$out/$Label-link.rsp" ($link.Line -replace '^  LINK_LIBRARIES =\s*','') -Encoding utf8NoBOM
    $mem = 'CMakeFiles/atelier_renderer_acceptance.dir/modules/core/memory/src'
    lld-link /nologo /subsystem:console /include:mi_version "/out:$out/$Label.exe" "$out/$Label.obj" `
        "$mem/fatal.cpp.obj" "$mem/global_new_delete.cpp.obj" "@$out/$Label-link.rsp"
    if ($LASTEXITCODE) { throw 'Hash study link failed.' }
} finally { Pop-Location }
