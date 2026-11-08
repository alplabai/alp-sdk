### Fixed — testing,pid,protocol: clock callback timestamp, stale GPIO handles, PID default integ limit, C++ static_assert (#2934, #2935, #2981)

alp/testing doubles for adc, can, i2c, spi, storage and uart now reject handles left open across alp_testing_reset_all() with ALP_ERR_NOT_READY; the PID default integrator limit is span/(2*|ki|) and also covers negative ki; public protocol and hw_info headers compile as C++ through the new ALP_STATIC_ASSERT in <alp/static_assert.h>, enforced by check_public_header_purity.py --cxx. (CHANGELOG.md not edited.)
