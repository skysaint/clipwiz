# dump_ico.ps1 - inspect the generated ICO container (debug helper)
$path = Join-Path $PSScriptRoot '..\src\clipwiz.ico'
$b = [IO.File]::ReadAllBytes($path)
Write-Host ("total={0} count={1}" -f $b.Length, [BitConverter]::ToUInt16($b, 4))
for ($i = 0; $i -lt [BitConverter]::ToUInt16($b, 4); $i++) {
    $o = 6 + 16 * $i
    Write-Host ("entry {0}: w={1} h={2} bytes={3} off={4}" -f $i, $b[$o], $b[$o + 1],
        [BitConverter]::ToUInt32($b, $o + 8), [BitConverter]::ToUInt32($b, $o + 12))
}
