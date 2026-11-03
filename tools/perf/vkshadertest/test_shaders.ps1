# Runs vkshadertest ONCE PER SHADER (one process each, so a driver crash only affects that file) over a folder of .spv files written by
# PS2X_GS_VKLOG_DUMP_SPIRV=1 (default: %APPDATA%\ShiningWind\spirv), and prints a table. With -StripDecoration it repeats the failing ones with that
# OpDecorate removed (5356 = AliasedPointer).
#   powershell -ExecutionPolicy Bypass -File test_shaders.ps1 -Exe <build>\Release\vkshadertest.exe [-Folder <dir>] [-Device 0] [-StripDecoration 5356]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Folder = "$env:APPDATA\ShiningWind\spirv",
    [int]$Device = 0,
    [int[]]$StripDecoration = @()
)
$ErrorActionPreference = 'Stop'
& $Exe --list
$rows = @()
foreach ($f in Get-ChildItem $Folder -Filter *.spv | Sort-Object Name) {
    $out = & $Exe --device $Device $f.FullName 2>&1 | Select-Object -Last 1
    $status = if ($out -match 'OK') { 'OK' } elseif ($out -match 'CRASH') { ($out -replace '^.*(CRASH.*)$', '$1') } else { "$out" }
    $row = [pscustomobject]@{ File = $f.Name; Bytes = $f.Length; Result = $status; WithStrip = '' }
    if ($StripDecoration.Count -gt 0 -and $status -ne 'OK') {
        $args2 = @('--device', $Device)
        foreach ($d in $StripDecoration) { $args2 += @('--strip-decoration', $d) }
        $out2 = & $Exe @args2 $f.FullName 2>&1 | Select-Object -Last 1
        $row.WithStrip = if ($out2 -match 'OK') { 'OK' } elseif ($out2 -match 'CRASH') { ($out2 -replace '^.*(CRASH.*)$', '$1') } else { "$out2" }
    }
    $rows += $row
}
$rows | Format-Table -AutoSize | Out-String -Width 200
