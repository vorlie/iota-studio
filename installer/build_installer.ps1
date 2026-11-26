param(
  [ValidateSet("Standard", "Mcp", "All")]
  [string]$Variant = "Standard",
  [string]$Configuration = "release",
  [string]$McpConfiguration = "release-mcp"
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

function Invoke-InstallerBuild {
  param(
    [bool]$IncludeMcp,
    [string]$BuildConfig
  )

  $buildRelDir = "..\build\$BuildConfig"
  $studioExe = Join-Path $RepoRoot "build\$BuildConfig\apps\studio\iota_studio.exe"
  if (-not (Test-Path $studioExe)) {
    throw "Built executable not found at $studioExe. Run cmake --build build/$BuildConfig first."
  }

  $mcpExe = Join-Path $RepoRoot "build\$BuildConfig\apps\mcp\iota_mcp.exe"
  if ($IncludeMcp -and (-not (Test-Path $mcpExe))) {
    throw "Built MCP executable not found at $mcpExe. Run cmake --build build/$BuildConfig with -DIOTA_BUILD_MCP=ON first."
  }

  $includeMcpFlag = if ($IncludeMcp) { 1 } else { 0 }
  $editionLabel = if ($IncludeMcp) { "MCP edition" } else { "standard edition" }
  $outputName = if ($IncludeMcp) {
    "IotaStudio-MCP-Setup-$AppVersion.exe"
  } else {
    "IotaStudio-Setup-$AppVersion.exe"
  }

  Write-Host "Building Iota Studio v$AppVersion installer ($editionLabel) from build/$BuildConfig using $IsccExe..."
  & $IsccExe "/DAppVersion=$AppVersion" "/DBuildDir=$buildRelDir" "/DIncludeMcp=$includeMcpFlag" $IssPath
  if ($LASTEXITCODE -ne 0) {
    throw "ISCC.exe exited with code $LASTEXITCODE"
  }

  $outputInstaller = Join-Path $RepoRoot "build\installer\$outputName"
  if (-not (Test-Path $outputInstaller)) {
    throw "Expected installer output not found at $outputInstaller"
  }

  $item = Get-Item $outputInstaller
  $sha256 = (Get-FileHash $outputInstaller -Algorithm SHA256).Hash
  Write-Host "Installer built successfully:"
  Write-Host "  Path:   $($item.FullName)"
  Write-Host "  Size:   $($item.Length) bytes"
  Write-Host "  SHA256: $sha256"
}

if ($Variant -eq "Standard" -or $Variant -eq "All") {
  Invoke-InstallerBuild -IncludeMcp $false -BuildConfig $Configuration
}

if ($Variant -eq "Mcp" -or $Variant -eq "All") {
  Invoke-InstallerBuild -IncludeMcp $true -BuildConfig $McpConfiguration
}
