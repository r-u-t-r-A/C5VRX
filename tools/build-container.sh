#!/usr/bin/env bash
# Build this firmware with the exact pinned toolchain (ESP-IDF v6.0.2) in a
# container; no local ESP-IDF install needed. Output: build/c5div_rx.bin and
# build/c5div_rx_merged.bin (flash at 0x0).
#   tools/build-container.sh            # podman or docker
#   tools/build-container.sh -DC5DIV_DEMOD=P64   # other boot decoder
set -euo pipefail
cd "$(dirname "$0")/.."
ENGINE=$(command -v podman || command -v docker)
"$ENGINE" run --rm -e EXTRA="$*" -v "$PWD":/project:Z -w /project docker.io/espressif/idf:v6.0.2 bash -c '
  set -euo pipefail
  git config --global --add safe.directory /project
  . "$IDF_PATH/export.sh" >/dev/null 2>&1
  idf.py $EXTRA build
  name=$(python3 -c "import json;print(json.load(open(\"build/project_description.json\"))[\"project_name\"])")
  esptool.py --chip esp32c5 merge_bin -o "build/${name}_merged.bin" \
    --flash_mode dio --flash_size 8MB --flash_freq 80m \
    0x2000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
    0x10000 "build/${name}.bin" >/dev/null
  echo "built build/${name}.bin and build/${name}_merged.bin"
'
