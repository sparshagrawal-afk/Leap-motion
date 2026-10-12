# Downloads a Whisper ggml model for the plugin (Windows).
#   .\scripts\download-model.ps1            -> base.en
#   .\scripts\download-model.ps1 small      -> multilingual small
param([string]$Model = "base.en")
$dir = Join-Path (Split-Path $PSScriptRoot -Parent) "models"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$Model.bin"
$out = Join-Path $dir "ggml-$Model.bin"
Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $out
Write-Host "Saved to $out - choose it with 'Load Whisper model...' in the plugin."
