# SPDX-License-Identifier: Apache-2.0
#
# tests/baremetal/ -- plain-CMake static archive regressions for the
# baremetal SDK backend.  Activated from tests/CMakeLists.txt when
# ALP_OS=baremetal; silent otherwise.

add_executable(alp_test_baremetal_registry_static_link
    ${CMAKE_CURRENT_SOURCE_DIR}/registry_static_link.c
)
target_link_libraries(alp_test_baremetal_registry_static_link PRIVATE alp::sdk)
target_include_directories(alp_test_baremetal_registry_static_link PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../yocto
)
target_compile_features(alp_test_baremetal_registry_static_link PRIVATE c_std_99)

add_test(NAME alp_test_baremetal_registry_static_link
         COMMAND alp_test_baremetal_registry_static_link)

# Delay fallback regression (#594) -- shared with tests/yocto/ since both
# link src/common/stub/stub_core.c's alp_delay_us/alp_delay_ms.
add_executable(alp_test_baremetal_delay
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/delay.c
)
target_link_libraries(alp_test_baremetal_delay PRIVATE alp::sdk)
target_include_directories(alp_test_baremetal_delay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../yocto
)
target_compile_features(alp_test_baremetal_delay PRIVATE c_std_99)

add_test(NAME alp_test_baremetal_delay
         COMMAND alp_test_baremetal_delay)

# EEPROM 24C128 offset/len range-check overflow regression (#738) -- shared
# with tests/yocto/ since the chip driver core (chips/eeprom_24c128/) is
# OS-agnostic and the test needs no real I2C bus (see the test file's
# header comment).
add_executable(alp_test_baremetal_eeprom_24c128_range
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/eeprom_24c128_range.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../chips/eeprom_24c128/eeprom_24c128.c
)
target_link_libraries(alp_test_baremetal_eeprom_24c128_range PRIVATE alp::sdk)
target_include_directories(alp_test_baremetal_eeprom_24c128_range PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../yocto
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
)
target_compile_features(alp_test_baremetal_eeprom_24c128_range PRIVATE c_std_99)

add_test(NAME alp_test_baremetal_eeprom_24c128_range
         COMMAND alp_test_baremetal_eeprom_24c128_range)
