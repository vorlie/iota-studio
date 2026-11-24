<#
.SYNOPSIS
  Builds Iota Studio from any plain PowerShell window (no Developer Command Prompt needed).

.DESCRIPTION
  Locates Visual Studio 2022 (Build Tools or full), loads the MSVC x64 environment
  (puts cl.exe and the bundled ninja.exe on PATH), configures the preset if needed,
  then builds. Optionally runs the tests and the headless smoke test.

.EXAMPLE
  .\build.ps1                       # Release build
  .\build.ps1 -Test                 # Release build + tests + smoke test
  .\build.ps1 -Preset release-mcp   # MCP edition
  .\build.ps1 -Run                  # Build, then launch Iota Studio
#>
param(
  [ValidateSet("release", "debug", "release-mcp", "debug-mcp")]
  [string]$Preset = "release",
  [string[]]$Target = @(),
  [switch]$Test,
  [switch]$Run
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Find-VcVars {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $root = & $vswhere -latest -products * `
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
      -property installationPath 2>$null
    if ($root) {
      $candidate = Join-Path $root "VC\Auxiliary\Build\vcvars64.bat"
      if (Test-Path $candidate) { return $candidate }
    }
  }
  foreach ($edition in "BuildTools", "Community", "Professional", "Enterprise") {
    foreach ($base in ${env:ProgramFiles(x86)}, $env:ProgramFiles) {
      $candidate = Join-Path $base "Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat"
      if (Test-Path $candidate) { return $candidate }
    }
  }
  return $null
}

# Load the MSVC environment only if the compiler is not already available.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vcvars = Find-VcVars
  if (-not $vcvars) {
    throw "Visual Studio 2022 with the 'Desktop development with C++' workload was not found. Install VS 2022 Build Tools and retry."
  }
  Write-Host "Loading MSVC environment from $vcvars"
  $envDump = cmd /c "`"$vcvars`" >nul && set"
  foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
      [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
  }
}

if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
  throw "ninja.exe not found even after loading the MSVC environment. Install the 'C++ CMake tools for Windows' component or run 'winget install Ninja-build.Ninja'."
}
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
  throw "cmake.exe not found. Install CMake 3.24+ and retry."
}

$configurePreset = switch ($Preset) {
  "release"     { "windows-msvc-ninja-release" }
  "debug"       { "windows-msvc-ninja-debug" }
  "release-mcp" { "windows-msvc-ninja-release-mcp" }
  "debug-mcp"   { "windows-msvc-ninja-debug-mcp" }
}

if (-not (Test-Path "build\$Preset\build.ninja")) {
  Write-Host "Configuring preset $configurePreset..."
  cmake --preset $configurePreset
  if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }
}

$buildArgs = @("--build", "--preset", $Preset, "--parallel")
if ($Target.Count -gt 0) { $buildArgs += @("--target") + $Target }
Write-Host "Building preset $Preset..."
cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }

if ($Test) {
  & ".\build\$Preset\tests\iota_tests.exe" "-tce=*APK presets*,*built-in presets*"
  if ($LASTEXITCODE -ne 0) { throw "Tests failed (exit $LASTEXITCODE)." }
  & ".\build\$Preset\apps\studio\iota_studio.exe" --smoke-test
  if ($LASTEXITCODE -ne 0) { throw "Smoke test failed (exit $LASTEXITCODE)." }
}

Write-Host "Build OK: build\$Preset\apps\studio\iota_studio.exe"

if ($Run) {
  & ".\build\$Preset\apps\studio\iota_studio.exe"
}
