#!/usr/bin/env bash
# Build the example firmware and stage flashable images in dist/.
#
#   scripts/ci-build.sh make    # legacy GNU Make build system
#   scripts/ci-build.sh cmake   # CMake / idf.py build system
#
# Requires IDF_PATH to point at ESP8266_RTOS_SDK and xtensa-lx106-elf-gcc on
# PATH. Used by .github/workflows/ci.yml; run it locally to reproduce CI.
set -euo pipefail

build_system=${1:?usage: $0 make|cmake}
project=softuart-example
: "${IDF_PATH:?IDF_PATH must point at ESP8266_RTOS_SDK}"
python=${PYTHON:-python}
jobs=${JOBS:-$(nproc)}
# Treat compiler warnings in the softuart component and the example as errors
export SOFTUART_WERROR=1

cd "$(dirname "$0")/.."
rm -rf dist
mkdir -p dist

case "$build_system" in
make)
    # A fresh checkout has no sdkconfig; generate it from sdkconfig.defaults
    # first, otherwise make tries to start the interactive menuconfig.
    make defconfig BATCH_BUILD=1 PYTHON="$python"
    make -j"$jobs" BATCH_BUILD=1 PYTHON="$python"
    cp build/partitions_singleapp.bin dist/partition-table.bin
    ;;
cmake)
    "$python" "$IDF_PATH/tools/idf.py" build
    cp build/partition_table/partition-table.bin dist/
    ;;
*)
    echo "unknown build system: $build_system" >&2
    exit 2
    ;;
esac

cp build/bootloader/bootloader.bin \
   "build/$project.bin" "build/$project.elf" "build/$project.map" dist/

# Offsets match the single-app partition table used by this project.
cat > dist/flash_args <<ARGS
--flash_mode dio --flash_freq 40m --flash_size 2MB
0x0 bootloader.bin
0x8000 partition-table.bin
0x10000 $project.bin
ARGS

"$python" "$IDF_PATH/tools/idf_size.py" "build/$project.map" | tee dist/size.txt
echo "Firmware staged in dist/:"
ls -l dist
