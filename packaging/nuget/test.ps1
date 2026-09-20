# Unpacks a Ravel.Dst .nupkg and builds and runs test_package/consumer.vcxproj against it,
# in Release and Debug, through the package's own .targets file.
#   pwsh packaging/nuget/test.ps1 -Package nuget-out/Ravel.Dst.0.3.0.nupkg
param([Parameter(Mandatory)][string]$Package)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$unpacked = Join-Path $here 'unpacked'
Remove-Item -Recurse -Force $unpacked -ErrorAction SilentlyContinue
Copy-Item $Package "$Package.zip"
Expand-Archive "$Package.zip" $unpacked
Remove-Item "$Package.zip"

$targets = Join-Path $unpacked 'build\native\Ravel.Dst.targets'
if (-not (Test-Path $targets)) { throw "no .targets file in the package" }
foreach ($file in 'build\native\include\ravel\runner.hpp', 'build\native\lib\x64\Release\ravel.lib',
                  'build\native\lib\x64\Debug\ravel.lib', 'LICENSE.txt', 'docs\README.md') {
  if (-not (Test-Path (Join-Path $unpacked $file))) { throw "package is missing $file" }
}

$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest `
  -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
$project = Join-Path $here 'test_package\consumer.vcxproj'
foreach ($config in 'Release', 'Debug') {
  & $msbuild $project /p:Configuration=$config /p:Platform=x64 "/p:RavelTargets=$targets" /v:m /nologo
  if ($LASTEXITCODE) { throw "build ($config) failed" }
  & (Join-Path $here "test_package\x64\$config\consumer.exe")
  if ($LASTEXITCODE) { throw "consumer ($config) failed" }
}
Write-Host 'package OK'
