#!/bin/bash
# Generate trusted_files list for Gramine manifest
# This script analyzes binary dependencies using ldd and outputs
# the minimal set of files needed for MySQL, launcher, and Gramine to run.
#
# Usage: ./generate-trusted-files.sh [output_file]
# If output_file is not specified, outputs to stdout
#
# The script:
# 1. Uses ldd to RECURSIVELY find all dependencies of mysqld and launcher
# 2. Adds Gramine runtime libraries and their dependencies (recursively)
# 3. Adds MySQL plugins that are commonly needed (and their dependencies)
# 4. Adds NSS libraries for DNS resolution (and their dependencies)
# 5. Adds MySQL configuration files
# 6. Outputs in Gramine manifest format
#
# IMPORTANT: This script recursively resolves ALL transitive dependencies
# to ensure no libraries are missing at runtime.

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

# Collect dependencies from mysqld (recursively)
echo "# Analyzing $MYSQLD_PATH (recursive)..." >&2
if [ -f "$MYSQLD_PATH" ]; then
    add_dep_recursive "$MYSQLD_PATH"
else
    echo "# Warning: $MYSQLD_PATH not found" >&2
fi

# Collect dependencies from launcher (recursively)
echo "# Analyzing $LAUNCHER_PATH (recursive)..." >&2
if [ -f "$LAUNCHER_PATH" ]; then
    add_dep_recursive "$LAUNCHER_PATH"
else
    echo "# Warning: $LAUNCHER_PATH not found" >&2
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

# Add MySQL plugins directory (ALL plugins) - recursively
# This includes group_replication.so for Group Replication support
echo "# Adding MySQL plugins (recursive)..." >&2
# Check multiple possible MySQL plugin directories
for MYSQL_PLUGIN_DIR in /usr/lib/mysql/plugin /usr/lib/x86_64-linux-gnu/mysql/plugin; do
    if [ -d "$MYSQL_PLUGIN_DIR" ]; then
        echo "# Found MySQL plugin directory: $MYSQL_PLUGIN_DIR" >&2
        # Add ALL plugins in the directory and their dependencies
        # Including: group_replication.so, mysql_native_password.so, caching_sha2_password.so, etc.
        for plugin in "$MYSQL_PLUGIN_DIR"/*.so; do
            if [ -f "$plugin" ]; then
                add_dep_recursive "$plugin"
            fi
        done
    fi
done

# Add MySQL components (component_*.so files are loaded dynamically)
echo "# Adding MySQL components..." >&2
for MYSQL_PLUGIN_DIR in /usr/lib/mysql/plugin /usr/lib/x86_64-linux-gnu/mysql/plugin; do
    if [ -d "$MYSQL_PLUGIN_DIR" ]; then
        for component in "$MYSQL_PLUGIN_DIR"/component_*.so; do
            if [ -f "$component" ]; then
                add_dep_recursive "$component"
            fi
        done
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
    echo "  # MySQL plugin directories (entire directories for dynamically loaded plugins/components)"
    echo '  "file:/usr/lib/mysql/plugin/",'
    if [ -d "/usr/lib/x86_64-linux-gnu/mysql/plugin" ]; then
        echo '  "file:/usr/lib/x86_64-linux-gnu/mysql/plugin/",'
    fi
    echo ""
    echo "  # MySQL support files (charsets, error messages, etc.)"
    echo '  "file:/usr/share/mysql/",'
    echo ""
    echo "  # Pre-initialized MySQL data directory (template for first boot)"
    echo "  # This is copied to the encrypted partition at runtime"
    echo '  "file:/app/mysql-init-data/",'
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
