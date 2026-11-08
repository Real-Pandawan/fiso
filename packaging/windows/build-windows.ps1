# Builds FISO Explorer for Windows x64 and produces the installer (.exe).
#   powershell -ExecutionPolicy Bypass -File packaging\windows\build-windows.ps1
# Requirements: Visual Studio 2022 (or Build Tools) with "Desktop development with C++",
# CMake >= 3.20, Node.js >= 20, git.
# SPDX-License-Identifier: Apache-2.0
$ErrorActionPreference = 'Stop'

$Root  = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$Build = Join-Path $Root 'build-windows'
$App   = Join-Path $Root 'fiso-explorer'

function Step($msg) { Write-Host "`n==> $msg" }
# npm and npx are called as npm.cmd / npx.cmd: their PowerShell wrappers (npm.ps1) re-read the
# original command line and lose the arguments when called through this function.
function Run($exe) {
  & $exe @args
  if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

foreach ($tool in 'cmake', 'node', 'npm') {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is not installed or not in PATH" }
}
if ([int](node -p "process.versions.node.split('.')[0]") -lt 20) { throw 'Node.js 20 or later is required' }

Step 'Engine and command-line tool'
Run cmake -S $Root -B $Build -A x64
Run cmake --build $Build --config Release --parallel

Step 'Tests'
Push-Location $Build
try { Run ctest -C Release --output-on-failure } finally { Pop-Location }

Step 'Application'
$Bin = Join-Path $Build 'fiso-cli\Release\fiso.exe'
if (-not (Test-Path $Bin)) { throw "fiso.exe not found at $Bin" }
New-Item -ItemType Directory -Force -Path (Join-Path $App 'bin') | Out-Null
Copy-Item $Bin (Join-Path $App 'bin\fiso.exe') -Force

Push-Location $App
try {
  Run npm.cmd ci --no-audit --no-fund
  if (Test-Path dist) { Remove-Item -Recurse -Force dist }
  $env:CSC_IDENTITY_AUTO_DISCOVERY = 'false'
  Run npx.cmd electron-builder --win nsis --x64 --publish never
  $Setup = Get-ChildItem dist -Filter '*.exe' | Select-Object -First 1
  if (-not $Setup) { throw 'no installer produced' }
  Write-Host $Setup.FullName
} finally { Pop-Location }
