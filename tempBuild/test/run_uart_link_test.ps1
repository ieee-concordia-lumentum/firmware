# Host-side tests of the actual UART framing and desktop ECC implementations.
$ErrorActionPreference = 'Stop'
$compiler = Get-Command gcc -ErrorAction Stop
$testOutput = Join-Path ([IO.Path]::GetTempPath()) ('uart-link-test-' + [Guid]::NewGuid().ToString('N') + '.exe')
try {
    # Build the real link code and ECC code into a temporary test program.
    & $compiler.Source -std=c99 -O2 -Wall -Wextra -Werror -DDESKTOP "-I$PSScriptRoot\..\include" "$PSScriptRoot\uart_link_test.c" "$PSScriptRoot\..\src\uart_link.c" "$PSScriptRoot\..\src\protocol.c" -o $testOutput
    if ($LASTEXITCODE -ne 0) { throw 'UART test compilation failed.' }
    & $testOutput
    if ($LASTEXITCODE -ne 0) { throw 'UART tests failed.' }
} finally {
    if (Test-Path -LiteralPath $testOutput) { Remove-Item -LiteralPath $testOutput }
}
