# ALP SDK - Unified SDK for E1M Edge AI Modules

**Vendor-independent Hardware Abstraction Layer for E1M Edge AI Modules**

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)](https://github.com/alplabai/alp-sdk)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Alif%20%7C%20Renesas%20%7C%20Mock-orange)](https://docs.alplab.ai/e1m/overview)
[![Docs](https://img.shields.io/badge/docs-docs.alplab.ai-64CBE9)](https://docs.alplab.ai)
[![Community](https://img.shields.io/badge/community-forum-243f8b)](https://community.alplab.ai)

> Write once, run on any E1M module. The ALP SDK provides a CMSIS-compatible abstraction layer that works across Alif Ensemble, Renesas RZ/V2N, and future processors.

**[Documentation](https://docs.alplab.ai)** | **[Getting Started](https://docs.alplab.ai/sdk/quick-start)** | **[API Reference](https://docs.alplab.ai/sdk/api/gpio)** | **[Community Forum](https://community.alplab.ai)** | **[Website](https://alplab.ai)**

---

## Features

- **Mock Driver** - Unit tests run without vendor SDK or hardware
- **VFT Pattern** - Platform switching at runtime via virtual function tables
- **CI/CD Ready** - Automatic testing with GitHub Actions
- **Multi-Platform** - Alif Ensemble, Mock (Renesas upcoming)
- **Zero Overhead** - No runtime cost with production builds
- **Type-Safe API** - Modern C interface, CMSIS-Driver compatible

---

## Quick Start (30 seconds, no hardware needed)

```bash
# Clone
git clone https://github.com/alplabai/alp-sdk.git
cd alp-sdk

# Build with Mock driver
cmake -B build -DBUILD_MOCK=ON -DBUILD_EXAMPLES=ON
cmake --build build

# Run
./build/bin/getting_started
```

For detailed setup instructions, see the [Installation Guide](https://docs.alplab.ai/sdk/installation).

### Expected Output

```
[MOCK SPI0] Initialized
  - Mode: Master, Baudrate: 1000000 Hz
  - Data bits: 8, Clock mode: CPOL0_CPHA1

[MOCK SPI0] Send 8 bytes: 01 02 03 04 05 AA BB CC
[MOCK SPI0] Receive 8 bytes: 01 02 03 04 05 AA BB CC

Loopback verification: PASSED
```

---

## Supported Hardware

| Module | Processor | AI Performance | Price | Status |
|--------|-----------|---------------|-------|--------|
| [E1M-AEN](https://docs.alplab.ai/e1m/e1m-aen) | Alif Ensemble | 1024 GOPS | - | Available |
| [E1M-X V2N](https://docs.alplab.ai/e1m/e1m-x-v2n) | Renesas RZ/V2N | 4 TOPS | $89 | Available |
| [E1M-X V2N-M1](https://docs.alplab.ai/e1m/e1m-x-v2n-m1) | RZ/V2N + DeepX M1 | 25 TOPS | $179 | Available |
| [Industrial Camera](https://docs.alplab.ai/e1m/industrial-camera) | RZ/V2N + DeepX M1 | 25 TOPS | $599 | Available |

All modules use the **E1M open standard** form factor. [Compare modules](https://docs.alplab.ai/hardware/specifications).

---

## Architecture (VFT Pattern)

```
Application Code
       |
       v
+------------------+
|   ALP SDK API    |  <-- alp_spi.h, alp_gpio.h, alp_i2c.h, alp_uart.h
+------------------+
       |
       v (virtual function table)
+------+------+--------+
|      |      |        |
v      v      v        v
Mock   Alif   Renesas  Future
(Test) (E1M)  (E1M)    platforms
```

The same application code runs on all platforms. Switch targets by changing one line:

```c
// For testing (no hardware needed)
alp_spi_handle_t *spi = alp_spi_create_mock(0);

// For Alif hardware
alp_spi_handle_t *spi = alp_spi_create_alif(0);
```

---

## API Example

```c
#include "alp_spi_vft.h"

int main(void) {
    alp_spi_handle_t *spi = alp_spi_create_mock(0);

    alp_spi_config_t config = {
        .instance = 0,
        .baudrate = 1000000,
        .data_bits = 8,
        .mode = ALP_SPI_MODE_MASTER
    };
    alp_spi_init(spi, &config, NULL);

    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    alp_spi_send(spi, data, 4);

    alp_spi_deinit(spi);
    alp_spi_destroy(spi);
    return 0;
}
```

See more examples in the [SDK documentation](https://docs.alplab.ai/sdk/examples/blinky).

---

## Building for Hardware

### Alif Ensemble (E1M-AEN)

```bash
# Install CMSIS Pack
cpackget init https://www.keil.com/pack/index.pidx
cpackget add https://github.com/alifsemi/alif_ensemble-cmsis-dfp/releases/latest

# Build
cmake -B build -DBUILD_ALIF=ON -DBUILD_EXAMPLES=ON
cmake --build build
```

### Renesas RZ/V2N (Coming Soon)

```bash
cmake -B build -DBUILD_RENESAS=ON -DRENESAS_FSP_PATH=$(pwd)/vendor/renesas
cmake --build build
```

---

## Directory Structure

```
alp-sdk/
├── alp/
│   ├── include/           # Public API headers
│   └── src/
│       ├── core/          # Platform-independent dispatcher
│       └── drivers/
│           ├── mock/      # For unit testing
│           ├── alif/      # Alif Ensemble driver
│           └── renesas/   # Renesas RZ (coming)
├── examples/              # Example applications
├── tests/                 # Unit tests (mock-based)
└── vendor/                # Vendor SDKs (not in git)
```

---

## Testing

```bash
cmake -B build -DBUILD_MOCK=ON -DBUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

---

## Resources

- [Documentation](https://docs.alplab.ai) - Full SDK and hardware docs
- [API Reference](https://docs.alplab.ai/sdk/api/gpio) - GPIO, I2C, SPI, UART
- [Community Forum](https://community.alplab.ai) - Ask questions, share projects
- [Hardware Overview](https://docs.alplab.ai/e1m/overview) - E1M module comparison
- [alplab.ai](https://alplab.ai) - Product information and ordering

---

## Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

- [Report a bug](https://github.com/alplabai/alp-sdk/issues/new?template=bug_report.md)
- [Request a feature](https://github.com/alplabai/alp-sdk/issues/new?template=feature_request.md)
- [Ask on the forum](https://community.alplab.ai/c/alp-sdk/6)

---

## License

MIT License - see [LICENSE](LICENSE) for details.

---

<p align="center">
  <a href="https://alplab.ai"><strong>Alp Lab AB</strong></a> - Edge AI Modules for Everyone
</p>
