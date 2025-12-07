#!/bin/bash
# Generate trusted_files list for Gramine manifest
# This script analyzes binary dependencies using ldd and outputs
# the minimal set of files needed for MySQL, launcher, and Gramine to run.
#
# Usage: ./generate-trusted-files.sh [output_file]
# If output_file is not specified, outputs to stdout
#
# The script:
# 1. Uses ldd to find direct dependencies of mysqld and launcher
# 2. Adds Gramine runtime libraries and their dependencies
# 3. Adds MySQL plugins that are commonly needed
# 4. Adds NSS libraries for DNS resolution
# 5. Adds MySQL configuration files
# 6. Outputs in Gramine manifest format

set -e

OUTPUT_FILE="${1:-}"

# Binaries to analyze
MYSQLD_PATH="/usr/sbin/mysqld"
LAUNCHER_PATH="/usr/local/bin/mysql-ratls-launcher"

# Gramine paths (auto-detect)
GRAMINE_LIBDIR=""
for dir in /usr/local/lib/x86_64-linux-gnu/gramine /usr/lib/x86_64-linux-gnu/gramine; do
    if [ -d "$dir" ]; then
        GRAMINE_LIBDIR="$dir"
        break
    fi
done

GRAMINE_RUNTIME_DIR=""
for dir in /usr/local/lib/x86_64-linux-gnu/gramine/runtime/glibc /usr/lib/x86_64-linux-gnu/gramine/runtime/glibc; do
    if [ -d "$dir" ]; then
        GRAMINE_RUNTIME_DIR="$dir"
        break
    fi
done

# Temporary file for collecting dependencies
DEPS_FILE=$(mktemp)
trap "rm -f $DEPS_FILE" EXIT

# Function to extract library paths from ldd output
extract_ldd_deps() {
    local binary="$1"
    if [ -f "$binary" ]; then
        ldd "$binary" 2>/dev/null | \
            grep -E '^\s+.+ => .+ \(0x' | \
            awk '{print $3}' | \
            sort -u
    fi
}

# Function to resolve symlinks to get the actual file
resolve_symlink() {
    local path="$1"
    if [ -L "$path" ]; then
        # Get the actual file the symlink points to
        readlink -f "$path"
    else
        echo "$path"
    fi
}

# Function to add all files in a directory (non-recursive)
add_dir_files() {
    local dir="$1"
    if [ -d "$dir" ]; then
        find "$dir" -maxdepth 1 -type f -o -type l 2>/dev/null
    fi
}

echo "# Analyzing dependencies..." >&2

# Collect dependencies from mysqld
echo "# Analyzing $MYSQLD_PATH..." >&2
if [ -f "$MYSQLD_PATH" ]; then
    extract_ldd_deps "$MYSQLD_PATH" >> "$DEPS_FILE"
else
    echo "# Warning: $MYSQLD_PATH not found" >&2
fi

# Collect dependencies from launcher
echo "# Analyzing $LAUNCHER_PATH..." >&2
if [ -f "$LAUNCHER_PATH" ]; then
    extract_ldd_deps "$LAUNCHER_PATH" >> "$DEPS_FILE"
else
    echo "# Warning: $LAUNCHER_PATH not found" >&2
fi

# Add Gramine runtime libraries and their dependencies
echo "# Adding Gramine runtime libraries..." >&2
if [ -n "$GRAMINE_RUNTIME_DIR" ] && [ -d "$GRAMINE_RUNTIME_DIR" ]; then
    # Add all libraries in the Gramine runtime directory
    for lib in "$GRAMINE_RUNTIME_DIR"/*.so*; do
        if [ -f "$lib" ]; then
            echo "$lib" >> "$DEPS_FILE"
            # Also get dependencies of each Gramine runtime library
            extract_ldd_deps "$lib" >> "$DEPS_FILE"
        fi
    done
else
    echo "# Warning: Gramine runtime directory not found" >&2
fi

# Add Gramine direct libraries (libpal, libsysdb, etc.)
echo "# Adding Gramine direct libraries..." >&2
if [ -n "$GRAMINE_LIBDIR" ] && [ -d "$GRAMINE_LIBDIR" ]; then
    for lib in "$GRAMINE_LIBDIR"/direct/*.so* "$GRAMINE_LIBDIR"/sgx/*.so*; do
        if [ -f "$lib" ]; then
            echo "$lib" >> "$DEPS_FILE"
        fi
    done
fi

# Add RA-TLS library and its dependencies
echo "# Adding RA-TLS libraries..." >&2
for ratls_lib in /usr/local/lib/x86_64-linux-gnu/libratls*.so* /usr/local/lib/libratls*.so* /usr/lib/x86_64-linux-gnu/libratls*.so*; do
    if [ -f "$ratls_lib" ]; then
        echo "$ratls_lib" >> "$DEPS_FILE"
        extract_ldd_deps "$ratls_lib" >> "$DEPS_FILE"
    fi
done

# Add NSS libraries for DNS resolution (loaded via dlopen)
echo "# Adding NSS libraries..." >&2
for nss_lib in /lib/x86_64-linux-gnu/libnss_*.so* /usr/lib/x86_64-linux-gnu/libnss_*.so*; do
    if [ -f "$nss_lib" ]; then
        echo "$nss_lib" >> "$DEPS_FILE"
    fi
done

# Add MySQL plugins directory (commonly needed plugins)
echo "# Adding MySQL plugins..." >&2
MYSQL_PLUGIN_DIR="/usr/lib/mysql/plugin"
if [ -d "$MYSQL_PLUGIN_DIR" ]; then
    # Add essential plugins
    for plugin in \
        "$MYSQL_PLUGIN_DIR/mysql_native_password.so" \
        "$MYSQL_PLUGIN_DIR/caching_sha2_password.so" \
        "$MYSQL_PLUGIN_DIR/sha256_password.so" \
        "$MYSQL_PLUGIN_DIR/auth_socket.so"; do
        if [ -f "$plugin" ]; then
            echo "$plugin" >> "$DEPS_FILE"
        fi
    done
fi

# Add libcurl dependencies (for launcher HTTP requests)
echo "# Adding libcurl and dependencies..." >&2
for curl_lib in /usr/lib/x86_64-linux-gnu/libcurl*.so*; do
    if [ -f "$curl_lib" ]; then
        echo "$curl_lib" >> "$DEPS_FILE"
        # Also get libcurl's dependencies
        extract_ldd_deps "$curl_lib" >> "$DEPS_FILE"
    fi
done

# Add SGX DCAP libraries (for attestation)
echo "# Adding SGX DCAP libraries..." >&2
for sgx_lib in /usr/lib/x86_64-linux-gnu/libsgx*.so* /usr/lib/x86_64-linux-gnu/libdcap*.so*; do
    if [ -f "$sgx_lib" ]; then
        echo "$sgx_lib" >> "$DEPS_FILE"
        extract_ldd_deps "$sgx_lib" >> "$DEPS_FILE"
    fi
done

# Sort and deduplicate, then resolve symlinks
echo "# Deduplicating and resolving symlinks..." >&2
UNIQUE_DEPS=$(sort -u "$DEPS_FILE" | while read -r dep; do
    if [ -n "$dep" ] && [ -f "$dep" ]; then
        # Output both the symlink and the resolved path
        echo "$dep"
        resolved=$(resolve_symlink "$dep")
        if [ "$resolved" != "$dep" ]; then
            echo "$resolved"
        fi
    fi
done | sort -u)

# Generate output
generate_output() {
    echo "# Auto-generated trusted_files for MySQL RA-TLS"
    echo "# Generated on: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "# Binaries analyzed: $MYSQLD_PATH, $LAUNCHER_PATH"
    echo ""
    echo "sgx.trusted_files = ["
    echo "  # Gramine LibOS and runtime"
    echo '  "file:{{ gramine.libos }}",'
    echo '  "file:{{ gramine.runtimedir() }}/",'
    echo ""
    echo "  # Launcher binary (entrypoint)"
    echo '  "file:{{ entrypoint }}",'
    echo ""
    echo "  # MySQL server binary (target of execve)"
    echo '  "file:/usr/sbin/mysqld",'
    echo ""
    echo "  # System libraries (auto-detected via ldd)"
    
    # Output each library
    echo "$UNIQUE_DEPS" | while read -r lib; do
        if [ -n "$lib" ]; then
            echo "  \"file:$lib\","
        fi
    done
    
    echo ""
    echo "  # MySQL configuration files"
    echo '  "file:/etc/mysql/my.cnf",'
    echo '  "file:/etc/mysql/mysql.cnf",'
    echo '  "file:/etc/mysql/mysql.conf.d/",'
    echo '  "file:/etc/mysql/conf.d/",'
    echo ""
    echo "  # MySQL support files (charsets, error messages)"
    echo '  "file:/usr/share/mysql/charsets/",'
    echo '  "file:/usr/share/mysql/english/",'
    echo "]"
}

# Count dependencies
DEP_COUNT=$(echo "$UNIQUE_DEPS" | grep -c . || echo "0")
echo "# Found $DEP_COUNT unique library dependencies" >&2

if [ -n "$OUTPUT_FILE" ]; then
    generate_output > "$OUTPUT_FILE"
    echo "# Output written to: $OUTPUT_FILE" >&2
else
    generate_output
fi

echo "# Done!" >&2
