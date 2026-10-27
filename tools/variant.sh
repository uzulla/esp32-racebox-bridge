# Sourced by the mise fw:* tasks: maps VARIANT to the IDF target and the frontend (display).
#   c6-geek  Waveshare ESP32-C6-GEEK (ESP32-C6 + ST7789 LCD)
#   c6       ESP32-C6 board without a display
#   s3       ESP32-S3 board without a display
case "${VARIANT:?VARIANT is not set}" in
  c6-geek) IDF_TARGET_NAME=esp32c6 FRONTEND=lcd ;;
  c6)      IDF_TARGET_NAME=esp32c6 FRONTEND=none ;;
  s3)      IDF_TARGET_NAME=esp32s3 FRONTEND=none ;;
  *) echo "unknown VARIANT '$VARIANT' (c6-geek | c6 | s3)" >&2; exit 1 ;;
esac
# Own build directory and sdkconfig per variant, so switching never needs a clean build.
IDF_ARGS="-B build-$VARIANT -DSDKCONFIG=sdkconfig.$VARIANT -DRACEBOX_FRONTEND=$FRONTEND"
