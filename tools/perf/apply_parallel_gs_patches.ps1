# Applies the local patches of third_party_patches/parallel-gs (in file name order) to a parallel-gs checkout at the pinned commit 3a66c19.
# Already applied patches are detected (the reverse patch applies) and skipped, so it can be run repeatedly. Nothing is pushed or published.
#   powershell -File tools/perf/apply_parallel_gs_patches.ps1 [-Dest C:\ps2x-gpu\parallel-gs] [-Check]
param(
    [string]$Dest = (Join-Path (Join-Path $PSScriptRoot '..\..') 'third_party\parallel-gs'),
    [switch]$Check  # only report what would happen
)
$patchDir = Join-Path (Join-Path $PSScriptRoot '..\..') 'third_party_patches\parallel-gs'
if (-not (Test-Path "$Dest\gs\gs_interface.hpp")) { throw "No parallel-gs checkout in $Dest" }

# git reports a failing --check on stderr: keep PowerShell from turning that into a terminating error
function Invoke-Git { param([string[]]$GitArgs) $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'; & git -C $Dest @GitArgs 2>&1 | Out-Null; $rc = $LASTEXITCODE; $ErrorActionPreference = $prev; return $rc }

foreach ($patch in (Get-ChildItem $patchDir -Filter '*.patch' | Sort-Object Name)) {
    if ((Invoke-Git @('apply', '--reverse', '--check', '--ignore-whitespace', $patch.FullName)) -eq 0) { Write-Host "already applied: $($patch.Name)"; continue }
    if ((Invoke-Git @('apply', '--check', '--ignore-whitespace', $patch.FullName)) -ne 0) { throw "$($patch.Name) does not apply to $Dest (is it the pinned commit 3a66c19 plus the earlier patches?)" }
    if ($Check) { Write-Host "would apply: $($patch.Name)"; continue }
    if ((Invoke-Git @('apply', '--ignore-whitespace', $patch.FullName)) -ne 0) { throw "failed to apply $($patch.Name)" }
    Write-Host "applied: $($patch.Name)"
}
