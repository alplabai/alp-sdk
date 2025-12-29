# ALP SDK Scripts

This directory contains utility scripts for setting up the ALP SDK development environment.

## Available Scripts

### 1. Alif SDK Downloader

Downloads required Alif Ensemble SDK files for building and running ALP SDK with Alif hardware support.

**Windows (Batch):**
```cmd
scripts\download_alif_sdk.bat
```

**Windows/Linux (PowerShell):**
```powershell
pwsh scripts/download_alif_sdk.ps1
```

**Linux/macOS (Bash):**
```bash
bash scripts/download_alif_sdk.sh
```

### What Gets Downloaded

- **CMSIS Driver Headers**: Standard ARM CMSIS driver interfaces
  - Driver_SPI.h
  - Driver_USART.h
  - Driver_I2C.h
  - Driver_GPIO.h
  - Driver_Common.h

- **Alif Board Configuration**: DevKit E7 board definitions
  - board.h

- **Alif Device Headers**: M55 core system headers
  - system_M55_HE.h
  - system_M55_HP.h

- **Documentation**: Setup and build instructions
  - README.md

### Download Location

All files are downloaded to: `vendor/alif/sdk/`

### Important Notes

⚠️ **The downloaded files are MINIMAL headers for compilation only.**

For actual hardware execution, you must install the full Alif Ensemble SDK:

1. **Official Website**: https://alifsemi.com/support/software-tools/ensemble/
2. **CMSIS Pack**: Use `cpackget` to install the complete DFP pack
3. **GitHub**: Clone the full repository

## Typical Workflow

### Step 1: Clone Repository
```bash
git clone https://github.com/alplabai/alp-sdk.git
cd alp-sdk
```

### Step 2: Download Vendor SDK (optional for Mock)
```bash
# Windows
scripts\download_alif_sdk.bat

# PowerShell
pwsh scripts/download_alif_sdk.ps1

# Linux/macOS
bash scripts/download_alif_sdk.sh
```

### Step 3: Open in VS Code
```bash
code .
```

### Step 4: Build

**Mock Driver (no vendor SDK needed):**
```bash
cmake -B build -DBUILD_MOCK=ON -DBUILD_EXAMPLES=ON
cmake --build build
./build/bin/getting_started
```

**Alif Driver (requires downloaded SDK):**
```bash
cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=vendor/alif/sdk
cmake --build build
# Flash to Alif E7 hardware and run
```

## Future Scripts

Coming soon:
- `download_renesas_fsp.ps1` - Renesas FSP downloader
- `setup_vscode.ps1` - VS Code configuration setup
- `run_tests.ps1` - Automated testing
- `build_all.ps1` - Build all platform configurations

## Troubleshooting

### Download Failed
- Check internet connection
- Verify GitHub is accessible
- Try using VPN if rate-limited
- Download full SDK manually from vendor website

### Permission Denied (Linux/macOS)
```bash
chmod +x scripts/*.sh
```

### PowerShell Execution Policy (Windows)
```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

## Contributing

To add a new download script:
1. Follow the existing naming convention: `download_<vendor>_sdk.<ext>`
2. Include error handling and retry logic
3. Create README.md in the download destination
4. Update this file with usage instructions
5. Test on Windows, Linux, and macOS if applicable

## Support

- ALP SDK Issues: https://github.com/alplabai/alp-sdk/issues
- Alif Support: https://alifsemi.com/support/
- Renesas Support: https://www.renesas.com/support
