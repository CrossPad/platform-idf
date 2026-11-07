#!/usr/bin/env bash
# Self-test of the crosspad-ci image: every tool at its pinned version, and a
# build through each toolchain the CrossPad repos use, without network access
# (ci-image.yml runs it under --network none: the image must be complete) and
# without /cache.
#   docker run --rm --network none <image> /opt/crosspad-ci/smoke.sh
set -euo pipefail

w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
export CCACHE_DIR="$w/ccache" PIP_CACHE_DIR="$w/pip" IDF_COMPONENT_CACHE_PATH="$w/idf-components"

step() { printf '\n== %s\n' "$*"; }
expect() {  # expect <what> <pattern> <command...>
  local what=$1 pat=$2; shift 2
  local out; out=$("$@" 2>&1 | head -n 3)
  grep -qE -- "$pat" <<<"$out" || { echo "FAIL $what: wanted /$pat/, got: $out"; exit 1; }
  echo "ok   $what: $(grep -m 1 -E -- "$pat" <<<"$out")"
}

step "versions"
expect arm-none-eabi-gcc '14\.3\.Rel1' arm-none-eabi-gcc --version
expect cmake          '3\.30\.2'      cmake --version
expect ninja          '^1\.11\.1'     ninja --version
expect ccache         '4\.9\.1'       ccache --version
expect g++            '13\.3\.0'      g++ --version
expect git            '2\.43\.'       git --version
expect python3        '3\.12\.3'      python3 --version
expect python3.10     '3\.10\.22'     python3.10 --version
expect openssl        '^OpenSSL 3\.0' openssl version
expect doxygen        '^1\.16\.1'     doxygen --version
expect dot            'graphviz'      dot -V
expect "git http.version"  '^HTTP/1\.1$' git config --system --get http.version
expect "git safe.directory" '^\*$'      git config --system --get-all safe.directory

step "ESP-IDF export"
plain_path=$PATH
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null
expect idf.py            'ESP-IDF v5\.5\.5' idf.py --version
expect xtensa-esp-elf    'esp-14\.2\.0_20260121' xtensa-esp32s3-elf-gcc --version
expect riscv32-esp-elf   'esp-14\.2\.0_20260121' riscv32-esp-elf-gcc --version
expect esp32ulp-elf      '2\.38'                 esp32ulp-elf-as --version
expect esptool           '4\.12\.0'              python -m esptool version

step "ESP-IDF: esp32s3 app with C++ and a ULP RISC-V program"
p="$w/idf"
mkdir -p "$p/main" "$p/ulp"
cat > "$p/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smoke)
EOF
cat > "$p/sdkconfig.defaults" <<'EOF'
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ULP_COPROC_ENABLED=y
CONFIG_ULP_COPROC_TYPE_RISCV=y
CONFIG_ULP_COPROC_RESERVE_MEM=4096
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
EOF
cat > "$p/main/CMakeLists.txt" <<'EOF'
idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "" REQUIRES ulp bt esp_wifi nvs_flash)
ulp_embed_binary(ulp_main "../ulp/main.c" "main.cpp")
EOF
cat > "$p/main/main.cpp" <<'EOF'
#include <string>
#include <vector>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nimble/nimble_port.h"
#include "ulp_riscv.h"
#include "ulp_main.h"
extern const uint8_t ulp_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_bin_end[] asm("_binary_ulp_main_bin_end");
extern "C" void app_main(void) {
    std::vector<std::string> v{"cross", "pad"};
    ESP_LOGI("smoke", "%s%s %d", v[0].c_str(), v[1].c_str(), (int)ulp_counter);
    ulp_riscv_load_binary(ulp_bin_start, ulp_bin_end - ulp_bin_start);
    ulp_riscv_run();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    nimble_port_init();
}
EOF
cat > "$p/ulp/main.c" <<'EOF'
#include <stdint.h>
#include "ulp_riscv_utils.h"
volatile uint32_t counter;
int main(void) { counter++; return 0; }
EOF
idf.py -C "$p" -B "$p/build" build >"$w/idf.log" 2>&1 || { tail -n 60 "$w/idf.log"; exit 1; }
test -s "$p/build/smoke.bin" && test -s "$p/build/esp-idf/main/ulp_main/ulp_main.bin"
echo "ok   idf.py build: $(stat -c %s "$p/build/smoke.bin") B app, $(stat -c %s "$p/build/esp-idf/main/ulp_main/ulp_main.bin") B ULP"
# Leave the IDF environment: everything below uses the image's plain PATH.
PATH=$plain_path
hash -r
expect "python3 after IDF" '3\.12\.3' python3 --version

step "Arm: Cortex-M0+ with LTO and newlib-nano"
cat > "$w/m0.c" <<'EOF'
#include <stdio.h>
#include <string.h>
int main(void) { char b[16]; snprintf(b, sizeof b, "%d", 42); return (int)strlen(b); }
EOF
arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -flto -ffat-lto-objects --specs=nano.specs --specs=nosys.specs \
  -Wl,--gc-sections "$w/m0.c" -o "$w/m0.elf" >/dev/null 2>&1   # nosys stub warnings
arm-none-eabi-objcopy -O binary "$w/m0.elf" "$w/m0.bin"
echo "ok   arm-none-eabi: $(stat -c %s "$w/m0.bin") B"

step "Host C++: g++, zlib, cmake + ninja + ccache"
mkdir -p "$w/host"
cat > "$w/host/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.14)
project(host CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(ZLIB REQUIRED)
add_executable(z z.cpp)
target_link_libraries(z ZLIB::ZLIB)
EOF
cat > "$w/host/z.cpp" <<'EOF'
#include <zlib.h>
#include <cstdio>
int main() { std::printf("zlib %s\n", zlibVersion()); return 0; }
EOF
cmake -S "$w/host" -B "$w/host/build" -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=ccache >/dev/null
cmake --build "$w/host/build" >/dev/null
expect "host binary" '^zlib 1\.3' "$w/host/build/z"

step "Python for crosspad-hil: venvs, ALSA and PortAudio"
for py in 3.12 3.10; do
  "python$py" -m venv "$w/venv$py"   # what crosspad-hil's workflow does
  "$w/venv$py/bin/python" -m pip --version >/dev/null
  "$w/venv$py/bin/python" - <<'EOF'
import ctypes, ctypes.util, sys
for lib in ("asound", "portaudio"):
    name = ctypes.util.find_library(lib)
    assert name, f"lib{lib} not found"
    ctypes.CDLL(name)
print(f"ok   python {sys.version.split()[0]} venv with pip; libasound + libportaudio load")
EOF
done

step "Release tools"
printf 'x' > "$w/f" && sha256sum "$w/f" >/dev/null
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$w/k.pem" 2>/dev/null
openssl dgst -sha256 -sign "$w/k.pem" -out "$w/f.sig" "$w/f"
echo "ok   sha256sum, openssl EC sign"

printf '\nsmoke OK (%s)\n' "${CROSSPAD_CI_IMAGE:-?}"
