# SPDX-License-Identifier: Apache-2.0
#
# Zephyr build glue for the Azure SDK for Embedded C (west project
# `azure-sdk-for-c` @ 1.5.0, group `extras-cloud`).  Builds az_core (span /
# json / base64 / context / log, no HTTP pipeline) + the az_iot hub client;
# added from zephyr/CMakeLists.txt under CONFIG_ALP_AZURE_IOT.  Upstream has
# no zephyr/module.yml, so the checkout is located with alp_cloud_sdk_dir()
# -- see vendors/azure-iot/README.md.

include(${CMAKE_CURRENT_LIST_DIR}/../cloud-sdk-dir.cmake)
alp_cloud_sdk_dir(ALP_AZURE_IOT_DIR azure-sdk-for-c ALP_AZURE_IOT_SDK_DIR)

if(NOT ALP_AZURE_IOT_DIR)
    message(FATAL_ERROR
        "CONFIG_ALP_AZURE_IOT=y but the Azure SDK for Embedded C is not "
        "checked out.  Run `west update --group-filter +extras-cloud` or "
        "set ALP_AZURE_IOT_SDK_DIR to its path.")
endif()

set(_core ${ALP_AZURE_IOT_DIR}/sdk/src/azure/core)
set(_iot  ${ALP_AZURE_IOT_DIR}/sdk/src/azure/iot)

zephyr_library_named(alp_azure_iot)
zephyr_library_sources(
    ${_core}/az_base64.c
    ${_core}/az_context.c
    ${_core}/az_json_reader.c
    ${_core}/az_json_token.c
    ${_core}/az_json_writer.c
    ${_core}/az_log.c
    ${_core}/az_precondition.c
    ${_core}/az_span.c
    # No OS abstraction ships for Zephyr; the no-op platform layer satisfies
    # the az_platform_* symbols az_core references (clock/sleep are unused by
    # the hub client, which only serialises topics and payloads).
    ${ALP_AZURE_IOT_DIR}/sdk/src/azure/platform/az_noplatform.c
    ${_iot}/az_iot_common.c
    ${_iot}/az_iot_hub_client.c
    ${_iot}/az_iot_hub_client_sas.c
    ${_iot}/az_iot_hub_client_telemetry.c
    ${_iot}/az_iot_hub_client_c2d.c
    ${_iot}/az_iot_hub_client_twin.c
    ${_iot}/az_iot_hub_client_methods.c
    ${_iot}/az_iot_hub_client_commands.c
    ${_iot}/az_iot_hub_client_properties.c
)

zephyr_include_directories(${ALP_AZURE_IOT_DIR}/sdk/inc)
