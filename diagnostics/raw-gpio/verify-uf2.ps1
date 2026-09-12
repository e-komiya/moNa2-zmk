param([Parameter(Mandatory=$true)][string]$Path)
$ErrorActionPreference = 'Stop'
$data = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path)
if ($data.Length -eq 0 -or $data.Length % 512 -ne 0) { throw 'Invalid UF2 size' }
$count = $data.Length / 512
$minAddress = [uint32]::MaxValue
$maxAddress = 0
$seen = [Collections.Generic.HashSet[uint32]]::new()
$foundVectors = $false
for ($offset = 0; $offset -lt $data.Length; $offset += 512) {
    $magic0 = [BitConverter]::ToUInt32($data, $offset)
    $magic1 = [BitConverter]::ToUInt32($data, $offset + 4)
    $flags = [BitConverter]::ToUInt32($data, $offset + 8)
    $address = [BitConverter]::ToUInt32($data, $offset + 12)
    $size = [BitConverter]::ToUInt32($data, $offset + 16)
    $index = [BitConverter]::ToUInt32($data, $offset + 20)
    $blocks = [BitConverter]::ToUInt32($data, $offset + 24)
    $family = [BitConverter]::ToUInt32($data, $offset + 28)
    $end = [BitConverter]::ToUInt32($data, $offset + 508)
    if ($magic0 -ne 0x0A324655 -or $magic1 -ne 0x9E5D5157L -or $end -ne 0x0AB16F30) { throw 'UF2 magic mismatch' }
    if ($flags -ne 0x2000 -or $family -ne 0xADA52840L) { throw 'Not plain nRF52840-family UF2' }
    if ($blocks -ne $count -or $index -ge $count -or !$seen.Add($index)) { throw 'Block numbering mismatch' }
    if ($size -eq 0 -or $size -gt 476 -or $address -lt 0x27000 -or ($address + $size) -gt 0xEC000) { throw 'Payload outside application partition' }
    $minAddress = [Math]::Min($minAddress, $address)
    $maxAddress = [Math]::Max($maxAddress, $address + $size)
    if ($address -eq 0x27000) {
        $sp = [BitConverter]::ToUInt32($data, $offset + 32)
        $reset = [BitConverter]::ToUInt32($data, $offset + 36)
        if ($sp -le 0x20000000 -or $sp -gt 0x20040000 -or $reset -lt 0x27001 -or $reset -ge 0xEC000 -or !($reset -band 1)) { throw 'Unexpected vector table' }
        $foundVectors = $true
    }
}
if (!$foundVectors) { throw 'Application vector table absent' }
if (![Text.Encoding]::ASCII.GetString($data).Contains('MONA2-RAW-GPIO-v1')) { throw 'Diagnostic identifier absent' }
[pscustomobject]@{
    Result = 'PASS'
    Blocks = $count
    Start = ('0x{0:X}' -f $minAddress)
    EndExclusive = ('0x{0:X}' -f $maxAddress)
    Family = '0xADA52840'
    SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}
