# Pinned bare-metal ARM toolchain for building and measuring quorum firmware.
#
# A container rather than a host install, deliberately. A safety product's footprint numbers are only meaningful if
# the toolchain that produced them is identified, and "whatever was on the developer's laptop" is not an
# identification. This image is the toolchain of record; the digest can go in a release note.
FROM debian:12-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc-arm-none-eabi \
      binutils-arm-none-eabi \
      libnewlib-arm-none-eabi \
      libstdc++-arm-none-eabi-dev \
      libstdc++-arm-none-eabi-newlib \
      make \
 && rm -rf /var/lib/apt/lists/*

# The C++ headers are needed even though NO libstdc++ code is linked: <cstdint> and <cstddef> are headers, and a
# freestanding C++17 implementation is still required to provide them. The link stays -nostdlib.
RUN arm-none-eabi-g++ --version && arm-none-eabi-size --version
WORKDIR /src
