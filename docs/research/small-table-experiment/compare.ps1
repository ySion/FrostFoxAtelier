param(
    [ValidateRange(1, 1000)][int]$Rounds = 18,
    [string]$OutputDirectory = 'out/small-hash-profile',
    [string]$Reference = 'baseline-v2',
    [string]$FirstCandidate = 'scan2',
    [string]$SecondCandidate = 'scan4',
    [ValidateRange(-1, 63)][int]$LogicalProcessor = -1,
    [string]$Mesh
)

$ErrorActionPreference = 'Stop'
$directory = (Resolve-Path $OutputDirectory).Path
$env:PATH = "$(Resolve-Path out/release);$env:PATH"
$candidates = @($FirstCandidate, $SecondCandidate | Where-Object { $_ })
$orders = if ($SecondCandidate) {
    "$Reference $FirstCandidate $SecondCandidate", "$FirstCandidate $SecondCandidate $Reference",
    "$SecondCandidate $Reference $FirstCandidate", "$SecondCandidate $FirstCandidate $Reference",
    "$Reference $SecondCandidate $FirstCandidate", "$FirstCandidate $Reference $SecondCandidate"
} else {
    "$Reference $FirstCandidate", "$FirstCandidate $Reference"
}
$inputs = @(if ($Mesh) { (Resolve-Path $Mesh).Path })

function Get-Median($values) {
    $sorted = @($values | Sort-Object)
    $left = [int][math]::Floor(($sorted.Count - 1) / 2)
    $right = [int][math]::Floor($sorted.Count / 2)
    return ($sorted[$left] + $sorted[$right]) / 2
}

function Read-Samples {
    foreach ($round in 0..($Rounds - 1)) {
        foreach ($variant in $orders[$round % $orders.Count].Split(' ')) {
            $output = & "$directory/$variant.exe" @inputs
            if ($LASTEXITCODE) { throw "$variant failed in round $round ($LASTEXITCODE)." }
            if ($Mesh -and !($output -match '^mesh_digest,')) { throw "$variant skipped the mesh workload." }
            foreach ($line in $output) {
                $metric, $value = $line.Split(',')
                [pscustomobject]@{
                    Round = $round
                    Backend = $variant
                    Metric = $metric
                    Value = [double]::Parse($value, [cultureinfo]::InvariantCulture)
                }
            }
        }
    }
}
$process = Get-Process -Id $PID
$original = $process.ProcessorAffinity
try {
    if ($LogicalProcessor -ge 0) {
        $mask = [long]1 -shl $LogicalProcessor
        if (!($original.ToInt64() -band $mask)) { throw "Logical processor $LogicalProcessor is unavailable." }
        $process.ProcessorAffinity = [IntPtr]$mask
    }
    $rows = Read-Samples
} finally {
    if ($LogicalProcessor -ge 0) { $process.ProcessorAffinity = $original }
}
$rows | Export-Csv "$directory/final-results.csv" -NoTypeInformation

$summary = foreach ($group in ($rows | Group-Object Metric)) {
    $baseline = @($group.Group | Where-Object Backend -eq $Reference | Sort-Object Round)
    foreach ($variant in $candidates) {
        $candidate = @($group.Group | Where-Object Backend -eq $variant | Sort-Object Round)
        if ($group.Name -eq 'mesh_digest' -and (@($group.Group.Value | Sort-Object -Unique).Count -ne 1)) {
            throw 'Mesh outputs differ across implementations.'
        }
        $changes = @(for ($i = 0; $i -lt $Rounds; ++$i) {
            100 * ($candidate[$i].Value / $baseline[$i].Value - 1)
        })
        [pscustomobject]@{
            Metric = $group.Name
            Backend = $variant
            Baseline = Get-Median $baseline.Value
            Candidate = Get-Median $candidate.Value
            PairedPercent = [math]::Round((Get-Median $changes), 2)
            Wins = @($changes | Where-Object { $_ -lt 0 }).Count
        }
    }
}
$summary | Export-Csv "$directory/final-summary.csv" -NoTypeInformation
$summary | Format-Table -AutoSize
