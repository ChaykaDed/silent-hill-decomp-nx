#!/bin/bash
# Build script for Nintendo Switch port
# Usage: ./build_switch.sh [configure|build|clean|rebuild]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLCHAIN="${SCRIPT_DIR}/toolchains/switch.cmake"
BUILD_DIR="${SCRIPT_DIR}/build_switch"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=== Silent Hill Switch Build ===${NC}"

# Check prerequisites
if [ ! -f "$TOOLCHAIN" ]; then
    echo -e "${RED}Error: Toolchain not found at $TOOLCHAIN${NC}"
    exit 1
fi

if [ -z "$DEVKITPRO" ]; then
    echo -e "${RED}Error: DEVKITPRO not set${NC}"
    echo "Please install devkitPro and set the DEVKITPRO environment variable"
    exit 1
fi

echo -e "${GREEN}✓ DEVKITPRO: $DEVKITPRO${NC}"

# Verify compilers exist
if [ ! -f "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" ]; then
    echo -e "${RED}Error: aarch64-none-elf-gcc not found${NC}"
    echo "Run: dkp-pacman -S switch-gcc"
    exit 1
fi

echo -e "${GREEN}✓ Compiler found${NC}"

# Handle arguments
MODE="${1:-build}"

case "$MODE" in
    configure)
        echo -e "${YELLOW}Configuring for Nintendo Switch...${NC}"
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake .. \
            -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
            -DCMAKE_BUILD_TYPE=Release \
            -G Ninja
        echo -e "${GREEN}✓ Configuration complete${NC}"
        ;;
    
    build)
        if [ ! -d "$BUILD_DIR" ]; then
            echo -e "${YELLOW}Build directory not found, configuring first...${NC}"
            mkdir -p "$BUILD_DIR"
            cd "$BUILD_DIR"
            cmake .. \
                -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
                -DCMAKE_BUILD_TYPE=Release \
                -G Ninja
        fi
        echo -e "${YELLOW}Building for Nintendo Switch...${NC}"
        cd "$BUILD_DIR"
        cmake --build . --config Release
        echo -e "${GREEN}✓ Build complete!${NC}"
        # Embed icon if available
        if [ -f "${SCRIPT_DIR}/SH1icon.png" ]; then
            elf2nro SilentHillPC SilentHillPC.nro --nacp=SilentHillPC.nacp
            python3 "${SCRIPT_DIR}/maker.py" --nro "${BUILD_DIR}/SilentHillPC.nro" --icon "${SCRIPT_DIR}/SH1icon.png" --author "ChaykaDed"
        fi
        echo -e "${GREEN}Output: ${BUILD_DIR}${NC}"
        ;;


    clean)
        echo -e "${YELLOW}Cleaning build directory...${NC}"
        rm -rf "$BUILD_DIR"
        echo -e "${GREEN}✓ Clean complete${NC}"
        ;;
    
    rebuild)
        echo -e "${YELLOW}Full rebuild...${NC}"
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake .. \
            -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
            -DCMAKE_BUILD_TYPE=Release \
            -G Ninja
        cmake --build . --config Release
        echo -e "${GREEN}✓ Rebuild complete!${NC}"
        # Embed icon if available
        if [ -f "${SCRIPT_DIR}/SH1icon.png" ]; then
            elf2nro SilentHillPC SilentHillPC.nro --nacp=SilentHillPC.nacp
            python3 "${SCRIPT_DIR}/maker.py" --nro "${BUILD_DIR}/SilentHillPC.nro" --icon "${SCRIPT_DIR}/SH1icon.png" --author "ChaykaDed"
        fi
        echo -e "${GREEN}Output: ${BUILD_DIR}${NC}"
        ;;
    
    *)
        echo "Usage: $0 [configure|build|clean|rebuild]"
        echo "  configure - Configure CMake (one-time setup)"
        echo "  build     - Build (configure if needed)"
        echo "  clean     - Remove build directory"
        echo "  rebuild   - Clean and build from scratch"
        exit 1
        ;;
esac
