# Run from any directory: .\test\run_ecc_test.ps1
$ErrorActionPreference = 'Stop'
$compiler = Get-Command gcc -ErrorAction Stop
$outputFile = Join-Path ([IO.Path]::GetTempPath()) ('firmware-ecc-test-' + [Guid]::NewGuid().ToString('N') + '.exe')
$objectFiles = @()
try {
    $exports = @('protocolInit', 'protocolTransmit', 'protocolProcess',
                 'protocolTransmitTick', 'protocolReceiveTick', 'protocolIsReady',
                 'protocolIsTransmitting', 'protocolGetMessageData', 'protocolGetMessageLength',
                 'protocolGetCorrectedSymbolCount', 'protocolGetReceiverProgress', 'protocolResetReceiver')

    foreach ($role in @('Sender', 'Receiver')) {
        $objectFile = "$outputFile.$role.o"
        $objectFiles += $objectFile
        $roleFlag = '-D' + $role.ToUpperInvariant()
        $renameFlags = @($exports | ForEach-Object { "-D$_=$($role)_$_" })
        & $compiler.Source -std=c99 -O2 -Wall -Wextra -Werror "-I$PSScriptRoot\..\include" $roleFlag @renameFlags -c "$PSScriptRoot\..\src\protocol.c" -o $objectFile
        if ($LASTEXITCODE -ne 0) { throw "$role timing test compilation failed." }
    }

    & $compiler.Source -std=c99 -O2 -Wall -Wextra -Werror "-I$PSScriptRoot\..\include" "$PSScriptRoot\ecc_test.c" @objectFiles -o $outputFile
    if ($LASTEXITCODE -ne 0) { throw 'ECC test compilation failed.' }
    & $outputFile
    if ($LASTEXITCODE -ne 0) { throw 'ECC repair test failed. See the cases above.' }
} finally {
    if (Test-Path -LiteralPath $outputFile) { Remove-Item -LiteralPath $outputFile }
    foreach ($objectFile in $objectFiles) {
        if (Test-Path -LiteralPath $objectFile) { Remove-Item -LiteralPath $objectFile }
    }
}
