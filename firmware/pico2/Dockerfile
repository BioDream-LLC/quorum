# Build environment for firmware/pico2 -- Raspberry Pi Pico 2 (RP2350, Cortex-M33).
#
# Why this exists as its own image: quorum-arm:1 is a bare cross-compiler, and the
# Pico SDK needs a host toolchain as well as a target one. It builds pioasm and
# picotool for the host during the target build, so cmake, g++ and python3 all have
# to be present alongside arm-none-eabi-gcc. libusb is picotool's dependency.
#
#   docker build -t quorum-pico:1 -f firmware/pico2/Dockerfile .
#
FROM quorum-arm:1

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake \
        g++ \
        make \
        python3 \
        git \
        ca-certificates \
        libusb-1.0-0-dev \
        pkg-config \
        xxd \
    && rm -rf /var/lib/apt/lists/*

# The SDK is mounted rather than baked in, so its version is visible in the build
# command and can be pinned per-run instead of hidden inside an image layer.
ENV PICO_SDK_PATH=/pico-sdk
WORKDIR /src
