# SPDX-License-Identifier: Apache-2.0
#
# Zephyr build glue for the AWS IoT Device SDK for Embedded C (west project
# `aws-iot-device-sdk-embedded-C` @ 202412.00, group `extras-cloud`).  Builds
# coreMQTT + coreJSON only; added from zephyr/CMakeLists.txt under
# CONFIG_ALP_AWS_IOT.  Upstream has no zephyr/module.yml, so the checkout is
# located with alp_cloud_sdk_dir() -- see vendors/aws-iot/README.md.

include(${CMAKE_CURRENT_LIST_DIR}/../cloud-sdk-dir.cmake)
alp_cloud_sdk_dir(ALP_AWS_IOT_DIR aws-iot-device-sdk-embedded-C ALP_AWS_IOT_SDK_DIR)

if(NOT ALP_AWS_IOT_DIR)
    message(FATAL_ERROR
        "CONFIG_ALP_AWS_IOT=y but the AWS IoT Device SDK for Embedded C is "
        "not checked out.  Run `west update --group-filter +extras-cloud` "
        "or set ALP_AWS_IOT_SDK_DIR to its path (submodules initialised).")
endif()

# Upstream's own file lists (also define the *_INCLUDE_PUBLIC_DIRS).
include(${ALP_AWS_IOT_DIR}/libraries/standard/coreMQTT/mqttFilePaths.cmake)
include(${ALP_AWS_IOT_DIR}/libraries/standard/coreJSON/jsonFilePaths.cmake)

zephyr_library_named(alp_aws_iot)
zephyr_library_sources(${MQTT_SOURCES} ${MQTT_SERIALIZER_SOURCES} ${JSON_SOURCES})

# Public so the application sees the same headers and macro state.
zephyr_include_directories(${MQTT_INCLUDE_PUBLIC_DIRS} ${JSON_INCLUDE_PUBLIC_DIRS})

# coreMQTT otherwise #includes an application-supplied core_mqtt_config.h;
# use the upstream defaults (core_mqtt_config_defaults.h) instead.
zephyr_compile_definitions(MQTT_DO_NOT_USE_CUSTOM_CONFIG)
