$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot

# clangd needs the same MSVC/SDK environment as the real build.
# Keep all setup output off stdout: it carries the MCP protocol.
. (Join-Path $projectRoot 'tools/enter-dev.ps1') 1>$null 6>$null

$compileCommands = Join-Path $projectRoot 'out/dev/compile_commands.json'
if (-not (Test-Path -LiteralPath $compileCommands)) {
    throw 'Missing compilation database. Run cmake --preset dev in the project development environment first.'
}
Copy-Item -LiteralPath $compileCommands -Destination (Join-Path $projectRoot 'compile_commands.json') -Force

& serena start-mcp-server --project $projectRoot --context ide --transport stdio --open-web-dashboard false
exit $LASTEXITCODE
