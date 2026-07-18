#!/bin/bash
# Test build script for AllyTouchI2cDxe (driver + probe).
# Mirrors .github/workflows/build.yml; run under Linux / WSL Ubuntu.

set -e
set -u

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

BUILD_DIR="build_test"
EDK2_BRANCH="edk2-stable202411"
ARCH="X64"
TOOLCHAIN="GCC5"
BUILD_TYPE="DEBUG"   # DEBUG for testing; change to RELEASE if needed

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}AllyTouchI2cDxe Test Build Script${NC}"
echo -e "${GREEN}========================================${NC}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "${YELLOW}[1/8] Checking dependencies...${NC}"
for tool in git make gcc nasm iasl python3; do
    if ! command -v $tool &> /dev/null; then
        echo -e "${RED}Error: $tool is not installed${NC}"
        echo "Please install: sudo apt install build-essential uuid-dev iasl git nasm python3"
        exit 1
    fi
done
echo -e "${GREEN}✓ All dependencies found${NC}"

echo -e "${YELLOW}[2/8] Preparing build directory...${NC}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ -d "edk2" ]; then
    echo "EDK2 already exists, cleaning old build artifacts..."
    rm -rf edk2/Build
    rm -rf edk2/MdeModulePkg/AllyTouchI2cDxe
fi
echo -e "${GREEN}✓ Build directory ready: $BUILD_DIR${NC}"

echo -e "${YELLOW}[3/8] Cloning EDK2...${NC}"
if [ ! -d "edk2" ]; then
    git clone --depth 1 --branch "$EDK2_BRANCH" https://github.com/tianocore/edk2.git
    cd edk2
    git submodule update --init --depth 1
    cd ..
else
    echo "EDK2 already exists, skipping clone"
fi
echo -e "${GREEN}✓ EDK2 ready${NC}"

echo -e "${YELLOW}[4/8] Setting up EDK2 environment...${NC}"
cd edk2
export WORKSPACE=$PWD
export PACKAGES_PATH=$WORKSPACE
export PYTHON_COMMAND=/usr/bin/python3
if [ ! -e /usr/bin/python ]; then
    # -n: fail instead of hanging on a password prompt when run non-interactively.
    # PYTHON_COMMAND above covers the build even if this symlink can't be made.
    echo "Creating python symlink..."
    sudo -n ln -sf /usr/bin/python3 /usr/bin/python || \
        echo "  (no passwordless sudo; continuing with PYTHON_COMMAND only)"
fi
if [ ! -f "BaseTools/Source/C/bin/GenFw" ]; then
    echo "Building EDK2 BaseTools..."
    make -C BaseTools -j$(nproc)
else
    echo "BaseTools already built, skipping"
fi
set +u
source edksetup.sh
set -u
echo -e "${GREEN}✓ EDK2 environment setup complete${NC}"

echo -e "${YELLOW}[5/8] Copying sources to EDK2...${NC}"
# Keep the repo layout (src/ + tools/probe/) so the probe's relative includes
# of ../../src/*.h resolve.
mkdir -p MdeModulePkg/AllyTouchI2cDxe
cp -r "$SCRIPT_DIR"/src "$SCRIPT_DIR"/tools MdeModulePkg/AllyTouchI2cDxe/
echo -e "${GREEN}✓ Sources copied${NC}"

echo -e "${YELLOW}[6/8] Adding modules to build configuration...${NC}"
if ! grep -q "AllyTouchI2cDxe/src/AllyTouchI2cDxe.inf" MdeModulePkg/MdeModulePkg.dsc; then
    sed -i '/\[Components\]/a \  MdeModulePkg/AllyTouchI2cDxe/src/AllyTouchI2cDxe.inf\n  MdeModulePkg/AllyTouchI2cDxe/tools/probe/AllyTouchProbe.inf' MdeModulePkg/MdeModulePkg.dsc
    echo "Modules added to build configuration"
else
    echo "Modules already in build configuration"
fi
echo -e "${GREEN}✓ Build configuration updated${NC}"

echo -e "${YELLOW}[7/8] Building driver + probe (${BUILD_TYPE})...${NC}"
set +u
source edksetup.sh
set -u
build -a $ARCH -t $TOOLCHAIN -b $BUILD_TYPE -p MdeModulePkg/MdeModulePkg.dsc -m MdeModulePkg/AllyTouchI2cDxe/src/AllyTouchI2cDxe.inf
build -a $ARCH -t $TOOLCHAIN -b $BUILD_TYPE -p MdeModulePkg/MdeModulePkg.dsc -m MdeModulePkg/AllyTouchI2cDxe/tools/probe/AllyTouchProbe.inf
echo -e "${GREEN}✓ Build successful${NC}"

echo -e "${YELLOW}[8/8] Preparing output...${NC}"
cd "$SCRIPT_DIR"
mkdir -p output
OUT_DIR="$BUILD_DIR/edk2/Build/MdeModule/${BUILD_TYPE}_${TOOLCHAIN}/${ARCH}"
for f in AllyTouchI2cDxe.efi AllyTouchProbe.efi; do
    if [ ! -f "$OUT_DIR/$f" ]; then
        echo -e "${RED}Error: $f not found at $OUT_DIR${NC}"
        exit 1
    fi
    cp "$OUT_DIR/$f" output/
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Build completed successfully!${NC}"
echo -e "${GREEN}========================================${NC}"
echo -e "Output files:"
echo -e "  ${GREEN}output/AllyTouchI2cDxe.efi${NC}  -> rEFInd drivers_x64/"
echo -e "  ${GREEN}output/AllyTouchProbe.efi${NC}   -> run from the UEFI Shell"
echo -e "${GREEN}========================================${NC}"
