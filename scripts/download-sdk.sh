#!/bin/bash
#
# WeCom Archive SDK Download Script
# This script automatically downloads the official WeCom SDK for your platform
#

set -e

SDK_VERSION="v3_20250205"
SDK_X86_URL="https://wwcdn.weixin.qq.com/node/wwcomm/sdk_x86_v3_20250205.tgz"
SDK_ARM_URL="https://wwcdn.weixin.qq.com/node/wwcomm/sdk_arm_v3_20250205.tgz"
DEFAULT_INSTALL_PATH="/usr/local/lib"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Detect architecture
detect_arch() {
    local arch=$(uname -m)
    case "$arch" in
        x86_64|amd64)
            echo "x86"
            ;;
        aarch64|arm64)
            echo "arm"
            ;;
        *)
            print_error "Unsupported architecture: $arch"
            exit 1
            ;;
    esac
}

# Check if running on Linux
check_os() {
    local os=$(uname -s)
    if [ "$os" != "Linux" ]; then
        print_error "This script only supports Linux. Detected: $os"
        print_info "For Windows, please download the SDK manually from:"
        print_info "https://wwcdn.weixin.qq.com/node/wework/images/sdk_win_v3.zip"
        exit 1
    fi
}

# Download and install SDK
download_sdk() {
    local arch=$1
    local install_path=${2:-$DEFAULT_INSTALL_PATH}
    local url

    if [ "$arch" = "x86" ]; then
        url=$SDK_X86_URL
    else
        url=$SDK_ARM_URL
    fi

    print_info "Detected architecture: $arch"
    print_info "Downloading SDK from: $url"

    # Create temp directory
    local tmp_dir=$(mktemp -d)
    trap "rm -rf $tmp_dir" EXIT

    # Download
    if command -v curl &> /dev/null; then
        curl -sL "$url" -o "$tmp_dir/sdk.tgz"
    elif command -v wget &> /dev/null; then
        wget -q "$url" -O "$tmp_dir/sdk.tgz"
    else
        print_error "Neither curl nor wget found. Please install one of them."
        exit 1
    fi

    print_info "Extracting SDK..."

    # Extract only the .so file
    tar -xzf "$tmp_dir/sdk.tgz" -C "$tmp_dir"

    # Find the .so file
    local so_file=$(find "$tmp_dir" -name "libWeWorkFinanceSdk_C.so" -type f | head -1)

    if [ -z "$so_file" ]; then
        print_error "Could not find libWeWorkFinanceSdk_C.so in the downloaded archive"
        exit 1
    fi

    # Check if we need sudo
    if [ -w "$install_path" ]; then
        cp "$so_file" "$install_path/"
    else
        print_warn "Need sudo to install to $install_path"
        sudo cp "$so_file" "$install_path/"
    fi

    # Update ldconfig if available
    if command -v ldconfig &> /dev/null; then
        if [ -w "/etc/ld.so.conf.d" ]; then
            ldconfig
        else
            sudo ldconfig
        fi
    fi

    print_info "SDK installed successfully to: $install_path/libWeWorkFinanceSdk_C.so"
    print_info ""
    print_info "You can now configure PHP to use the SDK by adding to php.ini:"
    print_info "  wecomarchive.sdk_lib_path=$install_path/libWeWorkFinanceSdk_C.so"
}

# Show usage
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -p, --path PATH    Installation path (default: $DEFAULT_INSTALL_PATH)"
    echo "  -a, --arch ARCH    Force architecture: x86 or arm (default: auto-detect)"
    echo "  -h, --help         Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                           # Auto-detect and install to default path"
    echo "  $0 -p /opt/lib               # Install to custom path"
    echo "  $0 -a arm                    # Force ARM architecture"
}

# Main
main() {
    local install_path=$DEFAULT_INSTALL_PATH
    local arch=""

    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -p|--path)
                install_path="$2"
                shift 2
                ;;
            -a|--arch)
                arch="$2"
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done

    check_os

    if [ -z "$arch" ]; then
        arch=$(detect_arch)
    fi

    # Create install path if it doesn't exist
    if [ ! -d "$install_path" ]; then
        print_info "Creating directory: $install_path"
        if [ -w "$(dirname $install_path)" ]; then
            mkdir -p "$install_path"
        else
            sudo mkdir -p "$install_path"
        fi
    fi

    download_sdk "$arch" "$install_path"

    print_info ""
    print_info "Installation complete! SDK version: $SDK_VERSION"
}

main "$@"
