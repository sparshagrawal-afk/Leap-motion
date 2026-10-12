#!/usr/bin/env bash
# Downloads a Whisper ggml model for the plugin.
#   ./scripts/download-model.sh            -> base.en (good default for English dialogue)
#   ./scripts/download-model.sh small      -> multilingual small
# Models: tiny[.en] base[.en] small[.en] medium[.en] large-v3 large-v3-turbo
set -euo pipefail
model="${1:-base.en}"
dir="$(cd "$(dirname "$0")/.." && pwd)/models"
mkdir -p "$dir"
url="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${model}.bin"
echo "Downloading $url"
curl -L --fail -o "$dir/ggml-${model}.bin" "$url"
echo "Saved to $dir/ggml-${model}.bin - choose it with 'Load Whisper model...' in the plugin."
