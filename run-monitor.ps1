$ErrorActionPreference = "Stop"

Set-Location -LiteralPath $PSScriptRoot

if (-not (Test-Path -LiteralPath ".\.venv\Scripts\python.exe")) {
    Write-Error "Virtual environment not found at .\.venv. Create it first, then run pip install -e ."
}

.\.venv\Scripts\python.exe -m npb_monitor --http-host 127.0.0.1 --http-port 8088
