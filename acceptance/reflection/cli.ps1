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
Run-Generator 'empty-presentation' $valid.Replace('AFIELD()', 'AFIELD(Label="", Category="")')
Run-Generator 'presentation' $valid.Replace('AFIELD()', 'AFIELD(Name="value", Label="Position", Category="Transform")')
if (-not (Get-Content -Raw $generatedSource).Contains('"Position", "Transform"')) {
    throw 'Generated registration lost field presentation metadata.'
}
Run-Generator 'empty-description' $valid.Replace('AFIELD()', 'AFIELD(DESC="")')
$ranged = '#include "math/math.hpp"' + "`n" + $valid.Replace('AFIELD() ProbeScalar value{};', @'
AFIELD(Min="0", Max="10", LockMin, HardMax="65504") ProbeScalar value{};
    AFIELD(Min="-1.5", Max="2", LockMax) atelier::math::Vector3d direction;
    AFIELD(HardMin="-5", HardMax="007") int count{};
    AFIELD(HardMin="1") unsigned samples{1};
'@)
Run-Generator 'range' $ranged
$rangeOutput = Get-Content -Raw $generatedSource
foreach ($text in @(
    '.range({.ui_min = Scalar{0x0p+0f}, .ui_max = Scalar{0x1.4p+3f}, .hard_min = Scalar{0x0p+0f}, .hard_max = Scalar{0x1.ffcp+15f}})',
    '.range({.ui_min = Scalar{-0x1.8p+0}, .ui_max = Scalar{0x1p+1}, .hard_max = Scalar{0x1p+1}})',
    '.range({.hard_min = Scalar{std::int32_t{-5}}, .hard_max = Scalar{std::int32_t{7}}})',
    '.range({.hard_min = Scalar{std::uint32_t{1u}}})')) {
    if (-not $rangeOutput.Contains($text)) { throw "Generated registration lost an exact field range: $text" }
}
$described = $valid.Replace('Name="probe.Widget"', 'Name="probe.Widget", DESC="Widget 类型\r\n说明"').Replace(
    'AFIELD() ProbeScalar value{};',
    'AFIELD(DESC="Value 字段\n说明") ProbeScalar value{}; AFUNCTION(DESC="Read 方法\t说明") float read() const { return value; }')
Run-Generator 'description' $described
$descriptionOutput = Get-Content -Raw -Encoding utf8 $generatedSource
foreach ($text in @('"Widget 类型\r\n说明"', '"Value 字段\n说明"', '"Read 方法\t说明"')) {
    if (-not $descriptionOutput.Contains($text)) { throw 'Generated registration lost UTF-8 description text or escapes.' }
}
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
    @{name='description-spelling'; text=$valid.Replace('AFIELD()', 'AFIELD(Desc="help")'); error='Unknown or duplicate reflection option: Desc'},
    @{name='duplicate-description'; text=$valid.Replace('AFIELD()', 'AFIELD(DESC="A", DESC="B")'); error='Unknown or duplicate reflection option: DESC'},
    @{name='description-value'; text=$valid.Replace('AFIELD()', 'AFIELD(DESC)'); error='requires a value'},
    @{name='description-zero'; text=$valid.Replace('AFIELD()', 'AFIELD(DESC="bad\0text")'); error='DESC requires valid UTF-8'},
    @{name='description-control'; text=$valid.Replace('AFIELD()', 'AFIELD(DESC="bad\x01")'); error='DESC requires valid UTF-8'},
    @{name='description-del'; text=$valid.Replace('AFIELD()', 'AFIELD(DESC="bad\x7f")'); error='DESC requires valid UTF-8'},
    @{name='description-c1'; text=$valid.Replace('AFIELD()', 'AFIELD(DESC="bad\xc2\x85")'); error='DESC requires valid UTF-8'},
    @{name='description-utf8'; text=$valid.Replace('AFIELD()', 'AFIELD(DESC="bad\xff")'); error='DESC requires valid UTF-8'},
    @{name='category-spelling'; text=$valid.Replace('AFIELD()', 'AFIELD(Catagory="Transform")'); error='Unknown or duplicate reflection option: Catagory'},
    @{name='duplicate-label'; text=$valid.Replace('AFIELD()', 'AFIELD(Label="A", Label="B")'); error='Unknown or duplicate reflection option: Label'},
    @{name='duplicate-category'; text=$valid.Replace('AFIELD()', 'AFIELD(Category="A", Category="B")'); error='Unknown or duplicate reflection option: Category'},
    @{name='category-value'; text=$valid.Replace('AFIELD()', 'AFIELD(Category=42)'); error='requires an ordinary string literal'},
    @{name='label-control'; text=$valid.Replace('AFIELD()', 'AFIELD(Label="line\nfeed")'); error='no control characters'},
    @{name='empty-field-name'; text=$valid.Replace('AFIELD()', 'AFIELD(Name="")'); error='requires a nonempty string'},
    @{name='class-label'; text=$valid.Replace('Name="probe.Widget"', 'Label="Widget"'); error='Unknown or duplicate reflection option: Label'},
    @{name='view-field'; text=('#include "foundation/string.hpp"' + "`n" + $valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD() atelier::StringView view;')); error='Borrowed views cannot be persistent fields'},
    @{name='array-ref-field'; text=('#include "foundation/array.hpp"' + "`n" + $valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD() atelier::Array<atelier::ReferenceSlot> references;')); error='reference-free Array'},
    @{name='array-hidden-ref'; text=('#include "foundation/array.hpp"' + "`n" + 'struct Nested { atelier::ReferenceSlot slot; };' + $valid.Replace('AFIELD() ProbeScalar value{};', 'atelier::Array<Nested> references;')); error='requires an explicit AFIELD'},
    @{name='option'; text=$valid.Replace('Name="probe.Widget"','Nmae="probe.Widget"'); error='Unknown or duplicate reflection option'},
    @{name='unmarked'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'atelier::ReferenceSlot missing;'); error='requires an explicit AFIELD'},
    @{name='nested-unmarked'; text='struct Nested { atelier::ReferenceSlot slot; };' + $valid.Replace('AFIELD() ProbeScalar value{};', 'Nested missing;'); error='requires an explicit AFIELD'},
    @{name='strength'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD(Sequence="probe.Widget") atelier::ReferenceSlot slot;'); error='matching Strong, Weak or Sequence'},
    @{name='target'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD(Strong="probe.Missing") atelier::ReferenceSlot slot;'); error='Reference target must name'},
    @{name='signature'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFUNCTION() void bad(float&&) {}'); error='scalar/enum/plain record values or const T&/T& borrows'},
    @{name='enum-width'; text='AENUM() enum class Small : unsigned char { value };'; error='int32_t or uint32_t underlying type'},
    @{name='enum-unscoped'; text='AENUM() enum Plain { value };'; error='namespace-level enum class'},
    @{name='static-method'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFUNCTION() static float bad() { return 0; }'); error='non-static member with a scalar, void or plain record result'},
    @{name='body'; text=$valid.Replace('AGENERATED_BODY(Widget)', ''); error='requires exactly one AGENERATED_BODY'},
    @{name='grouped'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD() float first{}, second{};'); error='split grouped declarations'},
    @{name='inherited-name'; text='ACLASS() struct Base { AGENERATED_BODY(Base) AFIELD() float value{}; };' + $valid.Replace('struct Widget {', 'struct Widget : Base {'); error='Inherited reflected member name cannot be shadowed'},
    @{name='range-lock-alone'; text=$valid.Replace('AFIELD()', 'AFIELD(LockMin)'); error='LockMin requires Min and excludes HardMin'},
    @{name='range-lock-hard'; text=$valid.Replace('AFIELD()', 'AFIELD(Max="1", LockMax, HardMax="2")'); error='LockMax requires Max and excludes HardMax'},
    @{name='range-flag-value'; text=$valid.Replace('AFIELD()', 'AFIELD(Min="0", LockMin="1")'); error='LockMin is a flag'},
    @{name='range-bool'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD(Max="1") bool flag{};'); error='Field ranges require a float, double, int32, uint32 or math vector field'},
    @{name='range-quaternion'; text=('#include "math/math.hpp"' + "`n" + $valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD(Max="1") atelier::math::Quaternion rotation;')); error='Field ranges require a float, double, int32, uint32 or math vector field'},
    @{name='range-fraction'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD(HardMax="1.5") int count{};'); error='HardMax requires a finite decimal std::int32_t value'},
    @{name='range-negative-uint'; text=$valid.Replace('AFIELD() ProbeScalar value{};', 'AFIELD(HardMin="-1") unsigned count{};'); error='HardMin requires a finite decimal std::uint32_t value'},
    @{name='range-nan'; text=$valid.Replace('AFIELD()', 'AFIELD(Max="nan")'); error='Max requires a finite decimal float value'},
    @{name='range-overflow'; text=$valid.Replace('AFIELD()', 'AFIELD(Min="1e39")'); error='Min requires a finite decimal float value'}
)
foreach ($case in $cases) {
    Run-Generator $case.name $case.text $case.error
    if ((Get-FileHash $generatedHeader -Algorithm SHA256).Hash -ne $originalHeader.Hash -or
        (Get-FileHash $generatedSource -Algorithm SHA256).Hash -ne $originalSource.Hash) {
        throw "Rejected input replaced a prior output: $($case.name)"
    }
}
"Reflection CLI: PASS (UTF-8 descriptions, field presentation, stale output isolation, deterministic writes, include dependencies, $($cases.Count) located failures preserving output)"
