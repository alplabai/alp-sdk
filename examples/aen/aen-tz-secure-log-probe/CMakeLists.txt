cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_tz_log_probe)
target_sources(app PRIVATE src/main.c)
# CMSE intrinsics (cmse_nonsecure_call / BLXNS) for the single-core NS transition.
target_compile_options(app PRIVATE -mcmse)
