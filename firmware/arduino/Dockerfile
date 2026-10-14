# Arduino toolchain for the Uno R4 runtime sketch, pinned to what the hand builds on Windows used:
# arduino-cli 1.5.2-rc.1 and the arduino:renesas_uno core 1.6.0 (which brings its own arm-none-eabi-gcc 7-2017q4).
FROM debian:bookworm-slim
ARG CLI=1.5.2-rc.1
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL "https://downloads.arduino.cc/arduino-cli/arduino-cli_${CLI}_Linux_64bit.tar.gz" \
       | tar -xz -C /usr/local/bin arduino-cli
ENV ARDUINO_DIRECTORIES_DATA=/opt/arduino ARDUINO_DIRECTORIES_DOWNLOADS=/opt/arduino/staging
RUN arduino-cli core update-index && arduino-cli core install arduino:renesas_uno@1.6.0 \
    && chmod -R a+rX /opt/arduino && arduino-cli version && arduino-cli core list
