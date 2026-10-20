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
    && rm -rf /var/lib/apt/lists/*

# ubuntu:24.04 ships a default non-root user "ubuntu" (uid 1000); work as that user.
USER ubuntu
WORKDIR /workspaces/kervan
