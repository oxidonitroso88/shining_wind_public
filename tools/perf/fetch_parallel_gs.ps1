# Fetches parallel-gs (Arntzen Software, LGPL-3.0+) at the pinned commit plus only the Granite submodules the standalone build links
# (~146 MB) into third_party/parallel-gs, where ps2xRuntime/CMakeLists.txt looks for it, and applies the local patches of
# third_party_patches/parallel-gs (see its README.md: HD texture replacement hook, LGPL obligations). Requires git and network access.
param([string]$Dest = (Join-Path (Join-Path $PSScriptRoot '..\..') 'third_party\parallel-gs'))
$ErrorActionPreference = 'Stop'
$commit = '3a66c1976170cbc2cb53a3593fabbc7c4b2ccfbd'
if (Test-Path "$Dest\gs\gs_interface.hpp") { Write-Host "Already present: $Dest (apply the patches with tools/perf/apply_parallel_gs_patches.ps1 if needed)"; return }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
git -C $Dest init -q
git -C $Dest remote add origin https://github.com/Arntzen-Software/parallel-gs.git
git -C $Dest fetch --depth 1 origin $commit
git -C $Dest checkout -q FETCH_HEAD
git -C $Dest submodule update --init --depth 1 Granite
foreach ($m in 'volk', 'khronos/vulkan-headers', 'glslang', 'spirv-headers', 'spirv-tools', 'rapidjson', 'stb/stb') {
    git -C "$Dest\Granite" submodule update --init --depth 1 "third_party/$m"
}
& (Join-Path $PSScriptRoot 'apply_parallel_gs_patches.ps1') -Dest $Dest
Write-Host "parallel-gs ready in $Dest"
