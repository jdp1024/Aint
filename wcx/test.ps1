param(
    [ValidateSet('x64', 'x86')]
    [string[]]$Architecture = @('x64', 'x86'),
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$fixtures = @(
    (Join-Path $repo 'samples\VOL10.AIN'),
    (Join-Path $repo 'samples\VOL20.AIN')
) | Where-Object { Test-Path -LiteralPath $_ }
foreach ($arch in $Architecture) {
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot 'build.bat') $arch
        if ($LASTEXITCODE -ne 0) { throw "WCX $arch build failed" }
    }
    & (Join-Path $PSScriptRoot "build\$arch\wcx_test.exe") @fixtures
    if ($LASTEXITCODE -ne 0) { throw "WCX $arch integration tests failed" }
}
