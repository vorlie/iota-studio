param(
  [string]$Configuration = "release"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CMakeLists = Join-Path $RepoRoot "CMakeLists.txt"
$IssPath = Join-Path $PSScriptRoot "iota_studio.iss"

if (-not (Test-Path $CMakeLists)) {
  throw "CMakeLists.txt not found at $CMakeLists"
}

$cmakeContent = Get-Content -Path $CMakeLists -Raw
if ($cmakeContent -notmatch 'project\s*\(\s*IotaStudio\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
  throw "Failed to parse IotaStudio VERSION from $CMakeLists"
}
$AppVersion = $Matches[1]

$IsccExe = $null
$cmdIscc = Get-Command iscc -ErrorAction SilentlyContinue
if ($cmdIscc) {
  $IsccExe = $cmdIscc.Source
} else {
  $candidates = @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe",
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe")
  )
  foreach ($cand in $candidates) {
    if ($cand -and (Test-Path $cand)) {
      $IsccExe = $cand
      break
    }
  }
}

if (-not $IsccExe) {
  throw "Inno Setup 6 compiler (ISCC.exe) not found. Install Inno Setup 6 and retry."
}

$BuildRelDir = "..\build\$Configuration"
$StudioExe = Join-Path $RepoRoot "build\$Configuration\apps\studio\iota_studio.exe"
if (-not (Test-Path $StudioExe)) {
  throw "Built executable not found at $StudioExe. Run cmake --build build/$Configuration first."
}

Write-Host "Building Iota Studio v$AppVersion installer using $IsccExe..."
& $IsccExe "/DAppVersion=$AppVersion" "/DBuildDir=$BuildRelDir" $IssPath
if ($LASTEXITCODE -ne 0) {
  throw "ISCC.exe exited with code $LASTEXITCODE"
}

$OutputInstaller = Join-Path $RepoRoot "build\installer\IotaStudio-Setup-$AppVersion.exe"
if (-not (Test-Path $OutputInstaller)) {
  throw "Expected installer output not found at $OutputInstaller"
}

$item = Get-Item $OutputInstaller
$sha256 = (Get-FileHash $OutputInstaller -Algorithm SHA256).Hash
Write-Host "Installer built successfully:"
Write-Host "  Path:   $($item.FullName)"
Write-Host "  Size:   $($item.Length) bytes"
Write-Host "  SHA256: $sha256"
