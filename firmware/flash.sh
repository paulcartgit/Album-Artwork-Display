#!/bin/zsh
cd "$(dirname "$0")" && ~/.platformio/penv/bin/pio run -e esp32-s3-photopainter -t upload
