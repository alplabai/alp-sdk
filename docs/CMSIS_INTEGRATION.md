# ALP SDK - CMSIS Integration Guide

This guide explains how to integrate ALP SDK into CMSIS-based Alif projects (like alif_vscode-template).

## Overview

ALP SDK provides **CMSIS layers** that can be imported into any CMSIS project:

- `layers/mock/alp-mock.clayer.yml` - Mock driver (no hardware needed)
- `layers/alif/alp-alif.clayer.yml` - Alif CMSIS Driver integration

## Integration Methods

### Method 1: Import ALP SDK Layer into Existing Project

**Use case:** Add ALP SDK to your existing Alif CMSIS project

**Steps:**

1. Clone ALP SDK alongside your project:
```bash
cd C:/repos/e8
git clone https://github.com/alplabai/alp-sdk.git
```

2. Edit your `.cproject.yml`:
```yaml
# your_project.cproject.yml
project:
  groups:
    - group: App
      files:
        - file: main.c
  
  layers:
    - layer: ../device/ensemble/alif-ensemble.clayer.yml  # Alif device layer
    - layer: ../alp-sdk/layers/alif/alp-alif.clayer.yml   # ⭐ Add ALP SDK
  
  packs:
    - pack: AlifSemiconductor::Ensemble
```

3. Use ALP API in your code:
```c
#include "alp_spi_vft.h"

int main(void) {
    alp_spi_handle_t *spi = alp_spi_create_alif(0);
    // ... use ALP API
}
```

4. Build with cbuild:
```bash
cbuild your_project.cproject.yml --context .debug+E7-HE
```

### Method 2: Standalone ALP SDK Project

**Use case:** New project using only ALP SDK

**Option A: With Alif Hardware**

Use the example in `examples/alif_cmsis_example/`:

```bash
cd alp-sdk/examples/alif_cmsis_example
cbuild alp_alif_example.cproject.yml --context .debug+E7-HE
```

**Option B: With Mock Driver (No Hardware)**

Use CMake build:
```bash
cmake -B build -DBUILD_MOCK=ON -DBUILD_EXAMPLES=ON
cmake --build build
./build/bin/getting_started
```

## File Structure

```
alp-sdk/
├── alp/
│   ├── include/
│   │   └── alp_spi_vft.h          # ALP API header
│   └── src/
│       ├── core/
│       │   └── alp_spi_core.c     # VFT dispatcher
│       └── drivers/
│           ├── mock/
│           │   └── alp_spi_mock.c # Mock driver
│           └── alif/
│               └── alp_spi_alif.c # Alif wrapper
├── layers/
│   ├── mock/
│   │   └── alp-mock.clayer.yml    # ⭐ Mock layer
│   └── alif/
│       └── alp-alif.clayer.yml    # ⭐ Alif layer
└── examples/
    ├── getting_started/            # CMake example
    ├── alif_spi_example/           # CMake example
    └── alif_cmsis_example/         # ⭐ CMSIS example
```

## Layer Files Explained

### alp-mock.clayer.yml
```yaml
layer:
  groups:
    - group: ALP Core
      files:
        - file: ../../alp/src/core/alp_spi_core.c
    - group: ALP Mock Driver
      files:
        - file: ../../alp/src/drivers/mock/alp_spi_mock.c
  
  add-paths:
    - ../../alp/include
  
  define:
    - ALP_PLATFORM_MOCK=1
```

**What it does:**
- Compiles ALP core + mock driver
- No vendor SDK required
- Perfect for CI/CD testing

### alp-alif.clayer.yml
```yaml
layer:
  components:
    - component: AlifSemiconductor::CMSIS Driver:SPI  # ⭐ Pulls in Driver_SPI0
  
  groups:
    - group: ALP Core
      files:
        - file: ../../alp/src/core/alp_spi_core.c
    - group: ALP Alif Driver
      files:
        - file: ../../alp/src/drivers/alif/alp_spi_alif.c
  
  add-paths:
    - ../../alp/include
  
  define:
    - ALP_PLATFORM_ALIF=1
```

**What it does:**
- Imports Alif CMSIS SPI component
- Compiles ALP core + Alif wrapper
- Links to Driver_SPI0, Driver_SPI1, etc.

## Example: Adding ALP SDK to alif_vscode-template

Starting point: `C:/repos/e8/alif_vscode-template`

**Step 1: Clone ALP SDK**
```bash
cd C:/repos/e8
git clone https://github.com/alplabai/alp-sdk.git
```

**Step 2: Create new project using ALP**
```bash
cd alif_vscode-template
mkdir alp_demo
```

**Step 3: Create `alp_demo/alp_demo.cproject.yml`**
```yaml
# yaml-language-server: $schema=https://raw.githubusercontent.com/Open-CMSIS-Pack/devtools/tools/projmgr/2.6.0/tools/projmgr/schemas/cproject.schema.json
project:
  groups:
    - group: App
      files:
        - file: main.c
  
  layers:
    - layer: ../device/ensemble/alif-ensemble.clayer.yml
    - layer: ../../alp-sdk/layers/alif/alp-alif.clayer.yml  # ⭐ Add ALP
  
  output:
    base-name: alp_demo
    type:
      - elf
      - bin
  
  packs:
    - pack: AlifSemiconductor::Ensemble
```

**Step 4: Create `alp_demo/main.c`**
```c
#include "alp_spi_vft.h"
#include <stdio.h>

int main(void) {
    printf("ALP SDK on Alif!\n");
    
    alp_spi_handle_t *spi = alp_spi_create_alif(0);
    
    alp_spi_config_t config = {
        .instance = 0,
        .mode = ALP_SPI_MODE_MASTER,
        .baudrate = 1000000
    };
    
    alp_spi_init(spi, &config, NULL);
    
    uint8_t data[] = {0x01, 0x02, 0x03};
    alp_spi_send(spi, data, sizeof(data));
    
    alp_spi_deinit(spi);
    free(spi);
    
    return 0;
}
```

**Step 5: Add to csolution**

Edit `alif.csolution.yml`:
```yaml
solution:
  projects:
    - project: blinky/blinky.cproject.yml
    - project: hello/hello.cproject.yml
    - project: alp_demo/alp_demo.cproject.yml  # ⭐ Add this
```

**Step 6: Build**
```bash
cbuild alif.csolution.yml --context alp_demo.debug+E7-HE
```

**Step 7: Flash to hardware**
Use Alif's programming tools as usual.

## Benefits of CMSIS Layer Approach

✅ **Native Alif Workflow** - Works exactly like blinky/hello examples
✅ **RTE Configuration** - Use RTE_Device.h for pin configuration
✅ **Pack Management** - Automatic component resolution
✅ **Multi-Project** - One csolution with multiple projects
✅ **Portability** - Same API works on Mock, Alif, Renesas

## Comparison: CMake vs CMSIS

| Feature | CMake Build | CMSIS Build |
|---------|-------------|-------------|
| Alif Hardware | ✅ Supported | ✅ Supported |
| Mock Driver | ✅ Primary | ✅ Supported |
| RTE Config | ❌ Manual | ✅ Automatic |
| CI/CD | ✅ Easy | ⚠️ Complex |
| Renesas | ✅ FSP Support | ❌ Not CMSIS |
| VS Code | ✅ Tasks | ✅ CMSIS Extension |

**Recommendation:** 
- **Alif development** → Use CMSIS layers (native workflow)
- **Multi-platform/CI/CD** → Use CMake (flexibility)
- **Both available** → Choose based on team preference

## Troubleshooting

### "Cannot find alp_spi_vft.h"
**Fix:** Check layer path in `.cproject.yml`:
```yaml
layers:
  - layer: ../../alp-sdk/layers/alif/alp-alif.clayer.yml  # Adjust path
```

### "Driver_SPI0 not found"
**Fix:** Ensure SPI component is included:
```yaml
# In alp-alif.clayer.yml:
components:
  - component: AlifSemiconductor::CMSIS Driver:SPI
```

### "RTE_Device.h not found"
**Fix:** Add Alif device layer first:
```yaml
layers:
  - layer: ../device/ensemble/alif-ensemble.clayer.yml  # First
  - layer: ../../alp-sdk/layers/alif/alp-alif.clayer.yml  # Then ALP
```

## Next Steps

1. **Try the example:** `examples/alif_cmsis_example`
2. **Integrate into your project:** Follow "Method 1" above
3. **Configure pins:** Edit `RTE_Device.h` in your project
4. **Test on hardware:** Flash and run on Alif DevKit

## Support

- ALP SDK: https://github.com/alplabai/alp-sdk
- Alif Support: https://alifsemi.com/support/
- CMSIS-Toolbox: https://github.com/Open-CMSIS-Pack/devtools
