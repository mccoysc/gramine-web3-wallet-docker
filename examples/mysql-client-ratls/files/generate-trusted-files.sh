#!/bin/bash
# Generate trusted_files list for Gramine manifest
# This script analyzes binary dependencies using ldd and outputs
# the minimal set of files needed for Node.js client, launcher, and Gramine to run.
#
# Usage: ./generate-trusted-files.sh [output_file]
# If output_file is not specified, outputs to stdout
#
# The script:
# 1. Uses ldd to RECURSIVELY find all dependencies of Node.js and launcher
# 2. Adds Gramine runtime libraries and their dependencies (recursively)
# 3. Adds NSS libraries for DNS resolution (and their dependencies)
# 4. Adds RA-TLS library and its dependencies
# 5. Outputs in Gramine manifest format
#
# IMPORTANT: This script recursively resolves ALL transitive dependencies
# to ensure no libraries are missing at runtime.

set -e

OUTPUT_FILE="${1:-}"

# Binaries to analyze
LAUNCHER_PATH="/usr/local/bin/mysql-client-ratls-launcher"

# Node.js binary paths (check multiple locations)
NODE_PATH=""
for path in /opt/node-install/bin/node /usr/local/bin/node /usr/bin/node; do
    if [ -f "$path" ]; then
        NODE_PATH="$path"
        break
    fi
done

if [ -z "$NODE_PATH" ]; then
    echo "# Warning: Node.js binary not found" >&2
    NODE_PATH="/usr/bin/node"  # Default fallback
fi

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

# Temporary files for collecting dependencies
ALL_DEPS=$(mktemp)
SEEN_FILE=$(mktemp)
trap "rm -f $ALL_DEPS $SEEN_FILE" EXIT

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

# Function to check if a file has already been processed
already_seen() {
    local f="$1"
    grep -Fxq "$f" "$SEEN_FILE" 2>/dev/null
}

# Function to recursively add a dependency and all its transitive dependencies
# This ensures we don't miss any libraries that are dependencies of dependencies
add_dep_recursive() {
    local obj="$1"
    [ -z "$obj" ] && return 0
    [ ! -f "$obj" ] && return 0

    # Skip if already processed (prevents infinite loops and duplicate work)
    if already_seen "$obj"; then
        return 0
    fi

    # Mark as seen
    echo "$obj" >> "$SEEN_FILE"
    
    # Add to final deps list
    echo "$obj" >> "$ALL_DEPS"

    # Recursively process this object's dependencies
    local deps
    deps=$(extract_ldd_deps "$obj")
    if [ -n "$deps" ]; then
        echo "$deps" | while read -r dep; do
            if [ -n "$dep" ] && [ -f "$dep" ]; then
                add_dep_recursive "$dep"
            fi
        done
    fi
    
    return 0
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

echo "# Analyzing dependencies (with recursive resolution)..." >&2

# Collect dependencies from launcher (recursively)
echo "# Analyzing $LAUNCHER_PATH (recursive)..." >&2
if [ -f "$LAUNCHER_PATH" ]; then
    add_dep_recursive "$LAUNCHER_PATH"
else
    echo "# Warning: $LAUNCHER_PATH not found" >&2
fi

# Collect dependencies from Node.js (recursively)
echo "# Analyzing $NODE_PATH (recursive)..." >&2
if [ -f "$NODE_PATH" ]; then
    add_dep_recursive "$NODE_PATH"
else
    echo "# Warning: $NODE_PATH not found" >&2
fi

# Add Gramine runtime libraries and their dependencies (recursively)
echo "# Adding Gramine runtime libraries (recursive)..." >&2
if [ -n "$GRAMINE_RUNTIME_DIR" ] && [ -d "$GRAMINE_RUNTIME_DIR" ]; then
    # Add all libraries in the Gramine runtime directory
    for lib in "$GRAMINE_RUNTIME_DIR"/*.so*; do
        if [ -f "$lib" ]; then
            add_dep_recursive "$lib"
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
            add_dep_recursive "$lib"
        fi
    done
fi

# Add RA-TLS library and its dependencies (recursively)
echo "# Adding RA-TLS libraries (recursive)..." >&2
for ratls_lib in /usr/local/lib/x86_64-linux-gnu/libratls*.so* /usr/local/lib/libratls*.so* /usr/lib/x86_64-linux-gnu/libratls*.so*; do
    if [ -f "$ratls_lib" ]; then
        add_dep_recursive "$ratls_lib"
    fi
done

# Add NSS libraries for DNS resolution (loaded via dlopen) - recursively
echo "# Adding NSS libraries (recursive)..." >&2
for nss_lib in /lib/x86_64-linux-gnu/libnss_*.so* /usr/lib/x86_64-linux-gnu/libnss_*.so*; do
    if [ -f "$nss_lib" ]; then
        add_dep_recursive "$nss_lib"
    fi
done

# Add libcurl dependencies (for launcher HTTP requests) - recursively
echo "# Adding libcurl and dependencies (recursive)..." >&2
for curl_lib in /usr/lib/x86_64-linux-gnu/libcurl*.so*; do
    if [ -f "$curl_lib" ]; then
        add_dep_recursive "$curl_lib"
    fi
done

# Add SGX DCAP libraries (for attestation) - recursively
echo "# Adding SGX DCAP libraries (recursive)..." >&2
for sgx_lib in /usr/lib/x86_64-linux-gnu/libsgx*.so* /usr/lib/x86_64-linux-gnu/libdcap*.so*; do
    if [ -f "$sgx_lib" ]; then
        add_dep_recursive "$sgx_lib"
    fi
done

# Add OpenSSL libraries (for TLS) - recursively
echo "# Adding OpenSSL libraries (recursive)..." >&2
for ssl_lib in /opt/openssl-install/lib64/*.so* /opt/openssl-install/lib/*.so* /usr/lib/x86_64-linux-gnu/libssl*.so* /usr/lib/x86_64-linux-gnu/libcrypto*.so*; do
    if [ -f "$ssl_lib" ]; then
        add_dep_recursive "$ssl_lib"
    fi
done

# Sort and deduplicate, then resolve symlinks
echo "# Deduplicating and resolving symlinks..." >&2
UNIQUE_DEPS=$(sort -u "$ALL_DEPS" | while read -r dep; do
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
    echo "# Auto-generated trusted_files for MySQL RA-TLS Client"
    echo "# Generated on: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "# Binaries analyzed: $LAUNCHER_PATH, $NODE_PATH"
    echo ""
    echo "sgx.trusted_files = ["
    echo "  # Gramine LibOS and runtime"
    echo '  "file:{{ gramine.libos }}",'
    echo '  "file:{{ gramine.runtimedir() }}/",'
    echo ""
    echo "  # Launcher binary (entrypoint)"
    echo '  "file:{{ entrypoint }}",'
    echo ""
    echo "  # Node.js binary (target of execve)"
    echo "  \"file:$NODE_PATH\","
    echo ""
    echo "  # Client script"
    echo '  "file:/app/mysql-client.js",'
    echo ""
    echo "  # System libraries (auto-detected via ldd)"
    
    # Output each library
    echo "$UNIQUE_DEPS" | while read -r lib; do
        if [ -n "$lib" ]; then
            echo "  \"file:$lib\","
        fi
    done
    
    echo ""
    echo "  # Node.js modules directory (installed during Docker build)"
    echo '  "file:/app/node_modules/",'
    echo ""
    echo "  # System configuration files"
    echo '  "file:/etc/hosts",'
    echo '  "file:/etc/resolv.conf",'
    echo '  "file:/etc/nsswitch.conf",'
    echo '  "file:/etc/host.conf",'
    echo '  "file:/etc/gai.conf",'
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
