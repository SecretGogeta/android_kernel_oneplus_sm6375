#!/bin/bash
set -e # Exit immediately if a command exits with a non-zero status.
set -o pipefail # Fail a pipe if any command in it fails.

# === Configuration Variables ===
ANYKERNEL_URL="https://github.com/osm0sis/AnyKernel3"
ANYKERNEL_DIR_NAME="AnyKernel3"
DEVICE_CODENAME="larry"
BASE_KERNEL_NAME_PREFIX="larry-KSUN-SuSFS"
YOUR_NAME="SecretGogeta"

# --- NDK Configuration (Example with r27c) ---
NDK_URL="https://dl.google.com/android/repository/android-ndk-r27c-linux.zip"
NDK_EXTRACTED_SUBDIR="android-ndk-r27c"

# --- GCC 4.9 Configuration - CLONING FROM LINEAGEOS GITHUB MIRROR ---
GCC_AARCH64_GIT_URL="https://github.com/LineageOS/android_prebuilts_gcc_linux-x86_aarch64_aarch64-linux-android-4.9.git"
GCC_AARCH64_GIT_BRANCH="lineage-19.1"  # CRITICAL: Ensures GCC 4.9. Verify this is correct.
GCC_AARCH64_TARGET_DIR_NAME="aarch64" # The name of the directory it will be cloned into

CUSTOM_DEFCONFIG_PATH="config/config.gz"

# KernelSU and SuSFS specific versions from rifsxd as per original request
KERNELSU_SETUP_URL="https://raw.githubusercontent.com/rifsxd/KernelSU-Next/next/kernel/setup.sh"
KERNELSU_BRANCH_ARG=""
SUSFS_SETUP_URL="https://raw.githubusercontent.com/rifsxd/KernelSU-Next/next-susfs/kernel/setup.sh"
SUSFS_BRANCH_ARG="next-susfs"

# Directories
KERNEL_TOP_DIR=$(pwd)
BUILD_TOOLS_DIR="${KERNEL_TOP_DIR}/build_tools"
LOG_FILE="${KERNEL_TOP_DIR}/build_log.txt"

exec &> >(tee -a "$LOG_FILE")

info() { echo "[INFO] $(date +'%Y-%m-%d %H:%M:%S') - $*"; }
error() { echo "[ERROR] $(date +'%Y-%m-%d %H:%M:%S') - $*" >&2; exit 1; }

# === Toolchain Setup ===
info "Starting kernel build process..."
info "Kernel source directory: $KERNEL_TOP_DIR"
info "Build tools will be located in: $BUILD_TOOLS_DIR"
mkdir -p "$BUILD_TOOLS_DIR"

info "Setting up NDK (Clang from NDK $NDK_EXTRACTED_SUBDIR)..."
NDK_INSTALL_PATH="$BUILD_TOOLS_DIR/$NDK_EXTRACTED_SUBDIR"
if [ ! -d "$NDK_INSTALL_PATH/toolchains/llvm/prebuilt/linux-x86_64" ]; then
    info "NDK $NDK_EXTRACTED_SUBDIR not found. Downloading and extracting..."
    wget --progress=bar:force:noscroll -q "$NDK_URL" -O "$BUILD_TOOLS_DIR/ndk.zip" || error "Failed to download NDK."
    unzip -q "$BUILD_TOOLS_DIR/ndk.zip" -d "$BUILD_TOOLS_DIR" || error "Failed to unzip NDK."
    [ -d "$NDK_INSTALL_PATH" ] || error "NDK extracted directory '$NDK_INSTALL_PATH' not found."
    rm "$BUILD_TOOLS_DIR/ndk.zip"
    info "NDK $NDK_EXTRACTED_SUBDIR setup complete."
else
    info "NDK $NDK_EXTRACTED_SUBDIR already present at $NDK_INSTALL_PATH."
fi
CLANG_BIN_PATH="$NDK_INSTALL_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin"
[ -f "$CLANG_BIN_PATH/clang" ] || error "Clang compiler not found."
[ -f "$CLANG_BIN_PATH/ld.lld" ] || error "LLD linker not found."

# --- MODIFIED GCC SETUP - CLONING FROM GITHUB INTO 'aarch64' ---
info "Setting up GCC 4.9 for aarch64 by cloning from LineageOS GitHub mirror..."
# Path where the GCC toolchain will reside (e.g., build_tools/aarch64)
GCC_AARCH64_INSTALL_PATH="$BUILD_TOOLS_DIR/$GCC_AARCH64_TARGET_DIR_NAME"

if [ ! -d "$GCC_AARCH64_INSTALL_PATH/bin" ]; then
    info "GCC aarch64 toolchain not found at $GCC_AARCH64_INSTALL_PATH. Cloning from $GCC_AARCH64_GIT_URL (branch: $GCC_AARCH64_GIT_BRANCH) into $GCC_AARCH64_TARGET_DIR_NAME..."
    # Clone with depth 1, specific branch, into the target directory name
    git clone --depth=1 --branch "$GCC_AARCH64_GIT_BRANCH" "$GCC_AARCH64_GIT_URL" "$GCC_AARCH64_INSTALL_PATH" || error "Failed to clone GCC toolchain from GitHub into $GCC_AARCH64_INSTALL_PATH."
    
    info "GCC aarch64 toolchain setup complete from GitHub clone."
else
    info "GCC aarch64 toolchain already present at $GCC_AARCH64_INSTALL_PATH (likely from cache)."
fi
GCC_AARCH64_BIN_PATH="$GCC_AARCH64_INSTALL_PATH/bin"
[ -f "$GCC_AARCH64_BIN_PATH/aarch64-linux-android-gcc" ] || error "aarch64-linux-android-gcc not found in $GCC_AARCH64_BIN_PATH."
# --- END OF MODIFIED GCC SETUP ---

info "Exporting environment variables for build..."
export PATH="$CLANG_BIN_PATH:$GCC_AARCH64_BIN_PATH:$PATH"
export ARCH="arm64"
export SUBARCH="arm64"
export KBUILD_BUILD_USER="$YOUR_NAME"
export KBUILD_BUILD_HOST="CirrusCI-SM6375"
export CC="clang"
export CLANG_TRIPLE="aarch64-linux-gnu-"
export LD="ld.lld"
export AR="llvm-ar"
export AS="llvm-as"
export NM="llvm-nm"
export OBJCOPY="llvm-objcopy"
export OBJDUMP="llvm-objdump"
export STRIP="llvm-strip"
export LLVM=1
export LLVM_IAS=1
export CROSS_COMPILE="${GCC_AARCH64_BIN_PATH}/aarch64-linux-android-" # This prefix must match tools in the bin dir

info "PATH set to: $PATH"
info "CC: $(command -v $CC)"
info "CROSS_COMPILE prefix: $CROSS_COMPILE"
info "KBUILD_BUILD_USER: $KBUILD_BUILD_USER"

# === Kernel Operations ===
cd "$KERNEL_TOP_DIR" || error "Failed to cd to kernel top directory."

info "Integrating KernelSU from $KERNELSU_SETUP_URL..."
rm -rf KernelSU drivers/kernelsu modules/kernelsu fs/kernelsu
if curl -LSs "$KERNELSU_SETUP_URL" | bash -s $KERNELSU_BRANCH_ARG; then
    info "KernelSU integration script completed."
else
    error "KernelSU integration script failed."
fi
([ -d "drivers/kernelsu" ] || [ -d "KernelSU" ]) || error "KernelSU directory not found post-integration."

info "Integrating SuSFS from $SUSFS_SETUP_URL (branch/arg: $SUSFS_BRANCH_ARG)..."
rm -rf fs/susfs
if curl -LSs "$SUSFS_SETUP_URL" | bash -s $SUSFS_BRANCH_ARG; then
    info "SuSFS integration script completed."
else
    error "SuSFS integration script failed."
fi
[ -d "fs/susfs" ] || error "SuSFS directory (fs/susfs) not found post-integration."

info "Cleaning up kernel source tree (make clean && make mrproper)..."
make O="$KERNEL_TOP_DIR" clean || error "'make clean' failed."
make O="$KERNEL_TOP_DIR" mrproper || error "'make mrproper' failed."

info "Applying custom defconfig from $CUSTOM_DEFCONFIG_PATH..."
[ -f "$CUSTOM_DEFCONFIG_PATH" ] || error "Mandatory defconfig '$CUSTOM_DEFCONFIG_PATH' not found."
zcat "$CUSTOM_DEFCONFIG_PATH" > .config || error "Failed to apply $CUSTOM_DEFCONFIG_PATH."

info "Injecting additional Kconfig options into .config..."
cat <<EOF >> .config

# === Custom Kconfig additions ===
# General
CONFIG_KPROBES=y
CONFIG_OVERLAY_FS=y
CONFIG_MODULE_SIG_ALL=n
CONFIG_MODULE_SIG_FORCE=n
CONFIG_TMPFS_XATTR=y
CONFIG_TMPFS_POSIX_ACL=y

# KernelSU
CONFIG_KSU=y
# CONFIG_KSU_KPROBES_HOOK=n

# SuSFS (KernelSU variant)
CONFIG_KSU_SUSFS=y
CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT=y
CONFIG_KSU_SUSFS_SUS_PATH=y
CONFIG_KSU_SUSFS_SUS_MOUNT=y
CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT=y
CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT=y
CONFIG_KSU_SUSFS_SUS_KSTAT=y
CONFIG_KSU_SUSFS_SUS_OVERLAYFS=n
CONFIG_KSU_SUSFS_TRY_UMOUNT=y
CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT=y
CONFIG_KSU_SUSFS_SPOOF_UNAME=y
CONFIG_KSU_SUSFS_ENABLE_LOG=y
CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS=y
CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG=y
CONFIG_KSU_SUSFS_OPEN_REDIRECT=y
CONFIG_KSU_SUSFS_SUS_SU=n
# === End of custom Kconfig additions ===
EOF

info "Running 'make olddefconfig' to finalize .config..."
make O="$KERNEL_TOP_DIR" olddefconfig || error "'make olddefconfig' failed."

info "Extracting kernel version..."
KERNEL_MAKE_VERSION=$(make -s O="$KERNEL_TOP_DIR" kernelversion) || error "Failed to get kernel version."
[ -n "$KERNEL_MAKE_VERSION" ] || error "Extracted kernel version is empty."
info "Kernel version determined as: $KERNEL_MAKE_VERSION"

NUM_THREADS=$(nproc)
info "Starting kernel compilation (using $NUM_THREADS threads)..."
if make O="$KERNEL_TOP_DIR" -j"$NUM_THREADS"; then
    info "Kernel compilation successful."
else
    error "Kernel compilation failed. Check $LOG_FILE."
fi

info "Verifying compiled kernel image..."
KERNEL_IMAGE_CANDIDATES=(
    "arch/arm64/boot/Image.gz-dtb" "arch/arm64/boot/Image-dtb"
    "arch/arm64/boot/Image.gz" "arch/arm64/boot/Image"
)
COMPILED_KERNEL_IMAGE=""
for img_path_suffix in "${KERNEL_IMAGE_CANDIDATES[@]}"; do
    if [ -f "$KERNEL_TOP_DIR/$img_path_suffix" ]; then
        COMPILED_KERNEL_IMAGE="$KERNEL_TOP_DIR/$img_path_suffix"
        info "Found kernel image: $COMPILED_KERNEL_IMAGE"
        break
    fi
done
[ -n "$COMPILED_KERNEL_IMAGE" ] || error "Compiled kernel image not found."

# === Packaging with AnyKernel3 ===
info "Setting up AnyKernel3 for packaging..."
ANYKERNEL_INSTALL_PATH="$BUILD_TOOLS_DIR/$ANYKERNEL_DIR_NAME"
[ -d "$ANYKERNEL_INSTALL_PATH" ] && rm -rf "$ANYKERNEL_INSTALL_PATH"
info "Cloning AnyKernel3 from $ANYKERNEL_URL..."
git clone --depth=1 "$ANYKERNEL_URL" "$ANYKERNEL_INSTALL_PATH" || error "Failed to clone AnyKernel3."

cd "$ANYKERNEL_INSTALL_PATH" || error "Failed to cd into AnyKernel3 directory."
rm -rf .git

info "Customizing AnyKernel3 (anykernel.sh)..."
KERNEL_STRING_CONTENT="${BASE_KERNEL_NAME_PREFIX}-${KERNEL_MAKE_VERSION} by ${YOUR_NAME}"
sed -i "s|^kernel.string=.*|kernel.string=$KERNEL_STRING_CONTENT|" anykernel.sh || error "Failed to set kernel.string."
sed -i "s|^device.name1=.*|device.name1=$DEVICE_CODENAME|" anykernel.sh || error "Failed to set device.name1."
sed -i "s|^do.devicecheck=.*|do.devicecheck=1|" anykernel.sh || error "Failed to set do.devicecheck."
sed -i '/^device.name[2-9]=/d' anykernel.sh
sed -i '/^device.name[0-9][0-9]=/d' anykernel.sh

info "Copying compiled kernel image ($COMPILED_KERNEL_IMAGE) to AnyKernel3..."
cp "$COMPILED_KERNEL_IMAGE" ./"$(basename "$COMPILED_KERNEL_IMAGE")" || error "Failed to copy kernel image."

info "Creating final flashable zip package..."
ZIP_DATE=$(date +%Y%m%d-%H%M)
FINAL_ZIP_NAME="${BASE_KERNEL_NAME_PREFIX}-${KERNEL_MAKE_VERSION}-${ZIP_DATE}.zip"
FINAL_ZIP_PATH="$KERNEL_TOP_DIR/$FINAL_ZIP_NAME"

zip -r9 "$FINAL_ZIP_PATH" . -x ".git/*" -x "README.md" -x "*.zip" || error "Failed to create flashable zip."

cd "$KERNEL_TOP_DIR" || error "Failed to cd back to kernel top directory."

# === Final Output ===
# ... (same as before) ...
info "---------------------------------------------------------------------"
info " Android Kernel Build Successfully Completed! "
info "---------------------------------------------------------------------"
info " Artifact      : $FINAL_ZIP_NAME (at $FINAL_ZIP_PATH)"
info " Kernel Version: $KERNEL_MAKE_VERSION"
info " Device        : $DEVICE_CODENAME"
info " Built by      : $YOUR_NAME on $KBUILD_BUILD_HOST"
info " Build Date    : $(date +'%Y-%m-%d %H:%M:%S %Z')"
info " Zip Date Tag  : $ZIP_DATE"
info "---------------------------------------------------------------------"
info " Toolchain Information:"
info "   Clang Path    : $CLANG_BIN_PATH"
info "   Clang Version : $($CC --version | head -n 1)"
info "   GCC Path      : $GCC_AARCH64_BIN_PATH"
info "   GCC Version   : $($CROSS_COMPILE"gcc" --version | head -n 1)"
info "---------------------------------------------------------------------"
info " Full build log available at: $LOG_FILE"
info "---------------------------------------------------------------------"

exit 0
