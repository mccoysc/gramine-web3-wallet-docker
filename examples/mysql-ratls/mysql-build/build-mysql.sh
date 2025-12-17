#!/bin/bash
set -e

# MySQL Source Build Script for SGX/Gramine Environment
# This script downloads MySQL source, applies custom patches for Group Replication
# enhancements, and compiles MySQL with the necessary modifications.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES_DIR="${SCRIPT_DIR}/patches"

# Configuration
MYSQL_VERSION="${MYSQL_VERSION:-8.0.40}"
MYSQL_SOURCE_URL="https://dev.mysql.com/get/Downloads/MySQL-8.0/mysql-${MYSQL_VERSION}.tar.gz"
BUILD_DIR="${BUILD_DIR:-/tmp/mysql-build}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local/mysql}"
PARALLEL_JOBS="${PARALLEL_JOBS:-$(nproc)}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

usage() {
    cat << EOF
Usage: $0 [OPTIONS]

MySQL Source Build Script for SGX/Gramine Environment

Options:
    -v, --version VERSION    MySQL version to build (default: ${MYSQL_VERSION})
    -b, --build-dir DIR      Build directory (default: ${BUILD_DIR})
    -p, --prefix DIR         Installation prefix (default: ${INSTALL_PREFIX})
    -j, --jobs N             Number of parallel build jobs (default: ${PARALLEL_JOBS})
    -c, --clean              Clean build directory before building
    -s, --skip-download      Skip downloading MySQL source (use existing)
    -o, --output FILE        Output tarball filename
    -h, --help               Show this help message

Environment Variables:
    MYSQL_VERSION            MySQL version to build
    BUILD_DIR                Build directory
    INSTALL_PREFIX           Installation prefix
    PARALLEL_JOBS            Number of parallel build jobs

Examples:
    $0                       # Build with defaults
    $0 -v 8.0.40 -j 4        # Build MySQL 8.0.40 with 4 parallel jobs
    $0 -c -o mysql-sgx.tar.gz # Clean build and create output tarball

EOF
    exit 0
}

# Parse command line arguments
CLEAN_BUILD=false
SKIP_DOWNLOAD=false
OUTPUT_TARBALL=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--version)
            MYSQL_VERSION="$2"
            shift 2
            ;;
        -b|--build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -p|--prefix)
            INSTALL_PREFIX="$2"
            shift 2
            ;;
        -j|--jobs)
            PARALLEL_JOBS="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -s|--skip-download)
            SKIP_DOWNLOAD=true
            shift
            ;;
        -o|--output)
            OUTPUT_TARBALL="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            ;;
    esac
done

# Update source URL with version
MYSQL_SOURCE_URL="https://dev.mysql.com/get/Downloads/MySQL-8.0/mysql-${MYSQL_VERSION}.tar.gz"

log_info "MySQL Build Configuration:"
log_info "  Version: ${MYSQL_VERSION}"
log_info "  Build Directory: ${BUILD_DIR}"
log_info "  Install Prefix: ${INSTALL_PREFIX}"
log_info "  Parallel Jobs: ${PARALLEL_JOBS}"
log_info "  Patches Directory: ${PATCHES_DIR}"

# Check for required tools
check_dependencies() {
    log_info "Checking build dependencies..."
    
    local missing_deps=()
    
    for cmd in cmake make gcc g++ patch tar wget; do
        if ! command -v "$cmd" &> /dev/null; then
            missing_deps+=("$cmd")
        fi
    done
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        log_error "Missing required tools: ${missing_deps[*]}"
        log_info "Install them with: apt-get install build-essential cmake wget"
        exit 1
    fi
    
    log_info "All build dependencies are available"
}

# Install build dependencies
install_build_deps() {
    log_info "Installing MySQL build dependencies..."
    
    apt-get update
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libssl-dev \
        libncurses5-dev \
        libudev-dev \
        libtirpc-dev \
        rpcsvc-proto \
        bison \
        libaio-dev \
        libldap2-dev \
        libsasl2-dev \
        libcurl4-openssl-dev \
        libevent-dev \
        libmecab-dev \
        libnuma-dev \
        libprotobuf-dev \
        libprotoc-dev \
        protobuf-compiler \
        zlib1g-dev \
        liblz4-dev \
        libzstd-dev \
        libreadline-dev \
        libfido2-dev
    
    log_info "Build dependencies installed"
}

# Download MySQL source
download_mysql_source() {
    if [ "$SKIP_DOWNLOAD" = true ] && [ -d "${BUILD_DIR}/mysql-${MYSQL_VERSION}" ]; then
        log_info "Skipping download, using existing source"
        return 0
    fi
    
    log_info "Downloading MySQL ${MYSQL_VERSION} source..."
    
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"
    
    if [ ! -f "mysql-${MYSQL_VERSION}.tar.gz" ]; then
        wget -q --show-progress "${MYSQL_SOURCE_URL}" -O "mysql-${MYSQL_VERSION}.tar.gz"
    else
        log_info "Source tarball already exists, skipping download"
    fi
    
    log_info "Extracting MySQL source..."
    tar -xzf "mysql-${MYSQL_VERSION}.tar.gz"
    
    log_info "MySQL source downloaded and extracted"
}

# Apply patches
apply_patches() {
    log_info "Applying custom patches..."
    
    cd "${BUILD_DIR}/mysql-${MYSQL_VERSION}"
    
    if [ ! -d "${PATCHES_DIR}" ]; then
        log_warn "Patches directory not found: ${PATCHES_DIR}"
        return 0
    fi
    
    local patch_count=0
    for patch_file in "${PATCHES_DIR}"/*.patch; do
        if [ -f "$patch_file" ]; then
            log_info "Applying patch: $(basename "$patch_file")"
            
            # Try to apply patch, skip if already applied
            if patch -p1 --dry-run < "$patch_file" &> /dev/null; then
                patch -p1 < "$patch_file"
                ((patch_count++))
            else
                log_warn "Patch may already be applied or conflicts: $(basename "$patch_file")"
            fi
        fi
    done
    
    log_info "Applied ${patch_count} patches"
}

# Configure MySQL build
configure_mysql() {
    log_info "Configuring MySQL build..."
    
    cd "${BUILD_DIR}/mysql-${MYSQL_VERSION}"
    
    mkdir -p build
    cd build
    
    cmake .. \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DWITH_SSL=system \
        -DWITH_ZLIB=bundled \
        -DWITH_LZ4=bundled \
        -DWITH_ZSTD=bundled \
        -DWITH_PROTOBUF=bundled \
        -DWITH_BOOST="${BUILD_DIR}/boost" \
        -DDOWNLOAD_BOOST=1 \
        -DWITH_INNODB_MEMCACHED=OFF \
        -DWITH_ROUTER=OFF \
        -DWITH_UNIT_TESTS=OFF \
        -DWITH_DEBUG=OFF \
        -DWITH_AUTHENTICATION_LDAP=OFF \
        -DWITH_AUTHENTICATION_KERBEROS=OFF \
        -DWITH_AUTHENTICATION_FIDO=OFF \
        -DWITH_NDB=OFF \
        -DWITH_NDBCLUSTER=OFF \
        -DWITH_EXAMPLE_STORAGE_ENGINE=OFF \
        -DWITH_FEDERATED_STORAGE_ENGINE=OFF \
        -DWITH_ARCHIVE_STORAGE_ENGINE=OFF \
        -DWITH_BLACKHOLE_STORAGE_ENGINE=OFF \
        -DFORCE_INSOURCE_BUILD=1 \
        -DWITH_GROUP_REPLICATION=ON \
        -DENABLED_LOCAL_INFILE=ON \
        -DMYSQL_UNIX_ADDR=/var/run/mysqld/mysqld.sock \
        -DMYSQL_DATADIR=/var/lib/mysql \
        -DSYSCONFDIR=/etc/mysql
    
    log_info "MySQL build configured"
}

# Build MySQL
build_mysql() {
    log_info "Building MySQL with ${PARALLEL_JOBS} parallel jobs..."
    
    cd "${BUILD_DIR}/mysql-${MYSQL_VERSION}/build"
    
    make -j"${PARALLEL_JOBS}"
    
    log_info "MySQL build completed"
}

# Install MySQL
install_mysql() {
    log_info "Installing MySQL to ${INSTALL_PREFIX}..."
    
    cd "${BUILD_DIR}/mysql-${MYSQL_VERSION}/build"
    
    make install
    
    log_info "MySQL installed"
}

# Create output tarball
create_tarball() {
    if [ -z "$OUTPUT_TARBALL" ]; then
        return 0
    fi
    
    log_info "Creating output tarball: ${OUTPUT_TARBALL}"
    
    cd "$(dirname "${INSTALL_PREFIX}")"
    tar -czf "${BUILD_DIR}/${OUTPUT_TARBALL}" "$(basename "${INSTALL_PREFIX}")"
    
    log_info "Tarball created: ${BUILD_DIR}/${OUTPUT_TARBALL}"
}

# Clean up build directory
cleanup() {
    if [ "$CLEAN_BUILD" = true ]; then
        log_info "Cleaning up build directory..."
        rm -rf "${BUILD_DIR}/mysql-${MYSQL_VERSION}"
        rm -rf "${BUILD_DIR}/boost"
        log_info "Cleanup completed"
    fi
}

# Main execution
main() {
    log_info "Starting MySQL build for SGX/Gramine environment"
    
    check_dependencies
    
    if [ "$CLEAN_BUILD" = true ] && [ -d "${BUILD_DIR}/mysql-${MYSQL_VERSION}" ]; then
        log_info "Cleaning existing build directory..."
        rm -rf "${BUILD_DIR}/mysql-${MYSQL_VERSION}"
    fi
    
    download_mysql_source
    apply_patches
    configure_mysql
    build_mysql
    install_mysql
    create_tarball
    
    log_info "MySQL build completed successfully!"
    log_info "Installation directory: ${INSTALL_PREFIX}"
    
    if [ -n "$OUTPUT_TARBALL" ]; then
        log_info "Output tarball: ${BUILD_DIR}/${OUTPUT_TARBALL}"
    fi
}

# Run main function
main
