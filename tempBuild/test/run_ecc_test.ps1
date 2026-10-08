# Run from any directory: .\test\run_ecc_test.ps1
$ErrorActionPreference = 'Stop'
$compiler = Get-Command gcc -ErrorAction Stop
$outputFile = Join-Path ([IO.Path]::GetTempPath()) ('firmware-ecc-test-' + [Guid]::NewGuid().ToString('N') + '.exe')
try {
    & $compiler.Source -std=c99 -O2 -Wall -Wextra "-I$PSScriptRoot\..\include" "$PSScriptRoot\ecc_test.c" -o $outputFile
    if ($LASTEXITCODE -ne 0) { throw 'ECC test compilation failed.' }
    & $outputFile
    if ($LASTEXITCODE -ne 0) { throw 'ECC repair test failed. See the cases above.' }
} finally {
    if (Test-Path -LiteralPath $outputFile) { Remove-Item -LiteralPath $outputFile }
}