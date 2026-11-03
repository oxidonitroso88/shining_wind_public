# Runs the game once per resolution with PS2X_PERF on and collects ONE csv (PS2X_PERF_LABEL = x1 / x2 / x4 ...) for tools/perf/perf_summary.py.
#   powershell -ExecutionPolicy Bypass -File tools\perf\perf_matrix.ps1 -Exe <ShiningWind.exe | ps2EntryRunner.exe> [-ExeArgs '<guest elf, if your launcher needs it>']
#        [-Seconds 90] [-Level 1] [-Configs x1,x2,x4] [-Scenario opening|story|field] [-ShotVBlanks 1500,2400] [-McRoot <mc0 folder>] [-Shots] [-Sample] [-PadScript '<PS2X_PAD_SCRIPT>'] [-Out <folder>] [-WorkingDirectory <folder>]
# LAUNCH: leave -ExeArgs EMPTY to start the game you already play (the remembered disc/folder of ShiningWind.exe), or use  -ExeArgs '--disc "<iso or folder with ALL disc files>"'.
# Do NOT pass port-work\game\SLPM_666.71: that folder only has the boot files, the game cannot read its data and does not run (game speed ~1%).
# -Scenario opening = nothing is pressed (the opening plays). -Scenario story = the same scripted input as tools\perf\measure_run.ps1 (skips the opening, New Game, then circle every 4 s):
#   it reaches the first dialogues/exploration after ~60-100 s, so use -Seconds 240 or more. -Shots saves a small screenshot every 25 s (PS2X_DUMP_FRAME) to see WHICH scene each window measured.
# -SampleDeep = like -Sample plus a stack sample per function (self + inclusive) written to perf.csv.hot.csv; read it with tools\perf\hot_report.py and the .map.
# -Sample = also samples the game thread every 2 ms (PS2X_PERF_SAMPLE=1): where its time goes by module and who it waits for (see perf_summary.py --timeline).
# Level 1 = CPU timers only (the honest CPU/wait picture). Level 2 = also GPU timestamps (real GPU time per renderer stage; run it as a SECOND pass: it can perturb a little).
# Same scene every time: the game starts, nothing is pressed (the opening plays) unless -PadScript scripts the controller. Do not touch the window while it runs; the process is closed
# after -Seconds. Run on an idle machine, plugged in (laptop), with the same power plan for every configuration.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$ExeArgs = '',
    [int]$Seconds = 90,
    [ValidateSet(1, 2)][int]$Level = 1,
    [string[]]$Configs = @('x1', 'x2', 'x4'),
    [string]$PadScript = '',
    [ValidateSet('opening', 'story', 'field')][string]$Scenario = 'opening',
    [string]$ShotVBlanks = '',
    [string]$McRoot = '',
    [switch]$Shots,
    [switch]$Sample,
    [switch]$SampleDeep,
    [switch]$MemSample,   # 0018: every 5 s record the process' private / working-set memory to mem.csv (separate pass: it is a small perturbation, do not use it for speed runs)
    [string]$Out = '',
    [string]$WorkingDirectory = ''
)
$ErrorActionPreference = 'Stop'
$Configs = @($Configs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # powershell -File passes 'x1,x2' as ONE string
$defs = @{
    'x1'  = @{ PS2X_GS_SCALE = '1'; PS2X_GS_OUTPUT = '' }
    'x2'  = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2' }
    'x4'  = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1' }
    # existing experimental option (no code change): shrink the frame on the GPU to the window size before the read-back
    'x2d' = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_GPU_DOWNSCALE = '1' }
    'x4d' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_GPU_DOWNSCALE = '1' }
    # experiment A/B (off by default in the game): wait for the presentation's own fence with the backend mutex released. Only x2/x4 use that path (x1 does not).
    'x2u' = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_PRESENT_UNLOCK = '1' }
    'x4u' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_PRESENT_UNLOCK = '1' }
    # experiment A/B (the game default keeps the [SIF:...] stderr lines): the same configuration with those lines silenced (PS2X_SIF_TRACE=0). Suffix q = quiet.
    'x1q'  = @{ PS2X_GS_SCALE = '1'; PS2X_GS_OUTPUT = ''; PS2X_SIF_TRACE = '0' }
    'x2q'  = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_SIF_TRACE = '0' }
    'x2uq' = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_SIF_TRACE = '0' }
    # experiment A/B: IPU OUT FIFO byte queue (f = contiguous buffer, v = verification mode, slower, diagnostic only)
    'x1f'  = @{ PS2X_GS_SCALE = '1'; PS2X_GS_OUTPUT = ''; PS2X_IPU_FASTBUF = '1' }
    'x1v'  = @{ PS2X_GS_SCALE = '1'; PS2X_GS_OUTPUT = ''; PS2X_IPU_FASTBUF = '2' }
    'x2uf' = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_IPU_FASTBUF = '1' }
    # diagnostic A/B: discard all std::cerr/std::cout output (n = null stdio) to measure what the stream logging costs
    'x1n'  = @{ PS2X_GS_SCALE = '1'; PS2X_GS_OUTPUT = ''; PS2X_STDIO_NULL = '1' }
    # combination FASTBUF + all stdio discarded: measurable with the 0012 binary (no rebuild)
    'x1fn' = @{ PS2X_GS_SCALE = '1'; PS2X_GS_OUTPUT = ''; PS2X_IPU_FASTBUF = '1'; PS2X_STDIO_NULL = '1' }
    # 0013: the selective logging fix is the default; v = PS2X_LOG_VERBOSE=1 restores every repetitive line (the pre-0013 behaviour). x1f (FASTBUF) in the 0013 binary already has the selective logging; x1flv = FASTBUF + verbose logging.
    'x1lv'  = @{ PS2X_GS_SCALE = '1'; PS2X_GS_OUTPUT = ''; PS2X_LOG_VERBOSE = '1' }
    # 0014: FASTBUF at the other resolutions (f = PS2X_IPU_FASTBUF=1, u = PS2X_GS_PRESENT_UNLOCK=1). x1f / x2uf exist above.
    'x2f'  = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_IPU_FASTBUF = '1' }
    'x4f'  = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4uf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_IPU_FASTBUF = '1' }
    # 0015: GPU downscale of the PRESENTATION COPY only (PS2X_GS_GPU_DOWNSCALE=1, internal rendering stays x2/x4). The target is the rectangle the picture occupies in the window
    # (aspect 640x448 fitted in the window; the default window at x2/x4 is 1280x896). It only shrinks when that is smaller than the frame in BOTH axes, so x2 (1280x447) never shrinks
    # in the default window: x2udf is expected to behave like x2uf. x4udf1080 forces the target to 1543x1080 (the picture of a maximised window on a 1920x1080 screen) as a cost measurement.
    'x2udf'     = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4udf'     = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4udf1080' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_GS_DOWNSCALE_TARGET = '1543x1080'; PS2X_IPU_FASTBUF = '1' }
    # 0016 factorial (x2/x4 x PROMOTE/SSTEX): x4udf = X16 + PS2X_GS_PROMOTE (which also turns super-sampled textures on); x2pudf = x2udf + PROMOTE; x4sudf = x4 + SSTEX only (no promotion);
    # x4npudf = x4 with neither (DIAGNOSTIC ONLY: without back-buffer promotion the x4 scanout is not the same picture path).
    'x2pudf'  = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_PROMOTE = '1'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4sudf'  = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_SSTEX = '1'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4npudf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    # 0017: x4udf with the parallel-gs per-flush image-memory limit overridden (PS2X_GS_IMG_PRESSURE_MB; the Vega default is 12 MiB). Same picture path, only the flush heuristic changes.
    # 0018: finer sweep around 32 MiB
    # 0022: HD_SSAA (UVs of sprites with HD replacements interpolated per super-sample, what play-x4.cmd sets) on top of the tested profiles
    'x2h'     = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_HD_SSAA = '1' }
    # 0023 (Vulkan validation tests): the same x2 HD-light profile without / with PRESENT_UNLOCK, and without FASTBUF, to isolate the unlock
    'x2pdh'   = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_PROMOTE = '1'; PS2X_GS_HD_SSAA = '1'; PS2X_GS_GPU_DOWNSCALE = '1' }
    'x2pudh'  = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_PROMOTE = '1'; PS2X_GS_HD_SSAA = '1'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1' }
    'x2pudfh' = @{ PS2X_GS_SCALE = '2'; PS2X_GS_OUTPUT = '2'; PS2X_GS_PROMOTE = '1'; PS2X_GS_HD_SSAA = '1'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4udfh'  = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_HD_SSAA = '1'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i16udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '16'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i24udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '24'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i40udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '40'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i48udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '48'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i32udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '32'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i64udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '64'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i128udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '128'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i256udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '256'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x4i512udf' = @{ PS2X_GS_SCALE = '4'; PS2X_GS_OUTPUT = '4'; PS2X_GS_PROMOTE = '1'; PS2X_GS_IMG_PRESSURE_MB = '512'; PS2X_GS_PRESENT_UNLOCK = '1'; PS2X_GS_GPU_DOWNSCALE = '1'; PS2X_IPU_FASTBUF = '1' }
    'x1flv' = @{ PS2X_GS_SCALE = '1'; PS2X_GS_OUTPUT = ''; PS2X_IPU_FASTBUF = '1'; PS2X_LOG_VERBOSE = '1' }
}
# 0014 -Scenario field: loads FILE01 of the memory card through the menus (same delays as tools\perf\load_slot1.ps1), then walks the left stick in a square, opens/closes the
# field menu with Start and keeps walking. Needs a save in FILE01 of the card the game uses (PS2X_MC_ROOT or the default one). The loading takes ~125 s: use -Seconds 240 or more.
if ($Scenario -eq 'field' -and -not $PadScript) {
    $PadScript = 'circle@38-38.2,start@70-70.2,start@82-82.2,start@91-91.2,circle@95.5-95.7,circle@100.7-100.9,circle@104.9-105.1,circle@112.1-112.3'
    for ($t = 128; $t -lt 1100; $t += 24) {
        $PadScript += (',lleft@{0}-{1},lup@{2}-{3},lright@{4}-{5},ldown@{6}-{7}' -f $t, ($t + 4), ($t + 5), ($t + 9), ($t + 10), ($t + 14), ($t + 15), ($t + 19))
        $PadScript += (',start@{0}-{1},start@{2}-{3}' -f ($t + 20), ($t + 20.2), ($t + 22), ($t + 22.2))
    }
}
if ($Scenario -eq 'story' -and -not $PadScript) {
    $PadScript = 'start@12-13,start@25-26,start@38-39,circle@45-46'
    for ($t = 62; $t -lt 1200; $t += 4) { $PadScript += (',circle@{0}-{1}' -f $t, ($t + 0.6)) }
}
if ($ExeArgs -match '(?i)SLPM_[0-9]+\.[0-9]+' -and $ExeArgs -notmatch '--disc') {
    $elf = ($ExeArgs -replace '"', '').Trim()
    if (Test-Path -LiteralPath $elf) {
        $dir = Split-Path -Parent $elf
        if (-not (Test-Path -LiteralPath (Join-Path $dir 'MODULES'))) { throw "The folder of the ELF ($dir) has no disc files (no MODULES folder): the game would not run and every number would be meaningless. Leave -ExeArgs empty (remembered game) or use -ExeArgs '--disc ""<iso>""'." }
    }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $Out) { $Out = Join-Path (Get-Location) "perf-$stamp" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$csv = Join-Path $Out 'perf.csv'
$managed = @('PS2X_GS_SCALE', 'PS2X_GS_OUTPUT', 'PS2X_GS_PROMOTE', 'PS2X_GS_SSTEX', 'PS2X_GS_IMG_PRESSURE_MB', 'PS2X_GS_GPU_DOWNSCALE', 'PS2X_GS_PRESENT_UNLOCK', 'PS2X_GS_DOWNSCALE_TARGET', 'PS2X_SIF_TRACE', 'PS2X_IPU_FASTBUF', 'PS2X_STDIO_NULL', 'PS2X_LOG_VERBOSE', 'PS2X_SHOT', 'PS2X_SHOT_VBLANKS', 'PS2X_MC_ROOT', 'PS2X_PERF_SAMPLE', 'PS2X_GS_HD_SSAA', 'PS2X_PERF', 'PS2X_PERF_CSV', 'PS2X_PERF_LABEL', 'PS2X_PAD_SCRIPT', 'PS2X_DUMP_FRAME', 'PS2X_DUMP_FRAME_EVERY')
if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Exe)) -ErrorAction SilentlyContinue) { throw 'The game is already running: close it first.' }
$seen = @{}; $labels = @()   # a config repeated in -Configs (x1,x1) gets its own label (x1, x1#2), log files and screenshots: nothing is merged or overwritten
foreach ($name in $Configs) {
    if (-not $defs.ContainsKey($name)) { throw "Unknown config $name (use: $($defs.Keys -join ', '))" }
    foreach ($k in $managed) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }   # no leftovers from a previous configuration
    foreach ($kv in $defs[$name].GetEnumerator()) { if ($kv.Value) { Set-Item "Env:$($kv.Key)" $kv.Value } }
    $seen[$name] = 1 + [int]$seen[$name]
    $label = if ($seen[$name] -gt 1) { "$name#$($seen[$name])" } else { $name }
    $labels += $label
    $env:PS2X_PERF = "$Level"; $env:PS2X_PERF_CSV = $csv; $env:PS2X_PERF_LABEL = $label
    if ($PadScript) { $env:PS2X_PAD_SCRIPT = $PadScript }
    if ($SampleDeep) { $env:PS2X_PERF_SAMPLE = '2' }   # + stack samples per function -> perf.csv.hot.csv (tools\perf\hot_report.py)
    elseif ($Sample) { $env:PS2X_PERF_SAMPLE = '1' }   # game-thread sampler: module breakdown + who-waits per window (column ee_samples)
    if ($ShotVBlanks) { $env:PS2X_SHOT = (Join-Path $Out ('{0}-vb' -f ($label -replace '#', '_'))); $env:PS2X_SHOT_VBLANKS = $ShotVBlanks }   # PNGs at fixed guest VBlanks: comparable between runs
    if ($McRoot) { $env:PS2X_MC_ROOT = $McRoot }
    if ($Shots) { $env:PS2X_DUMP_FRAME = (Join-Path $Out ("{0}-shot" -f ($label -replace "#", "_"))); $env:PS2X_DUMP_FRAME_EVERY = '1500' }
    Write-Host ("== {0}: {1} s, PS2X_PERF={2}" -f $label, $Seconds, $Level)
    $sp = @{ FilePath = $Exe; PassThru = $true; RedirectStandardError = (Join-Path $Out (($label -replace "#", "_") + ".stderr.log")); RedirectStandardOutput = (Join-Path $Out (($label -replace "#", "_") + ".stdout.log")) }
    if ($ExeArgs) { $sp.ArgumentList = $(if ($ExeArgs -match '\s' -and $ExeArgs -notmatch '"') { '"' + $ExeArgs + '"' } else { $ExeArgs }) }   # paths with spaces
    if ($WorkingDirectory) { $sp.WorkingDirectory = $WorkingDirectory }
    $p = Start-Process @sp
    $end = (Get-Date).AddSeconds($Seconds)
    $nextMem = (Get-Date).AddSeconds(5); $t0 = Get-Date
    while ((Get-Date) -lt $end -and -not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        if ($MemSample -and (Get-Date) -ge $nextMem) {
            $nextMem = (Get-Date).AddSeconds(5)
            try {
                $p.Refresh()
                $mf = Join-Path $Out 'mem.csv'
                if (-not (Test-Path $mf)) { 'label,t_s,private_mb,workingset_mb,peak_workingset_mb' | Out-File -Encoding ascii $mf }
                ('{0},{1},{2},{3},{4}' -f $label, [int]((Get-Date) - $t0).TotalSeconds, [int]($p.PrivateMemorySize64 / 1MB), [int]($p.WorkingSet64 / 1MB), [int]($p.PeakWorkingSet64 / 1MB)) | Out-File -Encoding ascii -Append $mf
            } catch { }
        }
    }
    if (-not $p.HasExited) { [void]$p.CloseMainWindow(); Start-Sleep -Seconds 3 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 3   # let the GPU/driver settle between configurations
}
foreach ($k in $managed) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
Write-Host "`nCSV: $csv"
$py = Get-Command python -ErrorAction SilentlyContinue
if ($py -and (Test-Path (Join-Path $PSScriptRoot 'perf_summary.py'))) { $txt = & python (Join-Path $PSScriptRoot 'perf_summary.py') $csv --labels ($labels -join ',') | Out-String; $txt | Out-File -Encoding utf8 (Join-Path $Out 'summary.txt'); Write-Host $txt }
else { Write-Host "python not found: run   python tools\perf\perf_summary.py $csv   on any PC" }
