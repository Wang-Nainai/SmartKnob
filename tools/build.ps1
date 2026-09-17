# SmartKnob build helper (ESP-IDF v5.5+, Windows)
# Usage: powershell -ExecutionPolicy Bypass -File tools\build.ps1 build
#        Arguments are passed through to idf.py (build / flash / monitor / ...)
#
# ESP-IDF path resolution order:
#   1. -IdfPath / -ToolsPath script parameters
#   2. IDF_PATH / IDF_TOOLS_PATH environment variables
#      (already set inside the official "ESP-IDF PowerShell" prompt)
#   3. Common install locations probed automatically
# You can also skip this script: run idf.py directly in the ESP-IDF prompt.
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs,
    [string]$IdfPath,
    [string]$ToolsPath
)

$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

if (-not $IdfPath)   { $IdfPath   = $env:IDF_PATH }
if (-not $ToolsPath) { $ToolsPath = $env:IDF_TOOLS_PATH }

if (-not $IdfPath) {
    $candidates = @(
        "$env:USERPROFILE\esp\esp-idf",
        "C:\Espressif\frameworks\esp-idf-v5.5.5",
        "C:\Espressif\frameworks\esp-idf-v5.5",
        "C:\Espressif\esp-idf",
        "C:\mysoft\ESP32\v5.5.5\esp-idf"   # local machine fallback
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "export.ps1")) {
            $IdfPath = $c
            break
        }
    }
}

if (-not $IdfPath -or -not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    Write-Host "ESP-IDF not found. Install ESP-IDF v5.5+ first, then either:" -ForegroundColor Red
    Write-Host "  - pass the path:     -IdfPath C:\Espressif\frameworks\esp-idf-v5.5.5" -ForegroundColor Red
    Write-Host "  - or set env var:    IDF_PATH=<path to esp-idf>" -ForegroundColor Red
    exit 1
}
Write-Host "Using ESP-IDF at $IdfPath"

# Tools dir: script param > env > sibling "tools" of the IDF root (Installer
# layout, e.g. C:\Espressif\tools) > official default ~/.espressif
if (-not $ToolsPath) {
    $idfRoot = Split-Path -Parent $IdfPath
    $sibling = Join-Path (Split-Path -Parent $idfRoot) "tools"
    if (Test-Path $sibling) {
        $ToolsPath = $sibling
    } else {
        $ToolsPath = "$env:USERPROFILE\.espressif"
    }
}
$env:IDF_PATH       = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath

# Python virtual env: Installer versions differ (py3.11 / py3.13 ...), pick
# the one that actually exists instead of relying on export.ps1's default
if (-not $env:IDF_PYTHON_ENV_PATH) {
    $envDir = Join-Path $ToolsPath "python_env"
    $pyEnv = Get-ChildItem $envDir -Directory -Filter "idf5.5_*" -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1
    if ($pyEnv) {
        $env:IDF_PYTHON_ENV_PATH = $pyEnv.FullName
    }
}

# Project root lives one level above tools\
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $projectRoot

# Import the official ESP-IDF environment (PATH / python env / toolchain)
& (Join-Path $IdfPath "export.ps1") | Out-Null
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "export.ps1 finished but idf.py is not on PATH" -ForegroundColor Red
    exit 1
}

if ($IdfArgs.Count -eq 0) {
    $IdfArgs = @("build")   # default action
}
idf.py @IdfArgs
exit $LASTEXITCODE
