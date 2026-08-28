# SmartKnob 一键构建脚本 (ESP-IDF v5.5.5, Windows Installer 布局)
# 用法: powershell -ExecutionPolicy Bypass -File tools\build.ps1
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

# ESP-IDF 环境参数
$env:IDF_PATH       = "C:\mysoft\ESP32\v5.5.5\esp-idf"
$env:IDF_TOOLS_PATH = "C:\mysoft\ESP32\tools"
$env:IDF_PYTHON_ENV_PATH = "$env:IDF_TOOLS_PATH\python_env\idf5.5_py3.11_env"
$env:ESP_ROM_ELF_DIR = "$env:IDF_TOOLS_PATH\tools\esp-rom-elfs\20241011\"
$pyEnv = "$env:IDF_TOOLS_PATH\python_env\idf5.5_py3.11_env"
$tools = "$env:IDF_TOOLS_PATH\tools"

# 构建所需工具加入 PATH
$env:PATH = @(
    "$pyEnv;$pyEnv\Scripts",
    "$tools\cmake\3.30.2\bin",
    "$tools\ninja\1.12.1",
    "$tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin",
    "$tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    "C:\Program Files\Git\cmd",
    $env:PATH
) -join ";"

Set-Location "D:\AI\ESP32_Projects\SmartKnob"
& "$pyEnv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" @args
exit $LASTEXITCODE