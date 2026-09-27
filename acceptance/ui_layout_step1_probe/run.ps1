param(
    [ValidateSet('dev','release','both')][string]$Configuration='both',
    [string]$ReuseMimallocSource=''
)
$ErrorActionPreference='Stop'
$OutputEncoding=[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$source=$PSScriptRoot
$root=Split-Path -Parent (Split-Path -Parent $source)
$out=Join-Path $root 'out/review/ui-layout-step1'
New-Item -ItemType Directory -Force -Path $out | Out-Null
. (Join-Path $root 'tools/enter-dev.ps1')
$sourceHashes=foreach($name in @('kernel.hpp','kernel.cpp','main.cpp','CMakeLists.txt','run.ps1')){[ordered]@{file=$name;sha256=(Get-FileHash (Join-Path $source $name) -Algorithm SHA256).Hash.ToLowerInvariant()}}
[ordered]@{purpose='native step-one research; not production acceptance';source_hashes=$sourceHashes;toolchain=(Get-Content (Join-Path $root 'cmake/toolchain-lock.json') -Raw | ConvertFrom-Json);dependencies=(Get-Content (Join-Path $root 'cmake/dependencies-lock.json') -Raw | ConvertFrom-Json);os=[Environment]::OSVersion.VersionString;started_utc=[DateTime]::UtcNow.ToString('o');font_family='Segoe UI';locale='en-US'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'environment.json') -Encoding utf8
$runs=[Collections.Generic.List[object]]::new()
$configs=if($Configuration -eq 'both'){@('dev','release')}else{@($Configuration)}
foreach($config in $configs){
    $build=Join-Path $out $config
    $type=if($config -eq 'dev'){'Debug'}else{'Release'}
    $args=@('-S',$source,'-B',$build,'-G','Ninja',"-DCMAKE_BUILD_TYPE=$type",("-DCMAKE_TOOLCHAIN_FILE="+(Join-Path $root 'cmake/windows-clang.cmake')),"-DCMAKE_MAKE_PROGRAM=$env:ATELIER_NINJA",'-DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
    if($ReuseMimallocSource){$args+="-DFETCHCONTENT_SOURCE_DIR_MIMALLOC=$ReuseMimallocSource"}
    & cmake @args 2>&1 | Tee-Object -FilePath (Join-Path $out "$config-configure.log")
    if($LASTEXITCODE -ne 0){throw "Configure failed: $config"}
    & cmake --build $build --target layout_step1_probe --parallel 8 2>&1 | Tee-Object -FilePath (Join-Path $out "$config-build.log")
    if($LASTEXITCODE -ne 0){throw "Build failed: $config"}
    $exe=Join-Path $build 'layout_step1_probe.exe'
    $info=[Diagnostics.ProcessStartInfo]::new($exe)
    $info.WorkingDirectory=$build;$info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    if(-not $process.Start()){throw 'Could not start native experiment'}
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $timeout=-not $process.WaitForExit(120000)
    if($timeout){$process.Kill($true);$process.WaitForExit()}
    $entry=[ordered]@{configuration=$config;executable=$exe;executable_sha256=(Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant();exit_code=$process.ExitCode;timeout=$timeout;stdout=$stdout.Result;stderr=$stderr.Result}
    $entry | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out "$config-run.json") -Encoding utf8
    $runs.Add($entry);Write-Host "$config exit=$($process.ExitCode) timeout=$timeout";$process.Dispose()
}
$runs | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'runs.json') -Encoding utf8
if(@($runs | Where-Object {$_.exit_code -ne 0 -or $_.timeout}).Count){exit 2}
