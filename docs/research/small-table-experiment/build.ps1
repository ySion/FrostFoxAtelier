param(
    [Parameter(Mandatory)]
    [ValidateSet('baseline-v2', 'scan2', 'scan4', 'plain4', 'tag4', 'owned', 'dictionary-lookup')]
    [string]$Label,
    [string]$OutputDirectory = 'out/small-hash-profile',
    [switch]$Allocations
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
Push-Location $root
try {
    $candidateHeader = 'modules/core/foundation/include/foundation/detail/small_hash_table.hpp'
    $usesCandidate = Select-String -Quiet -SimpleMatch 'using Storage = detail::SmallHashTable<' 'modules/core/foundation/include/foundation/map.hpp'
    $ownsIndex = Select-String -Quiet -SimpleMatch 'using Storage = detail::DenseHashTable<' 'modules/core/foundation/include/foundation/map.hpp'
    if ($Label -eq 'baseline-v2') {
        if ($usesCandidate -or $ownsIndex) { throw 'The baseline must use the original Map and Set.' }
    } elseif ($Label -eq 'owned' -or ($ownsIndex -and $Label -eq 'dictionary-lookup')) {
        if (!$ownsIndex) { throw 'The selected source must own its dense hash index.' }
    } else {
        $limit = if ($Label -eq 'scan2') { 2 } else { 4 }
        if (!$usesCandidate -or !(Select-String -Quiet -SimpleMatch "scan_limit = $limit;" $candidateHeader)) {
            throw "The selected source must implement a small-table limit of $limit for $Label."
        }
        if ($Label -in 'plain4', 'tag4', 'dictionary-lookup') {
            $packed = Select-String -Quiet -SimpleMatch 'm_max_bucket_capacity == 0' $candidateHeader
            $tagged = Select-String -Quiet -SimpleMatch 'Indexed::fingerprint_word' $candidateHeader
            $expectsFingerprints = $Label -in 'tag4', 'dictionary-lookup'
            if (!$packed -or ($tagged -ne $expectsFingerprints)) {
                throw "The source layout does not match $Label."
            }
        }
    }

    . ./tools/enter-dev.ps1
    cmake --build --preset release --target atelier_world_acceptance
    if ($LASTEXITCODE) { throw 'Release consumer build failed.' }

    $output = (New-Item -ItemType Directory -Force $OutputDirectory).FullName
    $unit = Get-Content out/release/compile_commands.json -Raw | ConvertFrom-Json |
        Where-Object { $_.file -match '[\\/]acceptance[\\/]world[\\/]main\.cpp$' } | Select-Object -First 1
    if (!$unit -or $unit.command -notmatch '(/nologo.*?) /Fo') { throw 'Missing world consumer compiler flags.' }
    $response = Join-Path $output "$Label.rsp"
    Set-Content -LiteralPath $response -Value $Matches[1] -Encoding utf8NoBOM
    $artifact = if ($Allocations) { "$Label-allocations" } else { $Label }
    $source = if ($Allocations) {
        'allocation-profile.cpp'
    } elseif ($Label -eq 'dictionary-lookup') { 'dictionary11/lookup.cpp' } else { 'profile.cpp' }
    clang-cl "@$response" /c (Join-Path $PSScriptRoot $source) "/Fo$output/$artifact.obj"
    if ($LASTEXITCODE) { throw 'Profile compilation failed.' }

    Set-Location out/release
    $lines = Get-Content build.ninja
    $build = ($lines | Select-String '^build atelier_world_acceptance\.exe:' | Select-Object -First 1).LineNumber
    if (!$build) { throw 'Missing world consumer link rule.' }
    $line = $lines[$build..($build + 12)] | Select-String '^  LINK_LIBRARIES =' | Select-Object -First 1
    if (!$line) { throw 'Missing world consumer link libraries.' }
    $libraries = @(($line.Line -replace '^  LINK_LIBRARIES =\s*', '') -split '\s+' | Where-Object { $_ })
    $memory = 'CMakeFiles/atelier_world_acceptance.dir/modules/core/memory/src'
    $overrides = @(if (!$Allocations) { "$memory/global_new_delete.cpp.obj" })
    lld-link /nologo /subsystem:console /include:mi_version "/out:$output/$artifact.exe" "$output/$artifact.obj" `
        "$memory/fatal.cpp.obj" @overrides @libraries
    if ($LASTEXITCODE) { throw 'Profile link failed.' }
} finally {
    Pop-Location
}
