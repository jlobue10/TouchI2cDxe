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
# X64 (default) builds the driver + probe for rEFInd's drivers_x64/;
# ARCH=AARCH64 builds the driver alone for drivers_aa64/ (the probe sweeps
# AMD/Intel controllers and is x86-only). On an x86-64 host an AARCH64 build
# needs a cross compiler: GCC5_AARCH64_PREFIX=aarch64-linux-gnu- (Debian
# package gcc-aarch64-linux-gnu); on an ARM64 host the native gcc is used.
ARCH="${ARCH:-X64}"
TOOLCHAIN="GCC5"
BUILD_TYPE="${BUILD_TYPE:-RELEASE}"
MODULE_DIR="MdeModulePkg/TouchI2cDxe"
if [ "$ARCH" != "X64" ] && [ "$ARCH" != "AARCH64" ]; then
    echo "Error: ARCH must be X64 or AARCH64 (got '$ARCH')"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "${YELLOW}[1/8] Checking dependencies...${NC}"
# nasm is only used by the X64 BaseLib/IoLib assembly.
DEPS="git make gcc iasl python3"
[ "$ARCH" = "X64" ] && DEPS="$DEPS nasm"
for tool in $DEPS; do
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

echo -e "${YELLOW}[7/8] Building driver (${ARCH} ${BUILD_TYPE})...${NC}"
build -a $ARCH -t $TOOLCHAIN -b $BUILD_TYPE \
      -p MdeModulePkg/MdeModulePkg.dsc -m "$MODULE_DIR/src/TouchI2cDxe.inf"
if [ "$ARCH" = "X64" ]; then
    echo -e "${YELLOW}      Building probe...${NC}"
    build -a $ARCH -t $TOOLCHAIN -b $BUILD_TYPE \
          -p MdeModulePkg/MdeModulePkg.dsc -m "$MODULE_DIR/tools/probe/TouchProbe.inf"
fi

echo -e "${YELLOW}[8/8] Preparing output...${NC}"
cd "$SCRIPT_DIR"
mkdir -p output
OUT_DIR="$BUILD_DIR/edk2/Build/MdeModule/${BUILD_TYPE}_${TOOLCHAIN}/${ARCH}"
if [ "$ARCH" = "X64" ]; then
    # Release asset names: the x64 driver keeps its historical name (the
    # rEFInd install scripts fetch it by that name), the AArch64 build
    # carries the EFI architecture suffix.
    FILES="TouchI2cDxe.efi:TouchI2cDxe.efi TouchProbe.efi:TouchProbe.efi"
else
    FILES="TouchI2cDxe.efi:TouchI2cDxe_aa64.efi"
fi
for pair in $FILES; do
    src="${pair%%:*}"
    dst="${pair##*:}"
    if [ ! -f "$OUT_DIR/$src" ]; then
        echo -e "${RED}Error: $src not found at $OUT_DIR${NC}"
        exit 1
    fi
    cp "$OUT_DIR/$src" "output/$dst"
done

echo -e "${GREEN}Build completed:${NC}"
if [ "$ARCH" = "X64" ]; then
    echo -e "  ${GREEN}output/TouchI2cDxe.efi${NC}       -> rEFInd drivers_x64/"
    echo -e "  ${GREEN}output/TouchProbe.efi${NC}        -> run from the UEFI Shell"
else
    echo -e "  ${GREEN}output/TouchI2cDxe_aa64.efi${NC}  -> rEFInd drivers_aa64/ (as TouchI2cDxe.efi)"
fi
