#!/bin/bash
# Build (and optionally flash) ESP-AT for the ESP32-C3 with the AT interface on SPI.
#
# ESP-AT (submodule esp_at, pinned to a release tag) builds against the
# ESP-IDF version that release pins. That ESP-IDF is cloned into
# esp_at/esp-idf (ESP-AT patches it at build time), separate from
# ~/esp/esp-idf, which this script doesn't touch. Our settings live in
# at_firmware/module_cfg and go on top of ESP-AT's module defaults
# (AT_EXT_MODULE_CFG).
#
# Usage:
#   at_firmware/build.sh                 build
#   at_firmware/build.sh flash <port>    build and flash, e.g. /dev/ttyACM0
#   at_firmware/build.sh clean           remove the build output (keeps esp-idf)
#
# Needs the usual ESP-IDF prerequisites (see README).
set -e

here="$(cd "$(dirname "$0")" && pwd)"
at_dir="$here/../esp_at"
platform="ESP32C3"
module="ESP32C3-SPI"
target="esp32c3"
silence=0

if [ ! -f "$at_dir/build.py" ]; then
    echo "esp_at submodule missing: git submodule update --init esp_at" >&2
    exit 1
fi

# An exported ESP-IDF (e.g. ~/esp/esp-idf) would override the one ESP-AT pins,
# and ESP-IDF's installer refuses to run from inside a virtual environment.
unset IDF_PATH IDF_TOOLS_EXPORT_CMD IDF_PYTHON_ENV_PATH ESP_IDF_VERSION VIRTUAL_ENV
export AT_EXT_MODULE_CFG="$here/module_cfg"

cd "$at_dir"

if [ "$1" = "clean" ]; then
    rm -rf build sdkconfig
    exit 0
fi

# Answer build.py's platform/module/silence questions in advance.
mkdir -p build
cat > build/module_info.json <<JSON
{"platform": "PLATFORM_$platform", "module": "$module", "description": "", "silence": $silence}
JSON

if [ ! -f esp-idf/install.sh ]; then
    # Clone the ESP-IDF this ESP-AT release pins, with build.py's own clone
    # code. build.py needs a few Python modules; give it a small venv for this
    # one step rather than installing into the system Python.
    if [ ! -x .venv/bin/python ]; then
        python3 -m venv .venv
        .venv/bin/python -m pip install -q -r requirements.txt
    fi
    .venv/bin/python -c "import build; build.at_submodules_update('$platform', '$module')"
fi

# Toolchain and ESP-IDF's own Python env, the standard ESP-IDF way
# (only does work the first time).
./esp-idf/install.sh "$target" > /dev/null

# shellcheck disable=SC1091
. ./esp-idf/export.sh > /dev/null

# ESP-AT's extra Python packages go into ESP-IDF's env
if ! python -c "import xlrd, yaml" 2> /dev/null; then
    python -m pip install -q -r requirements.txt
fi

python build.py build

if [ "$1" = "flash" ]; then
    port="${2:?usage: $0 flash <port>}"
    python build.py -p "$port" flash
fi
