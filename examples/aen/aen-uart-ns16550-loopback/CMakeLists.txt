# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_uart_ns16550_loopback)

target_sources(app PRIVATE src/main.c)
