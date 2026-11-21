param([string]$OutputDirectory = 'out/field-miss-study',
      [ValidateSet('fingerprint', 'full-hash')][string]$HashPatch = 'fingerprint')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
Push-Location $root
try {
    . ./tools/enter-dev.ps1
    cmake --build --preset release --target atelier_world_acceptance
    if ($LASTEXITCODE) { throw 'Release build failed.' }
    $out = (New-Item -Type Directory -Force $OutputDirectory).FullName
    $patched = (New-Item -Type Directory -Force "$out/patched").FullName
    $original = "$PSScriptRoot/../owned-index/dense_hash_table.hpp"
    Copy-Item -LiteralPath $original -Destination $patched
    git apply --unsafe-paths "--directory=$patched" -p5 "$PSScriptRoot/$HashPatch.patch"
    if ($LASTEXITCODE) { throw 'Cannot apply fingerprint counterfactual.' }
    foreach ($name in 'Before', 'After') {
        $path = if ($name -eq 'Before') { $original } else { "$patched/dense_hash_table.hpp" }
        # Expose the fingerprint for counting collisions outside timed loops.
        $header = (Get-Content $path -Raw).Replace('DenseHashTable', "FieldStudy$name")
        $header = $header.Replace("    static std::uint8_t name_tag(StringView", "public:`n    static std::uint8_t name_tag(StringView")
        Set-Content "$out/$name.hpp" $header -Encoding utf8NoBOM
    }
    $units = Get-Content out/release/compile_commands.json -Raw | ConvertFrom-Json
    function Compile-Study($unitPath, $source, $name, $extraFlags) {
        $unit = $units | Where-Object { $_.file.Replace('\', '/').EndsWith($unitPath) } | Select-Object -First 1
        if (!$unit -or $unit.command -notmatch '(/nologo.*?) /Fo') { throw "Missing flags for $unitPath." }
        Set-Content "$out/$name.rsp" $Matches[1] -Encoding utf8NoBOM
        clang-cl "@$out/$name.rsp" @extraFlags /c $source "/Fo$out/$name.obj"
        if ($LASTEXITCODE) { throw "Compile failed for $name." }
    }

    $runtime = 'modules/execution/runtime/src/runtime.cpp'
    $source = Get-Content $runtime -Raw
    function Replace-Once([string]$from, [string]$to) {
        if ([regex]::Matches($script:source, [regex]::Escape($from)).Count -ne 1) { throw "Source drift: $from" }
        $script:source = $script:source.Replace($from, $to)
    }
    Replace-Once 'Map<StringView, FieldId> field_by_name;' '::field_study::Index field_by_name;'
    Replace-Once 'return failure(ErrorCode::invalid_member, "Unknown field: " + std::string{name});' @'
if (::field_study::omit_message)
        return failure(ErrorCode::invalid_member, {});
    return failure(ErrorCode::invalid_member, "Unknown field: " + std::string{name});
'@
    Set-Content "$out/runtime.cpp" ("#include `"index.hpp`"`n" + $source) -Encoding utf8NoBOM
    Compile-Study $runtime "$out/runtime.cpp" 'runtime-study' @("/I$out", "/I$PSScriptRoot", "/I$(Split-Path $runtime)")
    Compile-Study 'acceptance/world/main.cpp' "$PSScriptRoot/profile.cpp" 'driver' @("/I$out")
    Set-Location out/release
    $lines = Get-Content build.ninja
    $build = ($lines | Select-String '^build atelier_world_acceptance\.exe:' | Select-Object -First 1).LineNumber
    $link = $lines[$build..($build + 12)] | Select-String '^  LINK_LIBRARIES =' | Select-Object -First 1
    if (!$link) { throw 'World consumer link rule missing.' }
    $libs = @(($link.Line -replace '^  LINK_LIBRARIES =\s*','') -split '\s+' | Where-Object { $_ })
    $mem = 'CMakeFiles/atelier_world_acceptance.dir/modules/core/memory/src'
    lld-link /nologo /subsystem:console /include:mi_version "/out:$out/field-study.exe" "$out/driver.obj" "$out/runtime-study.obj" `
        "$mem/fatal.cpp.obj" "$mem/global_new_delete.cpp.obj" @libs
    if ($LASTEXITCODE) { throw 'Link failed.' }
} finally {
    Pop-Location
}
