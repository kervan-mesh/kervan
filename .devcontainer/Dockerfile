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

# ubuntu:24.04 ships a default non-root user "ubuntu" (uid 1000); work as that user.
USER ubuntu
WORKDIR /workspaces/kervan
