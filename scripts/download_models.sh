#!/usr/bin/env bash
# Download RF-DETR ONNX weights needed by object-classifier.
# Primary model: rf-detr-nano.onnx (COCO-80) via agentjetson/rf-detr.
#
# Usage:
#   ./scripts/download_models.sh              # nano only → models/
#   ./scripts/download_models.sh --all        # nano + small + base
#   ./scripts/download_models.sh --variant base
#   MODEL_DIR=/opt/models ./scripts/download_models.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODEL_DIR="${MODEL_DIR:-${REPO_ROOT}/models}"
HF_BASE="https://huggingface.co/PierreMarieCurie/rf-detr-onnx/resolve/main"

# Maps variant → remote HF filename → local filename
remote_name() {
  case "$1" in
    nano)   echo "rf-detr-nano.onnx" ;;
    small)  echo "rf-detr-small.onnx" ;;
    base)   echo "rf-detr-base-coco.onnx" ;;
    medium) echo "rf-detr-medium.onnx" ;;
    *) return 1 ;;
  esac
}

local_name() {
  case "$1" in
    nano)   echo "rf-detr-nano.onnx" ;;
    small)  echo "rf-detr-small.onnx" ;;
    base)   echo "rf-detr-base.onnx" ;;
    medium) echo "rf-detr-medium.onnx" ;;
    *) return 1 ;;
  esac
}

download_one() {
  local variant="$1"
  local remote
  local dest_name
  remote="$(remote_name "$variant")" || {
    echo "error: unknown variant '$variant' (known: nano small base medium)" >&2
    return 1
  }
  dest_name="$(local_name "$variant")"
  local dest="${MODEL_DIR}/${dest_name}"

  if [[ -f "$dest" ]]; then
    local size
    size=$(wc -c < "$dest" | tr -d ' ')
    if [[ "$size" -gt 1000000 ]]; then
      echo "already present: $dest (${size} bytes)"
      return 0
    fi
    echo "removing incomplete file: $dest"
    rm -f "$dest"
  fi

  mkdir -p "$MODEL_DIR"
  local url="${HF_BASE}/${remote}"
  echo "downloading ${variant} → ${dest}"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --progress-bar -o "${dest}.partial" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --show-progress -O "${dest}.partial" "$url"
  else
    echo "error: need curl or wget" >&2
    return 1
  fi
  mv "${dest}.partial" "$dest"
  echo "  ok: $dest ($(wc -c < "$dest" | tr -d ' ') bytes)"
}

ALL=0
REQUESTED=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --all) ALL=1; shift ;;
    --variant)
      REQUESTED="${REQUESTED} $2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,12p' "$0"
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

if [[ $ALL -eq 1 ]]; then
  REQUESTED="nano small base"
elif [[ -z "${REQUESTED// }" ]]; then
  REQUESTED="nano"
fi

echo "Model directory: $MODEL_DIR"
for v in $REQUESTED; do
  download_one "$v"
done

echo ""
echo "Done. Run object-classifier with e.g.:"
echo "  ORT_DEVICE=cpu ./build/object_classifier --in-process sample.mp4 ${MODEL_DIR}/rf-detr-nano.onnx"
