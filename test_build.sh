#!/bin/bash
# Test build script for TouchI2cDxe (driver + probe).
# Same approach as UsbXbox360Dxe's test_build.sh: clone EDK2, drop the sources
# into MdeModulePkg, build just these modules. Mirrors .github/workflows/build.yml.

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
MODULE_DIR="MdeModulePkg/TouchI2cDxe"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "${YELLOW}[1/8] Checking dependencies...${NC}"
for tool in git make gcc nasm iasl python3; do
    if ! command -v $tool &> /dev/null; then
        echo -e "${RED}Error: $tool is not installed${NC}"
        echo "Debian/Ubuntu: sudo apt install build-essential uuid-dev iasl git nasm python3"
        exit 1
    fi
done
echo -e "${GREEN}✓ All dependencies found${NC}"

echo -e "${YELLOW}[2/8] Preparing build directory...${NC}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ -d "edk2" ]; then
    rm -rf "edk2/$MODULE_DIR"
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

echo -e "${YELLOW}[5/8] Copying sources to EDK2...${NC}"
# Keep the repo layout (src/ + tools/probe/) so the probe's relative includes
# of ../../src/*.h resolve.
mkdir -p "$MODULE_DIR"
cp -r "$SCRIPT_DIR"/src "$SCRIPT_DIR"/tools "$MODULE_DIR"/

echo -e "${YELLOW}[6/8] Adding modules to build configuration...${NC}"
if ! grep -q "TouchI2cDxe/src/TouchI2cDxe.inf" MdeModulePkg/MdeModulePkg.dsc; then
    sed -i "/\[Components\]/a \  $MODULE_DIR/src/TouchI2cDxe.inf\n  $MODULE_DIR/tools/probe/TouchProbe.inf" MdeModulePkg/MdeModulePkg.dsc
fi

echo -e "${YELLOW}[7/8] Building driver + probe (${BUILD_TYPE})...${NC}"
build -a $ARCH -t $TOOLCHAIN -b $BUILD_TYPE \
      -p MdeModulePkg/MdeModulePkg.dsc -m "$MODULE_DIR/src/TouchI2cDxe.inf"
build -a $ARCH -t $TOOLCHAIN -b $BUILD_TYPE \
      -p MdeModulePkg/MdeModulePkg.dsc -m "$MODULE_DIR/tools/probe/TouchProbe.inf"

echo -e "${YELLOW}[8/8] Preparing output...${NC}"
cd "$SCRIPT_DIR"
mkdir -p output
OUT_DIR="$BUILD_DIR/edk2/Build/MdeModule/${BUILD_TYPE}_${TOOLCHAIN}/${ARCH}"
for f in TouchI2cDxe.efi TouchProbe.efi; do
    if [ ! -f "$OUT_DIR/$f" ]; then
        echo -e "${RED}Error: $f not found at $OUT_DIR${NC}"
        exit 1
    fi
    cp "$OUT_DIR/$f" output/
done

echo -e "${GREEN}Build completed:${NC}"
echo -e "  ${GREEN}output/TouchI2cDxe.efi${NC}  -> rEFInd drivers_x64/"
echo -e "  ${GREEN}output/TouchProbe.efi${NC}   -> run from the UEFI Shell"
