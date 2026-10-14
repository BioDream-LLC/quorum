# Build image for quorum studio: Go, plus MinGW-w64 to cross-compile quorum's C++ tools for Windows.
FROM golang:1.27.1-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-mingw-w64-x86-64-posix cmake make nsis osslsigncode openssl \
    && rm -rf /var/lib/apt/lists/*
