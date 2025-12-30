# 🚀 ALP SDK - Unified SDK for E1M

**Platform-independent Hardware Abstraction Layer**

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![License](https://img.shields.io/badge/license-MIT-blue)]()
[![Platform](https://img.shields.io/badge/platform-Alif%20%7C%20Mock-orange)]()

---

## ✨ Features

- ✅ **Mock Driver** - Unit tests run without vendor SDK
- ✅ **VFT Pattern** - Platform switching at runtime
- ✅ **CI/CD Ready** - Automatic testing with GitHub Actions
- ✅ **Multi-Platform** - Alif, Mock (Renesas upcoming)
- ✅ **Zero Overhead** (with production build)
- ✅ **Type-Safe API** - Modern C interface

---

## 🚀 Workflow

### 1️⃣ Clone Repository
```bash
git clone https://github.com/alplabai/alp-sdk.git
cd alp-sdk
```

### 2️⃣ Download Vendor SDK (required for hardware deployment)

**⚠️ Note:** For deploying to **E1M board** or **Alif hardware**, the vendor SDK is required.

We recommend installing the official CMSIS Pack instead of using the download scripts:

**Option A: Install CMSIS Pack (Recommended)**
```bash
cpackget init https://www.keil.com/pack/index.pidx
cpackget add https://github.com/alifsemi/alif_ensemble-cmsis-dfp/releases/latest
```

**Option B: Download Basic Headers (Limited Support)**
```bash
# Windows
scripts\download_alif_sdk.bat

# PowerShell (Windows/Linux)
pwsh scripts/download_alif_sdk.ps1

# Linux/macOS
bash scripts/download_alif_sdk.sh
```

> **Note:** The download scripts provide basic CMSIS headers for code completion only. Some files may not be available via direct download - this is normal. For full Alif support with hardware drivers, use the CMSIS Pack (Option A).

### 3️⃣ Open in VS Code
```bash
code .
```

**VS Code will automatically prompt you to install recommended extensions:**
- C/C++ Extension Pack (IntelliSense, debugging)
- CMake Tools (build system)
- CMSIS-Toolbox (Alif CMSIS projects)
- Cortex-Debug (hardware debugging)

Click **"Install All"** when prompted.

### 4️⃣ Build and Run

**Option A: Use VS Code Tasks (Recommended)**
- Press `Ctrl+Shift+B` → Select "Full Build and Run (Mock)"
- Or: `Ctrl+Shift+P` → "Tasks: Run Task" → Select task

**Option B: Use Terminal**
```bash
# Mock Driver (for unit tests and development)
cmake -B build -DBUILD_MOCK=ON -DBUILD_EXAMPLES=ON
cmake --build build
./build/bin/getting_started

# Alif Driver (for hardware deployment)
cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=vendor/alif/sdk -DBUILD_EXAMPLES=ON
cmake --build build
# Flash to Alif E7 hardware and run
```

---

## 🎯 Quick Start (Mock Driver - 30 seconds!)

**Test the API without hardware - unit tests run without vendor SDK!**

```bash
# 1. Clone
git clone https://github.com/alplabai/alp-sdk.git
cd alp-sdk

# 2. Open in VS Code (install extensions when prompted)
code .

# 3. Press Ctrl+Shift+B → Select "Full Build and Run (Mock)"
```

**Or using terminal:**
```bash
cmake -B build -DBUILD_MOCK=ON -DBUILD_EXAMPLES=ON
cmake --build build
./build/bin/getting_started
```

### 🎉 Expected Output:

```
╔════════════════════════════════════════════════════════════╗
║         ALP SDK - Getting Started Example                 ║
║         Architecture: VFT Pattern + Mock Driver            ║
║                                                            ║
║  🎯 This example uses Mock driver for testing!            ║
╚════════════════════════════════════════════════════════════╝

[MOCK SPI0] 🎉 Created (Mock Driver for Testing)
  - Platform: mock
  - Hardware: Simulated

[MOCK SPI0] ✅ Initialized
  - Mode: Master
  - Baudrate: 1000000 Hz
  - Data bits: 8
  - Clock mode: CPOL0_CPHA1
  - Bit order: MSB first

[MOCK SPI0] 📤 Send 8 bytes: 01 02 03 04 05 AA BB CC
[MOCK SPI0] 📥 Receive 8 bytes: 01 02 03 04 05 AA BB CC

✅ Loopback verification: PASSED

╔════════════════════════════════════════════════════════════╗
║                      ✅ SUCCESS!                           ║
║                                                            ║
║  The ALP SDK API works with Mock driver for testing!      ║
╚════════════════════════════════════════════════════════════╝
```

---

## 🔌 ALP SDK Examples for Alif Hardware

### Prerequisites
- CMSIS-Toolbox installed (`cpackget`, `cbuild`)
- Alif Ensemble Pack installed (see Workflow section)
- **Alif E7 DevKit** hardware (E1M Custom Board also supported)

### Board Switching Architecture

All examples are configured for **E1M Custom Board** by default but can also deploy to:
- **E1M Custom Board** (default, `BOARD_ALIF_DEVKIT_VARIANT=6`)
- **Alif E7 DevKit Gen2** (`BOARD_ALIF_DEVKIT_VARIANT=4`)

To switch to E7 DevKit, edit [alp-sdk.csolution.yml](alp-sdk.csolution.yml#L13):
```yaml
define:
  - BOARD_ALIF_DEVKIT_VARIANT: 4  # Change from 6 to 4 for E7 DevKit Gen2
```

### Available Examples

#### 1. Blinky (LED Toggle) - **Uses ALP SDK**
**Target:** Alif E7 DevKit (also works on E1M)  
**Description:** Toggles RGB LED using **ALP GPIO abstraction layer**  
**Build:**
```bash
cbuild alp-sdk.csolution.yml --context blinky_alp.Debug+Alif-E7-DevKit
```
**Features:**
- Uses `alp_gpio_create_alif()` for platform abstraction
- Demonstrates ALP SDK GPIO API: `alp_gpio_init()`, `alp_gpio_write()`, `alp_gpio_toggle()`
- Blue LED toggles on M55_HE core
- Layer-based device configuration

**Output:** `.bin` and `.axf` files in `out/blinky_alp/Alif-E7-DevKit/Debug/`

#### 2. Hello World (Alternating LEDs) - **Uses ALP SDK**
**Target:** Alif E7 DevKit (also works on E1M)  
**Description:** Alternates Red/Green LEDs using **ALP GPIO abstraction layer**  
**Build:**
```bash
cbuild alp-sdk.csolution.yml --context hello_world_alp.Debug+Alif-E7-DevKit
```
**Features:**
- Uses `alp_gpio_create_alif()` for multi-LED control
- Demonstrates coordinated GPIO toggling
- Red and Green LEDs alternate at 5Hz
- Board-portable with variant define

#### 3. SPI Demo (Full Stack) - **Uses ALP SDK**
**Target:** Alif E7 DevKit (also works on E1M)  
**Description:** Demonstrates SPI, GPIO, and USART with **ALP SDK abstraction**  
**Build:**
```bash
cbuild alp-sdk.csolution.yml --context alp_spi_demo.Debug+Alif-E7-DevKit
```
**Features:**
- Uses `alp_spi_create_alif()`, `alp_gpio_create_alif()`, `alp_usart_create_alif()`
- Full VFT pattern demonstration
- Platform-portable code
- Shared device layer configuration

### Flashing to Hardware
```bash
# Using J-Link
JLinkExe -device <device_name> -if SWD -speed 4000 -autoconnect 1
J-Link> loadbin <path_to_bin> <load_address>
J-Link> g  # Go (run)
```

---

## 📚 API Example

### Mock Driver Example (For Unit Testing)

```c
#include "alp_spi_vft.h"

int main(void) {
    // 1. Create Mock SPI handle (for testing)
    alp_spi_handle_t *spi = alp_spi_create_mock(0);
    
    // 2. Configure
    alp_spi_config_t config = {
        .instance = 0,
        .baudrate = 1000000,
        .data_bits = 8,
        .mode = ALP_SPI_MODE_MASTER
    };
    alp_spi_init(spi, &config, NULL);
    
    // 3. Send data
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    alp_spi_send(spi, data, 4);
    
    // 4. Cleanup
    alp_spi_deinit(spi);
    alp_spi_destroy(spi);
    
    return 0;
}
```

---

## 🔧 Usage with Vendor SDK

### Alif Ensemble

**Recommended: Use CMSIS Pack**
```bash
# 1. Install CMSIS Pack
cpackget init https://www.keil.com/pack/index.pidx
cpackget add https://github.com/alifsemi/alif_ensemble-cmsis-dfp/releases/latest

# 2. Build (CMake auto-detects the pack)
cmake -B build -DBUILD_ALIF=ON
cmake --build build

# 3. Use Alif driver
alp_spi_handle_t *spi = alp_spi_create_alif(0);  // Real hardware!
```

**Alternative: Manual SDK Path**
```bash
# If you have the SDK in a custom location
cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=/path/to/alif/sdk
cmake --build build
```

### Renesas (Coming Soon)

```bash
# 1. Download Renesas FSP
./scripts/download_vendor_renesas.sh

# 2. Build
cmake -B build -DBUILD_RENESAS=ON -DRENESAS_FSP_PATH=$(pwd)/vendor/renesas
cmake --build build
```

---

## 🏗️ Architecture

### VFT (Virtual Function Table) Pattern

```
┌─────────────────────────────────────────┐
│  alp_spi_handle_t                       │
│  ┌───────────────────────────────────┐  │
│  │ ops (function pointer table)      │──┼──┐
│  │ hw_handle (platform data)         │  │  │
│  │ config                            │  │  │
│  └───────────────────────────────────┘  │  │
└─────────────────────────────────────────┘  │
                                             │
                   ┌─────────────────────────┘
                   │
       ┌───────────▼──────────┐
       │  ops->send(...)      │ (Virtual call)
       └───────────┬──────────┘
                   │
    ┌──────────────┼──────────────┐
    │              │              │
┌───▼────┐  ┌─────▼──────┐  ┌───▼──────┐
│ mock   │  │ alif       │  │ renesas  │
│ (Test) │  │ (CMSIS)    │  │ (FSP)    │
│        │  │            │  │ (coming) │
└────────┘  └────────────┘  └──────────┘
```

### Directory Structure

```
alp-sdk/
├── alp/
│   ├── include/           # Public API
│   │   └── alp_spi_vft.h
│   └── src/
│       ├── core/          # Platform-independent dispatcher
│       │   └── alp_spi_core.c
│       └── drivers/
│           ├── mock/      # 🧪 For unit testing
│           ├── alif/      # Alif Ensemble
│           └── renesas/   # Renesas RA
│
├── vendor/                # Vendor SDKs (not in git)
│   ├── alif/
│   └── renesas/
│
├── examples/
│   └── getting_started/   # Mock example
│
└── tests/
    └── mock/              # Unit tests
```

---

## 🧪 Testing

**Unit tests can run without vendor SDK using the Mock driver:**

```bash
# Build with tests
cmake -B build -DBUILD_MOCK=ON -DBUILD_TESTS=ON
cmake --build build

# Run tests
cd build
ctest --output-on-failure
```

---

## 📄 License

MIT License - see [LICENSE](LICENSE) file for details

---

## 📧 Contact

- **GitHub:** [alplabai/alp-sdk](https://github.com/alplabai/alp-sdk)
- **Issues:** [GitHub Issues](https://github.com/alplabai/alp-sdk/issues)
- **Email:** [contact@alplab.ai](mailto:contact@alplab.ai)

---

<p align="center">
  <strong>Made with ❤️ by ALP Lab AI Team</strong>
</p>

<p align="center">
  ⭐ Star us on GitHub if you find this useful!
</p>
