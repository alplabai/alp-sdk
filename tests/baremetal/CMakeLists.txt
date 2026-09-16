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

# eeprom_24c128_read_identity() unit checks (N24S128 second device-select
# header) -- shared with tests/yocto/ for the same reason as
# alp_test_baremetal_eeprom_24c128_range above.
add_executable(alp_test_baremetal_eeprom_24c128_read_identity
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/eeprom_24c128_read_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../chips/eeprom_24c128/eeprom_24c128.c
)
target_link_libraries(alp_test_baremetal_eeprom_24c128_read_identity PRIVATE alp::sdk)
target_include_directories(alp_test_baremetal_eeprom_24c128_read_identity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../yocto
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
)
target_compile_features(alp_test_baremetal_eeprom_24c128_read_identity PRIVATE c_std_99)

add_test(NAME alp_test_baremetal_eeprom_24c128_read_identity
         COMMAND alp_test_baremetal_eeprom_24c128_read_identity)

# eeprom_24c128_secure_page_write() / eeprom_24c128_secure_page_lock() unit
# checks -- shared with tests/yocto/ for the same reason as
# alp_test_baremetal_eeprom_24c128_range above. Deliberately links
# alp_sdk_headers (include paths only), NOT alp::sdk (the real backend) --
# this test file supplies its OWN recording-fake alp_i2c_write()/
# alp_i2c_write_read()/alp_delay_us() so the exact wire bytes are
# assertable; linking the real backend too would duplicate-define those
# three symbols.
add_executable(alp_test_baremetal_eeprom_24c128_secure_page
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/eeprom_24c128_secure_page.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../chips/eeprom_24c128/eeprom_24c128.c
)
target_link_libraries(alp_test_baremetal_eeprom_24c128_secure_page PRIVATE alp_sdk_headers)
target_include_directories(alp_test_baremetal_eeprom_24c128_secure_page PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../yocto
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
)
target_compile_features(alp_test_baremetal_eeprom_24c128_secure_page PRIVATE c_std_99)

add_test(NAME alp_test_baremetal_eeprom_24c128_secure_page
         COMMAND alp_test_baremetal_eeprom_24c128_secure_page)

# alp_secure_page_mirror_t unit checks (struct size/layout, CRC round-trip,
# blank-page detection) -- pure header logic, no chip driver source needed.
add_executable(alp_test_baremetal_alp_secure_page_mirror
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/alp_secure_page_mirror.c
)
target_link_libraries(alp_test_baremetal_alp_secure_page_mirror PRIVATE alp::sdk)
target_include_directories(alp_test_baremetal_alp_secure_page_mirror PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../yocto
)
target_compile_features(alp_test_baremetal_alp_secure_page_mirror PRIVATE c_std_99)

add_test(NAME alp_test_baremetal_alp_secure_page_mirror
         COMMAND alp_test_baremetal_alp_secure_page_mirror)

# examples/aen/aen-eeprom-provision mode 3 (ALP_LOCK_SECURE_PAGE) lock-time
# gates -- shared with tests/yocto/ for the same reason as
# alp_test_baremetal_alp_secure_page_mirror above.
add_executable(alp_test_baremetal_aen_eeprom_provision_lock_gates
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/aen_eeprom_provision_lock_gates.c
)
target_link_libraries(alp_test_baremetal_aen_eeprom_provision_lock_gates PRIVATE alp::sdk)
target_include_directories(alp_test_baremetal_aen_eeprom_provision_lock_gates PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../yocto
)
target_compile_features(alp_test_baremetal_aen_eeprom_provision_lock_gates PRIVATE c_std_99)

add_test(NAME alp_test_baremetal_aen_eeprom_provision_lock_gates
         COMMAND alp_test_baremetal_aen_eeprom_provision_lock_gates)
