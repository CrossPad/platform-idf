# crosspad-ci: the one CI image for every CrossPad repository. The repos differ
# in their C/C++ sources, not in their tools.
#
#   platform-idf       ESP-IDF v5.5.5 for esp32s3 only (xtensa-esp-elf, plus
#                      riscv32-esp-elf for the ULP RISC-V and esp32ulp-elf),
#                      IDF Python env pinned to what espressif/idf:v5.5.5 had,
#                      openssl/git/sha256sum for the release job
#   CrossPad_STM32_r2  Arm GNU Toolchain 14.3.Rel1, Cortex-M0+ multilib only
#                      (14.2 overflows flash_guard by ~100 B; 14.3 is checked by sha256)
#   crosspad-core/gui  g++ 13.3, zlib, doxygen 1.16.1 + graphviz
#   crosspad-hil       Python 3.12 (system, the same as IDF's) and 3.10.22 (standalone),
#                      ALSA + PortAudio runtime libraries
#   all of them        cmake 3.30.2 (IDF's), ninja 1.11.1, ccache 4.9.1, git 2.43
#
# Published by .github/workflows/ci-image.yml as
#   ghcr.io/crosspad/crosspad-ci:<IDF_VERSION>-<CI_IMAGE_REV>   (e.g. v5.5.5-1)
# Change anything in tools/ci/ -> bump CI_IMAGE_REV; a published tag is never
# overwritten. New IDF: IDF_VERSION + IDF_COMMIT + idf-python-constraints.txt
# (pip freeze of espressif/idf:<new>) + tool paths below, CI_IMAGE_REV back to 1.
# Self-test: docker run --rm --network none <image> /opt/crosspad-ci/smoke.sh
# No `# syntax=` line on purpose: the builder's own Dockerfile frontend
# handles everything here, and a syntax line would pull an unpinned
# frontend image from Docker Hub into every build.

ARG UBUNTU=ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

# Python 3.10 for crosspad-hil (3.12 is the system one): a pinned
# python-build-standalone, installed by uv in a stage of its own, so uv itself
# stays out of the image.
FROM ${UBUNTU} AS python310
ARG PYTHON310_VERSION=3.10.22
COPY --from=ghcr.io/astral-sh/uv:0.12.23@sha256:61d393e44e249f2e4b526b6c7ddcecce245946826e608e11c93ad4f5bba55b21 /uv /usr/local/bin/uv
RUN UV_PYTHON_INSTALL_DIR=/opt/python uv python install --no-cache --no-bin "${PYTHON310_VERSION}" \
 && cd /opt/python/cpython-${PYTHON310_VERSION}-linux-x86_64-gnu/lib/python3.10 \
 && rm -rf test idlelib tkinter turtledemo lib2to3/tests

# ubuntu:24.04 (noble), the base of espressif/idf:v5.5.5 and of GitHub's ubuntu-24.04:
# same glibc, g++ 13.3, Python 3.12.3, git 2.43, ccache 4.9.1, ninja 1.11.1.
FROM ${UBUNTU}

ARG IDF_VERSION=v5.5.5
ARG IDF_COMMIT=b774170ff46c393eeb5e495ea37936038d3f4f4f
ARG CI_IMAGE_REV=1
ARG ARM_GCC_VERSION=14.3.rel1
ARG ARM_GCC_SHA256=8f6903f8ceb084d9227b9ef991490413014d991874a1e34074443c2a72b14dbd
ARG PYTHON310_VERSION=3.10.22
ARG DOXYGEN_VERSION=1.16.1
ARG DOXYGEN_SHA256=a56f885d37e3aae08a99f638d17bbb381224c03a878d9e2dda4f9fa4baf1d8bd

LABEL org.opencontainers.image.source="https://github.com/CrossPad/platform-idf" \
      org.opencontainers.image.title="crosspad-ci" \
      org.opencontainers.image.description="CrossPad CI: ESP-IDF ${IDF_VERSION} (esp32s3), arm-none-eabi ${ARM_GCC_VERSION}, host C++ and Python" \
      org.opencontainers.image.version="${IDF_VERSION}-${CI_IMAGE_REV}"

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8
SHELL ["/bin/bash", "-euo", "pipefail", "-c"]

# --- System packages ---------------------------------------------------------
# No recommends: graphviz alone would pull in ~200 MB of fonts and X libraries.
# Noble's versions are fixed (only the Ubuntu revision moves with security
# updates); smoke.sh checks the ones the builds depend on.
# git 2.43 over HTTP/2 cannot list refs from github.com ("expected flush after
# ref listing"; the component manager's esp-now clone dies on it), and jobs run
# as root on a workspace created by the runner's uid: both go to /etc/gitconfig.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      bzip2 ca-certificates ccache curl g++ git graphviz \
      libasound2t64 libportaudio2 make ninja-build openssl \
      python3 python3-venv unzip xz-utils zlib1g-dev zstd \
 && rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb \
 && git config --system http.version HTTP/1.1 \
 && git config --system --add safe.directory '*'

# --- Arm GNU Toolchain (STM32G0B1, Cortex-M0+) -------------------------------
# Only the thumb/v6-m/nofp multilib is kept (what -mcpu=cortex-m0plus -mthumb
# selects); no gdb, Fortran, docs. 1009 MB -> ~0.2 GB.
RUN curl -fsSL -o /tmp/arm.tar.xz \
      "https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu/${ARM_GCC_VERSION}/binrel/arm-gnu-toolchain-${ARM_GCC_VERSION}-x86_64-arm-none-eabi.tar.xz" \
 && echo "${ARM_GCC_SHA256}  /tmp/arm.tar.xz" | sha256sum -c - \
 && mkdir -p /opt/arm-none-eabi \
 && tar -xJf /tmp/arm.tar.xz -C /opt/arm-none-eabi --strip-components=1 \
 && rm /tmp/arm.tar.xz \
 && cd /opt/arm-none-eabi \
 && rm -rf share/doc share/info share/man share/gdb share/gcc-* \
           bin/arm-none-eabi-gdb* bin/arm-none-eabi-gfortran bin/arm-none-eabi-lto-dump \
           libexec/gcc/arm-none-eabi/*/f951 lib/gcc/arm-none-eabi/*/plugin \
 && for d in arm-none-eabi/lib lib/gcc/arm-none-eabi/*; do \
      rm -rf "$d/arm"; \
      find "$d/thumb" -mindepth 1 -maxdepth 1 ! -name v6-m -exec rm -rf {} +; \
      find "$d/thumb/v6-m" -mindepth 1 -maxdepth 1 ! -name nofp -exec rm -rf {} +; \
    done

# --- ESP-IDF source ----------------------------------------------------------
# Shallow, without .git (3.3 GB in espressif/idf), docs, examples, test apps.
# version.txt makes IDF_VER "v5.5.5", what `git describe` gave in espressif/idf,
# so esp_app_desc is unchanged.
ENV IDF_PATH=/opt/esp/idf \
    IDF_TOOLS_PATH=/opt/esp
RUN git clone --quiet --depth 1 --branch "${IDF_VERSION}" --recurse-submodules \
      --shallow-submodules --jobs 8 https://github.com/espressif/esp-idf.git "${IDF_PATH}" \
 && test "$(git -C "${IDF_PATH}" rev-parse HEAD)" = "${IDF_COMMIT}" \
 && echo "${IDF_VERSION}" > "${IDF_PATH}/version.txt" \
 && git -C "${IDF_PATH}" submodule status --recursive > "${IDF_PATH}/submodules.txt" \
 && find "${IDF_PATH}" -name .git -prune -exec rm -rf {} + \
 && rm -rf "${IDF_PATH}/docs" "${IDF_PATH}/examples" "${IDF_PATH}/tools/test_apps" \
 && find "${IDF_PATH}/components" -mindepth 2 -maxdepth 2 -type d -name test_apps -exec rm -rf {} +

# --- ESP-IDF tools, esp32s3 only ---------------------------------------------
# riscv32-esp-elf stays: the ULP is RISC-V (CONFIG_ULP_COPROC_TYPE_RISCV=y) and
# needs only rv32imc_zicsr_zifencei/ilp32 (IDF always passes that -march).
# xtensa keeps the esp32s3 multilib (what the xtensa-esp32s3-elf-* drivers
# select). Not kept: the other chips' and the default multilibs, picolibc (IDF
# 6; v5.5 links newlib), lto-dump, docs, and gdb/openocd/qemu altogether
# (IDF_SKIP_TOOLS_CHECK below lets export.sh go on without them).
RUN python3 "${IDF_PATH}/tools/idf_tools.py" --non-interactive install --targets=esp32s3 \
      xtensa-esp-elf riscv32-esp-elf esp32ulp-elf esp-rom-elfs cmake \
 && rm -rf "${IDF_TOOLS_PATH}/dist" \
 && cd "${IDF_TOOLS_PATH}/tools" \
 && x=$(echo xtensa-esp-elf/*/xtensa-esp-elf) \
 && rm -rf "$x/picolibc" "$x/share/doc" "$x/share/info" "$x/share/man" "$x"/bin/*-lto-dump \
           "$x/xtensa-esp-elf/lib/esp32" "$x/xtensa-esp-elf/lib/esp32s2" "$x"/xtensa-esp-elf/lib/*.a \
           "$x"/lib/gcc/xtensa-esp-elf/*/esp32 "$x"/lib/gcc/xtensa-esp-elf/*/esp32s2 \
           "$x"/lib/gcc/xtensa-esp-elf/*/plugin \
 && r=$(echo riscv32-esp-elf/*/riscv32-esp-elf) \
 && rm -rf "$r/picolibc" "$r/share/doc" "$r/share/info" "$r/share/man" "$r"/bin/*-lto-dump \
           "$r"/lib/gcc/riscv32-esp-elf/*/plugin "$r/riscv32-esp-elf/lib/no-rtti" "$r"/riscv32-esp-elf/lib/*.a \
 && for d in "$r/riscv32-esp-elf/lib" "$r"/lib/gcc/riscv32-esp-elf/*; do \
      find "$d" -mindepth 1 -maxdepth 1 -type d -name 'rv32*' ! -name rv32imc_zicsr_zifencei -exec rm -rf {} +; \
    done \
 && c=$(echo cmake/*) \
 && rm -rf "$c/doc" "$c/man" "$c/bin/cmake-gui" "$c/bin/ccmake" "$c/share/vim" "$c/share/emacs" \
           "$c/share/bash-completion" "$c/share/aclocal" "$c/share/applications" "$c/share/icons" "$c/share/mime"

# --- ESP-IDF Python env ------------------------------------------------------
# The constraints file is the exact freeze of espressif/idf:v5.5.5 (esptool
# 4.12.0 writes the .bin, the component manager 2.5.0 resolves dependencies).
# Fresh mtime: idf_tools.py then uses it instead of downloading today's.
COPY idf-python-constraints.txt ${IDF_TOOLS_PATH}/espidf.constraints.v5.5.txt
RUN touch "${IDF_TOOLS_PATH}/espidf.constraints.v5.5.txt" \
 && python3 "${IDF_PATH}/tools/idf_tools.py" --non-interactive install-python-env \
 && find "${IDF_TOOLS_PATH}/python_env" -name __pycache__ -prune -exec rm -rf {} + \
 && rm -rf /root/.cache

# --- Python 3.10 for crosspad-hil (from the python310 stage) -----------------
COPY --from=python310 /opt/python /opt/python
RUN ln -s /opt/python/cpython-${PYTHON310_VERSION}-linux-x86_64-gnu/bin/python3.10 /usr/local/bin/python3.10 \
 && python3.10 -c 'import ssl, sqlite3, ctypes, venv'

# --- Doxygen (the version crosspad-core pins; Ubuntu's 1.9.8 pulls in LLVM) ---
RUN curl -fsSL -o /tmp/doxygen.tgz "https://www.doxygen.nl/files/doxygen-${DOXYGEN_VERSION}.linux.bin.tar.gz" \
 && echo "${DOXYGEN_SHA256}  /tmp/doxygen.tgz" | sha256sum -c - \
 && tar -xzf /tmp/doxygen.tgz -C /tmp "doxygen-${DOXYGEN_VERSION}/bin/doxygen" \
 && install -m 755 "/tmp/doxygen-${DOXYGEN_VERSION}/bin/doxygen" /usr/local/bin/doxygen \
 && strip --strip-unneeded /usr/local/bin/doxygen \
 && rm -rf /tmp/doxygen.tgz "/tmp/doxygen-${DOXYGEN_VERSION}"

COPY smoke.sh /opt/crosspad-ci/smoke.sh
# Open to any uid: a job may run as non-root, and a fresh named volume mounted
# here takes this mode.
RUN mkdir -m 1777 /cache

# IDF needs `. $IDF_PATH/export.sh` for idf.py (it switches python3 to the IDF
# venv, which the release scripts and crosspad-hil must not get). Everything
# else is on PATH as is. /cache is where the workflows mount the shared cache
# volume (ccache, pip, IDF components); without a volume it is plain disk.
ENV IDF_PYTHON_CHECK_CONSTRAINTS=no \
    IDF_SKIP_TOOLS_CHECK=1 \
    IDF_SKIP_CHECK_SUBMODULES=1 \
    IDF_CCACHE_ENABLE=1 \
    IDF_COMPONENT_CACHE_PATH=/cache/idf-components \
    CCACHE_DIR=/cache/ccache \
    PIP_CACHE_DIR=/cache/pip \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    CROSSPAD_CI_IMAGE=${IDF_VERSION}-${CI_IMAGE_REV} \
    PATH=/opt/arm-none-eabi/bin:/opt/esp/tools/cmake/3.30.2/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

CMD ["/bin/bash"]
