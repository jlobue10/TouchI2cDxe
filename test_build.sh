#!/bin/bash
# Test build script for AllyTouchI2cDxe driver
# Same approach as UsbXbox360Dxe's test_build.sh: clone EDK2, drop the driver
# into MdeModulePkg, build just this module.

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
BUILD_TYPE="${BUILD_TYPE:-RELEASE}"
DRIVER_DIR="MdeModulePkg/Bus/I2c/AllyTouchI2cDxe"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "${YELLOW}[1/8] Checking dependencies...${NC}"
for tool in git make gcc nasm iasl python3; do
    if ! command -v $tool &> /dev/null; then
        echo -e "${RED}Error: $tool is not installed${NC}"
        exit 1
    fi
done
echo -e "${GREEN}✓ All dependencies found${NC}"

echo -e "${YELLOW}[2/8] Preparing build directory...${NC}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ -d "edk2" ]; then
    rm -rf "edk2/$DRIVER_DIR"
fi

echo -e "${YELLOW}[3/8] Cloning EDK2...${NC}"
if [ ! -d "edk2" ]; then
    git clone --depth 1 --branch "$EDK2_BRANCH" https://github.com/tianocore/edk2.git
    cd edk2
    git submodule update --init --depth 1
    cd ..
fi

echo -e "${YELLOW}[4/8] Setting up EDK2 environment...${NC}"
cd edk2
export WORKSPACE=$PWD
export PACKAGES_PATH=$WORKSPACE
export PYTHON_COMMAND="$(command -v python3)"

if [ ! -f "BaseTools/Source/C/bin/GenFw" ]; then
    # -std=gnu17 + -Wno-error: stable202411 BaseTools (incl. the K&R-era Pccts
    # code) does not compile under GCC >= 15, whose C23 default makes () mean
    # (void). Harmless on older compilers.
    make -C BaseTools -j"$(nproc)" CC="gcc -std=gnu17" BUILD_OPTFLAGS="-O2 -Wno-error"
fi

set +u
source edksetup.sh
set -u

echo -e "${YELLOW}[5/8] Copying driver source to EDK2...${NC}"
mkdir -p "$DRIVER_DIR"
cp "$SCRIPT_DIR"/src/*.c "$SCRIPT_DIR"/src/*.h "$SCRIPT_DIR"/src/*.inf "$DRIVER_DIR"/

echo -e "${YELLOW}[6/8] Adding driver to build configuration...${NC}"
if ! grep -q "AllyTouchI2cDxe/AllyTouchI2cDxe.inf" MdeModulePkg/MdeModulePkg.dsc; then
    sed -i "/\[Components\]/a \  $DRIVER_DIR/AllyTouchI2cDxe.inf" MdeModulePkg/MdeModulePkg.dsc
fi

echo -e "${YELLOW}[7/8] Building driver (${BUILD_TYPE})...${NC}"
build -a $ARCH -t $TOOLCHAIN -b $BUILD_TYPE \
      -p MdeModulePkg/MdeModulePkg.dsc -m "$DRIVER_DIR/AllyTouchI2cDxe.inf"

echo -e "${YELLOW}[8/8] Preparing output...${NC}"
cd "$SCRIPT_DIR"
mkdir -p output
EFI_PATH="$BUILD_DIR/edk2/Build/MdeModule/${BUILD_TYPE}_${TOOLCHAIN}/${ARCH}/AllyTouchI2cDxe.efi"

if [ -f "$EFI_PATH" ]; then
    cp "$EFI_PATH" output/
    echo -e "${GREEN}Build completed: output/AllyTouchI2cDxe.efi ($(du -h output/AllyTouchI2cDxe.efi | cut -f1))${NC}"
else
    echo -e "${RED}Error: built .efi not found at $EFI_PATH${NC}"
    exit 1
fi
