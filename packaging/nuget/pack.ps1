# Builds ravel as a static library for x64 (Release and Debug, dynamic MSVC runtime),
# stages headers and libraries, and packs Ravel.Dst.<version>.nupkg into the output directory.
#   pwsh packaging/nuget/pack.ps1 -Version 0.3.0 [-Output out]
# Needs Visual Studio's C++ tools, CMake and nuget.exe on PATH (or NUGET_EXE).
param(
  [Parameter(Mandatory)][string]$Version,
  [string]$Output = "nuget-out"
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$stage = Join-Path $here 'stage'
$work = Join-Path $root 'build-nuget'
Remove-Item -Recurse -Force $stage, $work -ErrorAction SilentlyContinue

foreach ($config in 'Release', 'Debug') {
  $dir = Join-Path $work $config
  cmake -S $root -B $dir -A x64 -DRAVEL_BUILD_TESTS=OFF -DRAVEL_BUILD_BENCH=OFF `
    -DRAVEL_BUILD_EXAMPLES=OFF -DRAVEL_BUILD_FUZZ=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$(if ($config -eq 'Debug') {'Debug'})DLL"
  if ($LASTEXITCODE) { throw "configure ($config) failed" }
  cmake --build $dir --config $config --target ravel
  if ($LASTEXITCODE) { throw "build ($config) failed" }
  $lib = Join-Path $dir "$config\ravel.lib"
  $target = Join-Path $stage "lib\x64\$config"
  New-Item -ItemType Directory -Force $target | Out-Null
  Copy-Item $lib $target
}
New-Item -ItemType Directory -Force (Join-Path $stage 'include') | Out-Null
Copy-Item -Recurse (Join-Path $root 'include\ravel') (Join-Path $stage 'include')

$nuget = if ($env:NUGET_EXE) { $env:NUGET_EXE } else { 'nuget' }
New-Item -ItemType Directory -Force $Output | Out-Null
& $nuget pack (Join-Path $here 'Ravel.Dst.nuspec') -Version $Version -OutputDirectory $Output -NoDefaultExcludes
if ($LASTEXITCODE) { throw 'nuget pack failed' }
