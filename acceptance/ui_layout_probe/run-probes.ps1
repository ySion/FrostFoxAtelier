param(
    [ValidateSet('dev','release','both')][string]$Configuration = 'both',
    [string]$Candidates = 'yoga,taffy,clay',
    [string]$SharedToolsRoot = '',
    [string]$ReuseMimallocSource = '',
    [switch]$RunBenchmarks,
    [int]$Samples = 100
)
$ErrorActionPreference = 'Stop'
$OutputEncoding = [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$source = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $source)
$out = Join-Path $root 'out/review/ui-layout-validation'
$vendor = Join-Path $out 'vendor'
New-Item -ItemType Directory -Force -Path $vendor | Out-Null
. (Join-Path $source 'enter-probe.ps1') -SharedToolsRoot $SharedToolsRoot
$env:CARGO_HOME = Join-Path $out 'cargo-home'
$env:CARGO_TARGET_DIR = Join-Path $out 'cargo'
$names = @($Candidates.Split(',') | ForEach-Object { $_.Trim() })
foreach ($name in $names) { if ($name -notin @('yoga','taffy','clay')) { throw "Unknown candidate $name" } }
$pins = Get-Content (Join-Path $source 'sources.json') -Raw | ConvertFrom-Json
foreach ($name in $names) {
    $pin = $pins.$name
    $dir = Join-Path $vendor $name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $download = if ($pin.kind -eq 'header') { Join-Path $dir 'clay.h' } else { Join-Path $out "$name.tar.gz" }
    if (-not (Test-Path $download)) {
        & curl.exe --fail --location --retry 2 --retry-all-errors --connect-timeout 20 --header 'Accept: application/vnd.github.raw+json' --output "$download.partial" $pin.url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $name" }
        Move-Item -LiteralPath "$download.partial" -Destination $download
    }
    if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.sha256) { throw "Source checksum mismatch: $name" }
    if ($pin.kind -eq 'tar.gz' -and -not (Test-Path (Join-Path $dir 'Cargo.toml')) -and -not (Test-Path (Join-Path $dir 'CMakeLists.txt'))) {
        & tar.exe -xf $download -C $dir --strip-components=1
        if ($LASTEXITCODE -ne 0) { throw "Extract failed: $name" }
    }
}
if ('taffy' -in $names) {
    $rustVersion = (& rustc --version)
    if ($LASTEXITCODE -ne 0 -or $rustVersion -notmatch ('^rustc ' + [regex]::Escape($pins.rust.release) + ' ')) { throw "Rust compiler does not match the experimental pin: $rustVersion" }
    if (-not (Test-Path (Join-Path $source 'taffy/Cargo.lock'))) {
        & cargo generate-lockfile --manifest-path (Join-Path $source 'taffy/Cargo.toml')
        if ($LASTEXITCODE -ne 0) { throw 'Cargo lock generation failed' }
    }
}
$sourceFiles = @('main.cpp','probe_api.h','yoga_adapter.cpp','clay_adapter.cpp','CMakeLists.txt','enter-probe.ps1','run-probes.ps1','sources.json','taffy/Cargo.toml','taffy/Cargo.lock','taffy/src/lib.rs')
$hashes = foreach ($file in $sourceFiles) { $path=Join-Path $source $file; [ordered]@{ path=$file; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() } }
$fontFiles = foreach ($file in @('segoeui.ttf','msyh.ttc','msyhbd.ttc')) { $path=Join-Path "$env:WINDIR/Fonts" $file; if(Test-Path $path){[ordered]@{file=$file;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}} }
$toolchainPin = Get-Content (Join-Path $root 'cmake/toolchain-lock.json') -Raw | ConvertFrom-Json
$environment = [ordered]@{
    started_utc=[DateTime]::UtcNow.ToString('o'); configurations=$Configuration; candidates=$names; samples=$Samples; warmup=20
    os=[Environment]::OSVersion.VersionString; cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
    cpp=@(& (Join-Path $root ".tools/llvm-$($toolchainPin.llvm)/bin/clang-cl.exe") --version)
    rust=if('taffy' -in $names){@(& rustc -vV)}else{@()}; windows_sdk=$env:WindowsSDKVersion; vc_tools=$env:VCToolsVersion
    sources=$hashes; pins=$pins; font_family='Segoe UI'; locale='en-US'; available_font_files=$fontFiles
    font_note='File hashes record available base/fallback fonts, not proof of the actual fallback glyph face.'
    dwrite_sha256=(Get-FileHash -LiteralPath "$env:WINDIR/System32/dwrite.dll" -Algorithm SHA256).Hash.ToLowerInvariant()
}
$environment | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'environment.json') -Encoding utf8
$script:runs = [Collections.Generic.List[object]]::new()
function Invoke-Probe([string]$exe, [string[]]$Arguments, [string]$Label) {
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.WorkingDirectory = Split-Path -Parent $exe
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info
    $started = [DateTime]::UtcNow.ToString('o')
    if (-not $process.Start()) { throw "Cannot start $exe" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit(180000)
    if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
    $entry = [ordered]@{ label=$Label; executable=$exe; executable_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant(); arguments=$Arguments; started_utc=$started; exit_code=$process.ExitCode; timed_out=$timedOut; stdout=$stdoutTask.Result; stderr=$stderrTask.Result }
    $entry | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out "$Label.json") -Encoding utf8
    $script:runs.Add($entry)
    Write-Host "$Label exit=$($process.ExitCode) timeout=$timedOut"
    $process.Dispose()
}
$configs = if ($Configuration -eq 'both') { @('dev','release') } else { @($Configuration) }
foreach ($config in $configs) {
    $build = Join-Path $out $config
    $buildType = if ($config -eq 'dev') { 'Debug' } else { 'Release' }
    $args = @('-S',$source,'-B',$build,'-G','Ninja',"-DCMAKE_BUILD_TYPE=$buildType",("-DCMAKE_TOOLCHAIN_FILE=" + (Join-Path $root 'cmake/windows-clang.cmake')),"-DCMAKE_MAKE_PROGRAM=$env:ATELIER_NINJA",("-DLP_CANDIDATES=" + ($names -join ';')))
    if ($ReuseMimallocSource) { $args += "-DFETCHCONTENT_SOURCE_DIR_MIMALLOC=$ReuseMimallocSource" }
    & cmake @args
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $config" }
    & cmake --build $build --target layout_probes --parallel 8
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $config" }
    foreach ($name in $names) {
        $exe = Join-Path $build "layout_probe_$name.exe"
        Invoke-Probe $exe @() "$name-$config-behavior"
        Invoke-Probe $exe @('--diagnostics') "$name-$config-saturation"
        foreach ($depth in @(64,256,1024)) { Invoke-Probe $exe @('--depth',"$depth") "$name-$config-depth$depth" }
    }
}
# Performance runs occur only after every requested build, never alongside compilation.
if ($RunBenchmarks) {
    if ('release' -notin $configs) { throw 'Benchmarks require a release build in this invocation' }
    foreach ($name in $names) {
        $exe = Join-Path (Join-Path $out 'release') "layout_probe_$name.exe"
        Invoke-Probe $exe @('--bench','1000',"$Samples") "$name-release-bench-boxes1000"
        Invoke-Probe $exe @('--bench','128',"$Samples",'text') "$name-release-bench-text128"
    }
}
$script:runs | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'runs.json') -Encoding utf8
$failed = @($script:runs | Where-Object { $_.exit_code -ne 0 -or $_.timed_out })
Write-Host "Recorded $($script:runs.Count) runs; $($failed.Count) nonzero/timeout results. Nonzero candidate results remain evidence, not suppressed successes."
if ($failed.Count) { exit 2 }
