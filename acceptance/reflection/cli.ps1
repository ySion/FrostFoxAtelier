param(
    [Parameter(Mandatory)][string]$Generator,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path.Replace('\','/')
$commands = Get-Content -Raw (Join-Path $BuildDirectory 'compile_commands.json') | ConvertFrom-Json
$base = @($commands | Where-Object { $_.file.Replace('\','/') -eq $Source.Replace('\','/') })
if ($base.Count -ne 1) { throw 'Expected one real reflection consumer compile command.' }
$sourcePath = "$OutputDirectory/probe.cpp"
$headerPath = "$OutputDirectory/probe.hpp"
$generatedHeader = "$OutputDirectory/probe.gen.hpp"
$generatedSource = "$OutputDirectory/probe.gen.cpp"
$command = $base[0]
$command.command = $command.command.Replace($command.file.Replace('/','\'), $sourcePath).Replace($command.file, $sourcePath)
$command.file = $sourcePath
ConvertTo-Json -InputObject @($command) -Depth 4 | Set-Content "$OutputDirectory/compile_commands.json" -Encoding utf8
'#include "probe.hpp"' | Set-Content $sourcePath -Encoding utf8
'using ProbeScalar = float;' | Set-Content "$OutputDirectory/dependency.hpp" -Encoding utf8
$productionConfig = Get-Content -Raw (Join-Path $BuildDirectory 'generated/atelier_reflection_acceptance/reflection.json') | ConvertFrom-Json
$config = @{
    source=$sourcePath; database=$OutputDirectory; cpp=$generatedSource
    depfile="$OutputDirectory/probe.d"; resource_dir=$productionConfig.resource_dir
    headers=@(@{path=$headerPath; include='probe.hpp'; output=$generatedHeader})
}
$configPath = "$OutputDirectory/reflection.json"
$config | ConvertTo-Json -Depth 5 | Set-Content $configPath -Encoding utf8
$prefix = @'
#include "runtime/reflection.hpp"
#include "probe.gen.hpp"
#include "dependency.hpp"
#if 0
ACLASS(UnknownOption)
struct Disabled {};
#endif
'@
$valid = @'
ACLASS(Name="probe.Widget")
struct Widget {
    AGENERATED_BODY(Widget)
    AFIELD() ProbeScalar value{};
};
'@
function Run-Generator([string]$name, [string]$declarations, [string]$expectedError = '') {
    ($prefix + "`n" + $declarations) | Set-Content $headerPath -Encoding utf8
    & $Generator $configPath *> "$OutputDirectory/$name.log"
    $result = $LASTEXITCODE
    $output = Get-Content -Raw "$OutputDirectory/$name.log"
    if ($expectedError) {
        if ($result -eq 0 -or -not $output.Contains($expectedError) -or $output -notmatch 'probe\.hpp(?:\(\d+,\d+\)|:\d+:\d+): error:') {
            throw "Expected located failure for ${name}, got exit ${result}: $output"
        }
    } elseif ($result -ne 0) { throw "Generator failed for ${name}: $output" }
}
# A scan must not depend on stale generated output, including during bootstrap.
'#error stale generated header was read' | Set-Content $generatedHeader -Encoding utf8
Run-Generator 'valid' $valid
$originalHeader = Get-FileHash $generatedHeader -Algorithm SHA256
$originalSource = Get-FileHash $generatedSource -Algorithm SHA256
$headerTime = (Get-Item $generatedHeader).LastWriteTimeUtc.Ticks
$sourceTime = (Get-Item $generatedSource).LastWriteTimeUtc.Ticks
& $Generator $configPath *> "$OutputDirectory/repeated.log"
if ($LASTEXITCODE -ne 0 -or (Get-FileHash $generatedSource -Algorithm SHA256).Hash -ne $originalSource.Hash -or
    (Get-Item $generatedHeader).LastWriteTimeUtc.Ticks -ne $headerTime -or (Get-Item $generatedSource).LastWriteTimeUtc.Ticks -ne $sourceTime) {
    throw 'Repeated generation was not deterministic or rewrote unchanged output.'
}
if ((Get-Content -Raw "$OutputDirectory/probe.d") -notmatch 'dependency\.hpp') { throw 'Included header is missing from the depfile.' }
$cases = @(
    @{name='option'; text=$valid.Replace('Name="probe.Widget"','Nmae="probe.Widget"'); error='Unknown or duplicate reflection option'},
    @{name='unmarked'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'atelier::ReferenceSlot missing;'); error='requires an explicit AFIELD'},
    @{name='nested-unmarked'; text='struct Nested { atelier::ReferenceSlot slot; };' + $valid.Replace('AFIELD() ProbeScalar value{};', 'Nested missing;'); error='requires an explicit AFIELD'},
    @{name='strength'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD(Sequence="probe.Widget") atelier::ReferenceSlot slot;'); error='matching Strong, Weak or Sequence'},
    @{name='target'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD(Strong="probe.Missing") atelier::ReferenceSlot slot;'); error='Reference target must name'},
    @{name='signature'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFUNCTION() void bad(float&) {}'); error='parameters must be float or uint32_t'},
    @{name='static-method'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFUNCTION() static float bad() { return 0; }'); error='non-static scalar member signature'},
    @{name='body'; text=$valid.Replace('AGENERATED_BODY(Widget)', ''); error='requires exactly one AGENERATED_BODY'},
    @{name='grouped'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD() float first{}, second{};'); error='split grouped declarations'},
    @{name='inherited-name'; text='ACLASS() struct Base { AGENERATED_BODY(Base) AFIELD() float value{}; };' + $valid.Replace('struct Widget {', 'struct Widget : Base {'); error='Inherited reflected member name cannot be shadowed'}
)
foreach ($case in $cases) {
    Run-Generator $case.name $case.text $case.error
    if ((Get-FileHash $generatedHeader -Algorithm SHA256).Hash -ne $originalHeader.Hash -or
        (Get-FileHash $generatedSource -Algorithm SHA256).Hash -ne $originalSource.Hash) {
        throw "Rejected input replaced a prior output: $($case.name)"
    }
}
"Reflection CLI: PASS (stale output isolation, deterministic writes, include dependencies, $($cases.Count) located failures preserving output)"
