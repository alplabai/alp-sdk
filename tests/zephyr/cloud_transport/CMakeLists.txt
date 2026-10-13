# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_cloud_transport_test LANGUAGES C)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_ALP_AWS_IOT app PRIVATE src/coremqtt.c)

if(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
  target_sources(app PRIVATE src/tls.c)

  # Zephyr's own test PKI (samples/net/sockets/echo_server): a CA, a
  # CN=localhost server certificate it signed, that server's key, and an
  # unrelated self-signed certificate used here as the WRONG trust anchor.
  # Taken from the Zephyr tree at build time so no key material lives in this
  # repository.
  set(_certs ${ZEPHYR_BASE}/samples/net/sockets/echo_server/src)
  set(_gen ${ZEPHYR_BINARY_DIR}/include/generated)
  foreach(_f ca.der server.der server_privkey.der echo-apps-cert.der)
    generate_inc_file_for_target(app ${_certs}/${_f} ${_gen}/${_f}.inc)
  endforeach()
endif()
