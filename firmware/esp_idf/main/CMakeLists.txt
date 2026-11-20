# The same portable sources Bazel builds for host tests.
set(TALOS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
idf_component_register(
  SRCS
    "main.cc"
    "esp_platform.cc"
    "${TALOS_ROOT}/firmware/portable/board_io.cc"
    "${TALOS_ROOT}/firmware/portable/coap_link.cc"
    "${TALOS_ROOT}/firmware/portable/serial_link.cc"
    "${TALOS_ROOT}/firmware/portable/wire.cc"
    "${TALOS_ROOT}/common/coap/coap.cc"
    "${TALOS_ROOT}/firmware/portable/diag.cc"
    "${TALOS_ROOT}/firmware/portable/modules.cc"
    "${TALOS_ROOT}/firmware/portable/ota.cc"
    "${TALOS_ROOT}/firmware/portable/runtime.cc"
    "${TALOS_ROOT}/common/sha256/sha256.cc"
  INCLUDE_DIRS "." "${TALOS_ROOT}"
  REQUIRES driver esp_adc esp_driver_usb_serial_jtag esp_driver_i2c esp_driver_gpio esp_timer nvs_flash esp_system app_update)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
