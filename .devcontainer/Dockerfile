# Kervan development environment: host toolchains (GCC, Clang) + ARM cross toolchain.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        clang \
        clang-format \
        clang-tidy \
        libclang-rt-18-dev \
        gcovr \
        gdb \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        libusb-1.0-0-dev \
        pkg-config \
        git \
        ca-certificates \
        python3 \
        python3-pip \
        curl \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Node.js for the web UI (web/); Ubuntu's own package is too old for Vite. Checksums from
# https://nodejs.org/dist/v22.23.3/SHASUMS256.txt
ARG NODE_VERSION=22.23.3
RUN set -eu; \
    case "$(dpkg --print-architecture)" in \
        amd64) arch=x64; sum=df450af89261115ef9f9e3830c3eeb2cc9213b63c720b1af623cb5dcbe2e02de ;; \
        arm64) arch=arm64; sum=a44aeb94849a299b22df10b9e622ec2f605c2183501bc40590705131de7c740f ;; \
        *) echo "no Node.js build for this architecture" >&2; exit 1 ;; \
    esac; \
    file="node-v${NODE_VERSION}-linux-${arch}.tar.xz"; \
    curl -fsSLo "/tmp/$file" "https://nodejs.org/dist/v${NODE_VERSION}/$file"; \
    echo "$sum  /tmp/$file" | sha256sum -c -; \
    tar -xJf "/tmp/$file" -C /usr/local --strip-components=1 --no-same-owner; \
    rm "/tmp/$file"; \
    node --version

# Raspberry Pi Pico SDK and picotool for the board firmware (firmware/pico_nrf24).
ARG PICO_SDK_VERSION=2.2.0
RUN set -eu; \
    git clone --depth 1 --branch "${PICO_SDK_VERSION}" https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk; \
    git -C /opt/pico-sdk submodule update --init --depth 1 lib/tinyusb lib/cyw43-driver; \
    git clone --depth 1 --branch "${PICO_SDK_VERSION}" https://github.com/raspberrypi/picotool.git /tmp/picotool; \
    cmake -S /tmp/picotool -B /tmp/picotool/build -G Ninja -DPICO_SDK_PATH=/opt/pico-sdk \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local > /dev/null; \
    cmake --build /tmp/picotool/build; \
    cmake --install /tmp/picotool/build > /dev/null; \
    rm -rf /tmp/picotool; \
    picotool version
ENV PICO_SDK_PATH=/opt/pico-sdk

# Tools Zephyr needs on top of the above.
RUN apt-get update && apt-get install -y --no-install-recommends -o Acquire::Retries=5 \
        python3-venv \
        device-tree-compiler \
        gperf \
        file \
        wget \
    && rm -rf /var/lib/apt/lists/*

# Zephyr RTOS for the board firmware on ESP32, STM32 and nRF boards (firmware/zephyr_nrf24): the
# workspace with only the HALs we use, and the SDK with the ARM, Xtensa (ESP32, ESP32-S3) and
# RISC-V (ESP32-C3) toolchains.
ARG ZEPHYR_VERSION=v4.4.2
ARG ZEPHYR_SDK_VERSION=1.0.1
ENV ZEPHYR_BASE=/opt/zephyrproject/zephyr \
    ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk \
    PATH=/opt/zephyr-venv/bin:$PATH
RUN set -eu; \
    python3 -m venv /opt/zephyr-venv; \
    pip install --no-cache-dir west; \
    west init -m https://github.com/zephyrproject-rtos/zephyr --mr "${ZEPHYR_VERSION}" /opt/zephyrproject; \
    cd /opt/zephyrproject; \
    west config manifest.project-filter -- '-.*,+cmsis,+cmsis_6,+hal_nordic,+hal_stm32,+hal_espressif,+picolibc'; \
    west update --narrow -o=--depth=1; \
    pip install --no-cache-dir -r zephyr/scripts/requirements-base.txt; \
    west blobs fetch hal_espressif; \
    rm -rf /root/.cache
RUN set -eu; \
    file="zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz"; \
    curl -fsSLo "/tmp/$file" "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/$file"; \
    echo "ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb  /tmp/$file" | sha256sum -c -; \
    mkdir -p /opt/zephyr-sdk; \
    tar -xJf "/tmp/$file" -C /opt/zephyr-sdk --strip-components=1; \
    rm "/tmp/$file"; \
    cd /opt/zephyr-sdk; \
    ./setup.sh -t arm-zephyr-eabi -t xtensa-espressif_esp32_zephyr-elf \
        -t xtensa-espressif_esp32s3_zephyr-elf -t riscv64-zephyr-elf -c; \
    rm -rf /root/.cache

# Python packages the Zephyr modules need (esptool for the Espressif SoCs).
RUN cd /opt/zephyrproject && west packages pip --install && rm -rf /root/.cache

# ubuntu:24.04 ships a default non-root user "ubuntu" (uid 1000); work as that user.
USER ubuntu
WORKDIR /workspaces/kervan
