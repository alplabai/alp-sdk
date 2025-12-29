#!/bin/bash
# ============================================================================
# Alif Ensemble SDK Downloader (Bash)
# ============================================================================
# This script downloads the required Alif Ensemble SDK files for ALP SDK
# 
# Requirements:
#   - curl or wget
#   - Internet connection
#
# Usage:
#   bash scripts/download_alif_sdk.sh
#
# ============================================================================

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Configuration
DEST_DIR="vendor/alif/sdk"
ALIF_REPO="https://raw.githubusercontent.com/alifsemi/alif_ensemble-cmsis-dfp/main"
CMSIS_REPO="https://raw.githubusercontent.com/ARM-software/CMSIS_5/develop"

# Check for download tool
if command -v curl &> /dev/null; then
    DOWNLOAD_CMD="curl -fsSL -o"
elif command -v wget &> /dev/null; then
    DOWNLOAD_CMD="wget -q -O"
else
    echo -e "${RED}Error: Neither curl nor wget found. Please install one of them.${NC}"
    exit 1
fi

# Download function
download_file() {
    local url=$1
    local output=$2
    local description=$3
    
    echo -n "  Downloading $description..."
    
    if [ -f "$output" ]; then
        echo -e " ${YELLOW}[SKIP - exists]${NC}"
        return 0
    fi
    
    if $DOWNLOAD_CMD "$output" "$url" 2>/dev/null; then
        echo -e " ${GREEN}[OK]${NC}"
        return 0
    else
        echo -e " ${RED}[FAILED]${NC}"
        return 1
    fi
}

# Header
echo ""
echo -e "${CYAN}============================================================================${NC}"
echo -e "${CYAN}  Alif Ensemble SDK Downloader for ALP SDK${NC}"
echo -e "${CYAN}============================================================================${NC}"
echo ""

# Create directories
echo "[1/6] Creating directory structure..."
mkdir -p "$DEST_DIR/drivers"
mkdir -p "$DEST_DIR/CMSIS"
mkdir -p "$DEST_DIR/include"
echo -e "  ${GREEN}Created: $DEST_DIR${NC}"
echo ""

# Download CMSIS headers
echo "[2/6] Downloading CMSIS Driver headers..."
CMSIS_SUCCESS=0
CMSIS_TOTAL=5

declare -A CMSIS_FILES=(
    ["Driver_Common.h"]="$CMSIS_REPO/CMSIS/Driver/Include/Driver_Common.h"
    ["Driver_SPI.h"]="$CMSIS_REPO/CMSIS/Driver/Include/Driver_SPI.h"
    ["Driver_USART.h"]="$CMSIS_REPO/CMSIS/Driver/Include/Driver_USART.h"
    ["Driver_I2C.h"]="$CMSIS_REPO/CMSIS/Driver/Include/Driver_I2C.h"
    ["Driver_GPIO.h"]="$CMSIS_REPO/CMSIS/Driver/Include/Driver_GPIO.h"
)

for file in "${!CMSIS_FILES[@]}"; do
    if download_file "${CMSIS_FILES[$file]}" "$DEST_DIR/CMSIS/$file" "$file"; then
        ((CMSIS_SUCCESS++))
    fi
done
echo ""

# Download board config
echo "[3/6] Downloading Alif board configuration..."
BOARD_SUCCESS=0
BOARD_TOTAL=1

if download_file "$ALIF_REPO/Boards/DevKit-e7/board.h" "$DEST_DIR/board.h" "board.h"; then
    ((BOARD_SUCCESS++))
fi
echo ""

# Download device headers
echo "[4/6] Downloading Alif device headers..."
DEVICE_SUCCESS=0
DEVICE_TOTAL=2

declare -A DEVICE_FILES=(
    ["system_M55_HE.h"]="$ALIF_REPO/Device/M55_HE/Include/system_M55_HE.h"
    ["system_M55_HP.h"]="$ALIF_REPO/Device/M55_HP/Include/system_M55_HP.h"
)

for file in "${!DEVICE_FILES[@]}"; do
    if download_file "${DEVICE_FILES[$file]}" "$DEST_DIR/include/$file" "$file"; then
        ((DEVICE_SUCCESS++))
    fi
done
echo ""

# Create driver stub
echo "[5/6] Creating driver stubs..."
cat > "$DEST_DIR/drivers/Driver_SPI_Alif.h" << 'EOF'
/**
 * Alif SPI Driver Stub
 * 
 * This is a placeholder. For full functionality, download the complete
 * Alif Ensemble SDK from: https://alifsemi.com/support/software-tools/ensemble/
 * 
 * The actual driver implementation is provided in the Alif CMSIS-DFP pack.
 */

#ifndef DRIVER_SPI_ALIF_H
#define DRIVER_SPI_ALIF_H

#include "Driver_SPI.h"

// External driver instances (provided by Alif SDK)
extern ARM_DRIVER_SPI Driver_SPI0;
extern ARM_DRIVER_SPI Driver_SPI1;

#endif // DRIVER_SPI_ALIF_H
EOF
echo -e "  ${GREEN}Created driver stub: Driver_SPI_Alif.h${NC}"
echo ""

# Create README
echo "[6/6] Creating documentation..."
cat > "$DEST_DIR/README.md" << EOF
# Alif Ensemble SDK

Downloaded from: https://github.com/alifsemi/alif_ensemble-cmsis-dfp
Date: $(date "+%Y-%m-%d %H:%M:%S")

## Contents

- CMSIS Driver headers (SPI, USART, I2C, GPIO)
- Alif board configuration
- Device headers
- Driver stubs

## Downloaded Files

CMSIS Headers: $CMSIS_SUCCESS / $CMSIS_TOTAL
Board Config: $BOARD_SUCCESS / $BOARD_TOTAL
Device Headers: $DEVICE_SUCCESS / $DEVICE_TOTAL

## Full SDK Installation

For production use, download the complete Alif Ensemble SDK:

### Option 1: Official Website
1. Visit: https://alifsemi.com/support/software-tools/ensemble/
2. Download the full SDK package
3. Extract to: vendor/alif/sdk
4. Update ALIF_SDK_PATH in CMake

### Option 2: CMSIS Pack Manager
\`\`\`bash
cpackget init https://www.keil.com/pack/index.pidx
cpackget add https://github.com/alifsemi/alif_ensemble-cmsis-dfp/releases/download/v1.3.0/AlifSemiconductor.Ensemble.1.3.0.pack
\`\`\`

### Option 3: Git Clone
\`\`\`bash
git clone https://github.com/alifsemi/alif_ensemble-cmsis-dfp.git vendor/alif/cmsis-dfp
\`\`\`

## Build with ALP SDK

\`\`\`bash
# Configure with Alif support
cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=vendor/alif/sdk

# Build
cmake --build build

# Run example (requires Alif E7 hardware)
./build/bin/alif_spi_example
\`\`\`

## Important Notes

⚠️ The files downloaded by this script are MINIMAL headers for compilation.
⚠️ For actual hardware execution, you MUST install the full Alif SDK.
⚠️ The full SDK includes:
   - Complete driver implementations
   - Linker scripts
   - Startup code
   - Device configuration
   - Flash programming tools

## Support

- Alif Website: https://alifsemi.com
- Documentation: https://alifsemi.com/support/
- GitHub: https://github.com/alifsemi
- Forum: https://community.alifsemi.com

EOF
echo -e "  ${GREEN}Created: README.md${NC}"
echo ""

# Summary
TOTAL_FILES=$((CMSIS_TOTAL + BOARD_TOTAL + DEVICE_TOTAL))
TOTAL_SUCCESS=$((CMSIS_SUCCESS + BOARD_SUCCESS + DEVICE_SUCCESS))

echo -e "${CYAN}============================================================================${NC}"
echo -e "${CYAN}  Download Summary${NC}"
echo -e "${CYAN}============================================================================${NC}"
echo ""
echo "  CMSIS Headers:    $CMSIS_SUCCESS / $CMSIS_TOTAL"
echo "  Board Config:     $BOARD_SUCCESS / $BOARD_TOTAL"
echo "  Device Headers:   $DEVICE_SUCCESS / $DEVICE_TOTAL"
echo "  ---"
echo "  Total:            $TOTAL_SUCCESS / $TOTAL_FILES files"
echo ""
echo -e "  Location: ${CYAN}$DEST_DIR${NC}"
echo ""

if [ $TOTAL_SUCCESS -eq $TOTAL_FILES ]; then
    echo -e "${GREEN}============================================================================${NC}"
    echo -e "${GREEN}  SUCCESS! Alif SDK files downloaded${NC}"
    echo -e "${GREEN}============================================================================${NC}"
    echo ""
    echo "  Next steps:"
    echo "  1. Open project in VS Code"
    echo "  2. Build: cmake -B build -DBUILD_ALIF=ON -DALIF_SDK_PATH=$DEST_DIR"
    echo "  3. Compile: cmake --build build"
    echo ""
    echo -e "  ${YELLOW}Note: For actual hardware, download full SDK from:${NC}"
    echo -e "  ${YELLOW}      https://alifsemi.com/support/software-tools/ensemble/${NC}"
    echo ""
else
    echo -e "${YELLOW}============================================================================${NC}"
    echo -e "${YELLOW}  PARTIAL SUCCESS - Some files could not be downloaded${NC}"
    echo -e "${YELLOW}============================================================================${NC}"
    echo ""
    echo "  This may be due to:"
    echo "  - Internet connection issues"
    echo "  - Changed repository structure"
    echo "  - GitHub API rate limiting"
    echo ""
    echo "  For full SDK, visit:"
    echo -e "  ${CYAN}https://alifsemi.com/support/software-tools/ensemble/${NC}"
    echo ""
fi

echo -e "${CYAN}============================================================================${NC}"
echo ""
