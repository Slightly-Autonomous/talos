# The same portable sources Bazel builds for host tests.
set(TALOS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
idf_component_register(
  SRCS
    "main.cc"
    "esp_platform.cc"
    "${TALOS_ROOT}/firmware/portable/modules.cc"
    "${TALOS_ROOT}/firmware/portable/ota.cc"
    "${TALOS_ROOT}/firmware/portable/runtime.cc"
    "${TALOS_ROOT}/common/sha256/sha256.cc"
  INCLUDE_DIRS "." "${TALOS_ROOT}"
  REQUIRES driver esp_driver_i2c esp_driver_gpio esp_timer nvs_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
