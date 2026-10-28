$ErrorActionPreference = 'Stop'
$files = @(
    (Join-Path $PSScriptRoot 'build\x86\ain.wcx'),
    (Join-Path $PSScriptRoot 'build\x64\ain.wcx64'),
    (Join-Path $PSScriptRoot 'pluginst.inf'),
    (Join-Path $PSScriptRoot 'README.md')
)
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Missing $file. Run wcx\test.ps1 to build and test both architectures first."
    }
}
$archive = Join-Path $PSScriptRoot 'build\ain-wcx.zip'
Compress-Archive -LiteralPath $files -DestinationPath $archive -Force -CompressionLevel Optimal
Write-Output "Created $archive"
