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
# 2. Handles symlinks by resolving to real paths and adding both
# 3. Handles script files by parsing shebang and exec calls
# 4. Adds Gramine runtime libraries and their dependencies (recursively)
# 5. Adds NSS libraries for DNS resolution (and their dependencies)
# 6. Adds RA-TLS library and its dependencies
# 7. Tracks required mounts for discovered paths
# 8. Outputs in Gramine manifest format
#
# IMPORTANT: This script recursively resolves ALL transitive dependencies
# to ensure no libraries are missing at runtime.

set -e

OUTPUT_FILE="${1:-}"

# Binaries to analyze
LAUNCHER_PATH="/usr/local/bin/mysql-client-ratls-launcher"

# Track visited executables to prevent infinite loops
VISITED_EXEC=""

# Track required mounts (directories that need to be mounted)
REQUIRED_MOUNTS=""

# Known mounts from manifest template (these don't need to be added)
KNOWN_MOUNTS="/lib /lib/x86_64-linux-gnu /usr/lib /usr/local/lib /usr/bin /usr/sbin /usr/local/bin /bin /sbin /app /etc /var/lib/mysql-client-ssl /tmp"

# Check if a path is covered by known mounts
is_path_covered() {
    local path="$1"
    for mount in $KNOWN_MOUNTS; do
        case "$path" in
            "$mount"/*|"$mount")
                return 0
                ;;
        esac
    done
    return 1
}

# Add a mount requirement if not already covered
add_mount_requirement() {
    local path="$1"
    [ -z "$path" ] && return
    
    # Get the directory containing the file
    local dir
    if [ -d "$path" ]; then
        dir="$path"
    else
        dir=$(dirname "$path")
    fi
    
    # Check if already covered by known mounts
    if is_path_covered "$dir"; then
        return
    fi
    
    # Check if already in required mounts
    case " $REQUIRED_MOUNTS " in
        *" $dir "*) return ;;
    esac
    
    REQUIRED_MOUNTS="$REQUIRED_MOUNTS $dir"
    echo "# Mount required for: $dir" >&2
}

# Check if file is an ELF binary
is_elf() {
    local file="$1"
    [ -f "$file" ] || return 1
    local magic
    magic=$(head -c 4 "$file" 2>/dev/null | od -An -tx1 | tr -d ' ')
    [ "$magic" = "7f454c46" ]
}

# Check if file is a script (has shebang)
is_script() {
    local file="$1"
    [ -f "$file" ] || return 1
    head -c 2 "$file" 2>/dev/null | grep -q '^#!'
}

# Get interpreter from shebang line
get_shebang_interpreter() {
    local file="$1"
    head -n 1 "$file" 2>/dev/null | sed -E 's/^#![[:space:]]*//; s/[[:space:]].*$//'
}

# Parse script for exec calls with absolute paths
# Handles patterns like: exec /path/to/binary
parse_exec_calls() {
    local file="$1"
    grep -E '^[[:space:]]*(exec|\.)[[:space:]]+/[^[:space:]]+' "$file" 2>/dev/null | \
        sed -E 's/^[[:space:]]*(exec|\.)[[:space:]]+([^[:space:]]+).*/\2/' | \
        sort -u
}

# Recursively add an executable and all its dependencies
# Handles: symlinks (resolves and adds both), scripts (parses shebang and exec calls), ELF binaries (uses ldd)
add_executable_recursive() {
    local path="$1"
    
    # Skip empty paths
    [ -z "$path" ] && return 0
    
    # Skip if already visited (prevents infinite loops)
    case " $VISITED_EXEC " in
        *" $path "*) return 0 ;;
    esac
    VISITED_EXEC="$VISITED_EXEC $path"
    
    # Skip if file doesn't exist
    if [ ! -e "$path" ]; then
        echo "# Warning: $path does not exist" >&2
        return 0
    fi
    
    # Handle symlinks: add the symlink itself, then recurse on the real path
    if [ -L "$path" ]; then
        echo "# Found symlink: $path" >&2
        
        # Add the symlink path to trusted files
        echo "$path" >> "$ALL_DEPS"
        add_mount_requirement "$path"
        
        # Resolve to real path and recurse
        local real_path
        real_path=$(readlink -f "$path" 2>/dev/null || true)
        if [ -n "$real_path" ] && [ "$real_path" != "$path" ] && [ -e "$real_path" ]; then
            echo "# Symlink resolves to: $real_path" >&2
            add_executable_recursive "$real_path"
        fi
        return 0
    fi
    
    # Handle script files: parse shebang and exec calls
    if is_script "$path"; then
        echo "# Found script: $path" >&2
        
        # Add the script itself to trusted files
        echo "$path" >> "$ALL_DEPS"
        add_mount_requirement "$path"
        
        # Get and process the interpreter from shebang
        local interp
        interp=$(get_shebang_interpreter "$path")
        if [ -n "$interp" ] && [ -x "$interp" ]; then
            echo "# Script interpreter: $interp" >&2
            add_executable_recursive "$interp"
        fi
        
        # Parse exec calls in the script
        local exec_targets
        exec_targets=$(parse_exec_calls "$path")
        if [ -n "$exec_targets" ]; then
            echo "$exec_targets" | while read -r target; do
                if [ -n "$target" ] && [ -e "$target" ]; then
                    echo "# Script exec target: $target" >&2
                    add_executable_recursive "$target"
                fi
            done
        fi
        return 0
    fi
    
    # Handle ELF binaries: use ldd to find dependencies
    if is_elf "$path"; then
        echo "# Found ELF binary: $path" >&2
        add_dep_recursive "$path"
        add_mount_requirement "$path"
        return 0
    fi
    
    # Unknown file type - just add it
    echo "# Adding file (unknown type): $path" >&2
    echo "$path" >> "$ALL_DEPS"
    add_mount_requirement "$path"
}

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
# Handles various ldd output formats:
#   libfoo.so.1 => /path/to/libfoo.so.1 (0x...)
#   /lib64/ld-linux-x86-64.so.2 (0x...)
extract_ldd_deps() {
    local binary="$1"
    if [ -f "$binary" ]; then
        ldd "$binary" 2>/dev/null | while read -r line; do
            # Handle "libname => /path (addr)" format
            if echo "$line" | grep -qE '=>.*\(0x'; then
                path=$(echo "$line" | sed -E 's/.*=> ([^ ]+) \(0x.*/\1/')
                if [ -f "$path" ]; then
                    echo "$path"
                fi
            # Handle "/path (addr)" format (e.g., ld-linux)
            elif echo "$line" | grep -qE '^\s*/.*\(0x'; then
                path=$(echo "$line" | sed -E 's/^\s*([^ ]+) \(0x.*/\1/')
                if [ -f "$path" ]; then
                    echo "$path"
                fi
            fi
        done | sort -u
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
    # Debug: Show ldd output for launcher to help diagnose missing dependencies
    echo "# DEBUG: ldd output for launcher:" >&2
    ldd "$LAUNCHER_PATH" 2>&1 | head -30 >&2
    echo "# DEBUG: End of ldd output" >&2
    add_executable_recursive "$LAUNCHER_PATH"
else
    echo "# Warning: $LAUNCHER_PATH not found" >&2
fi

# Analyze Node.js starting from the wrapper script at /usr/local/bin/node
# The add_executable_recursive function will:
# 1. Detect it's a script or symlink and follow it
# 2. Parse shebang and exec calls to find the actual Node.js binary
# 3. Recursively analyze the actual binary and its dependencies
# This ensures all paths (wrapper script, interpreter, actual binary) are in trusted_files
# NO pre-assumed paths - the script discovers whatever is actually used
NODE_WRAPPER_PATH="/usr/local/bin/node"
echo "# Analyzing Node.js wrapper: $NODE_WRAPPER_PATH (recursive)..." >&2
if [ -e "$NODE_WRAPPER_PATH" ]; then
    add_executable_recursive "$NODE_WRAPPER_PATH"
else
    echo "# ERROR: $NODE_WRAPPER_PATH not found!" >&2
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
# Use add_executable_recursive to properly handle symlinks (e.g., libratls.so -> libratls.so.1)
echo "# Adding RA-TLS libraries (recursive)..." >&2
for ratls_lib in /usr/local/lib/x86_64-linux-gnu/libratls*.so* /usr/local/lib/libratls*.so* /usr/lib/x86_64-linux-gnu/libratls*.so*; do
    if [ -e "$ratls_lib" ]; then
        add_executable_recursive "$ratls_lib"
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
# Check both /usr/lib and /lib directories as libcurl location varies by system
echo "# Adding libcurl and dependencies (recursive)..." >&2
for curl_lib in /usr/lib/x86_64-linux-gnu/libcurl*.so* /lib/x86_64-linux-gnu/libcurl*.so*; do
    if [ -f "$curl_lib" ]; then
        echo "# Found libcurl: $curl_lib" >&2
        add_dep_recursive "$curl_lib"
        # Debug: Verify libcurl was added to ALL_DEPS
        if grep -q "$curl_lib" "$ALL_DEPS" 2>/dev/null; then
            echo "# DEBUG: Successfully added $curl_lib to ALL_DEPS" >&2
        else
            echo "# DEBUG: WARNING - $curl_lib NOT found in ALL_DEPS after add_dep_recursive!" >&2
        fi
    fi
done

# Debug: Show ALL_DEPS content for libcurl
echo "# DEBUG: libcurl entries in ALL_DEPS file:" >&2
grep -i curl "$ALL_DEPS" 2>/dev/null >&2 || echo "# DEBUG: No libcurl in ALL_DEPS!" >&2

# Add libraries that libcurl may load via dlopen (SSL backends, resolvers, etc.)
# These are not discovered by ldd but are needed at runtime
echo "# Adding libcurl dlopen dependencies (SSL backends, etc.)..." >&2
for dlopen_lib in \
    /usr/lib/x86_64-linux-gnu/libssl*.so* \
    /usr/lib/x86_64-linux-gnu/libcrypto*.so* \
    /lib/x86_64-linux-gnu/libssl*.so* \
    /lib/x86_64-linux-gnu/libcrypto*.so* \
    /usr/lib/x86_64-linux-gnu/libnghttp2*.so* \
    /usr/lib/x86_64-linux-gnu/librtmp*.so* \
    /usr/lib/x86_64-linux-gnu/libssh*.so* \
    /usr/lib/x86_64-linux-gnu/libldap*.so* \
    /usr/lib/x86_64-linux-gnu/liblber*.so* \
    /usr/lib/x86_64-linux-gnu/libgssapi*.so* \
    /usr/lib/x86_64-linux-gnu/libkrb5*.so* \
    /usr/lib/x86_64-linux-gnu/libidn2*.so*; do
    if [ -f "$dlopen_lib" ]; then
        add_dep_recursive "$dlopen_lib"
    fi
done

# Add SGX DCAP libraries (for attestation) - recursively
echo "# Adding SGX DCAP libraries (recursive)..." >&2
for sgx_lib in /usr/lib/x86_64-linux-gnu/libsgx*.so* /usr/lib/x86_64-linux-gnu/libdcap*.so*; do
    if [ -f "$sgx_lib" ]; then
        add_dep_recursive "$sgx_lib"
    fi
done

# Note: OpenSSL libraries are discovered automatically via ldd from Node.js and other binaries
# No need to explicitly scan directories - the script discovers whatever is actually used

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
    echo "# Binaries analyzed: $LAUNCHER_PATH, /usr/local/bin/node (wrapper + actual binaries)"
    echo "# Note: Script files, symlinks, and their targets are all included"
    echo ""
    echo "sgx.trusted_files = ["
    echo "  # Gramine LibOS and runtime"
    echo '  "file:{{ gramine.libos }}",'
    echo '  "file:{{ gramine.runtimedir() }}/",'
    echo ""
    echo "  # Launcher binary (entrypoint)"
    echo '  "file:{{ entrypoint }}",'
    echo ""
    echo "  # Client script"
    echo '  "file:/app/mysql-client.js",'
    echo ""
    echo "  # System libraries and executables (auto-detected via ldd and script parsing)"
    echo "  # Includes: symlinks + real paths, script interpreters, exec targets"
    
    # Output each library/executable
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

# Generate auto-discovered mounts section
generate_auto_mounts() {
    if [ -n "$REQUIRED_MOUNTS" ]; then
        echo "  # Auto-discovered mounts (directories found during dependency analysis)"
        for mount in $REQUIRED_MOUNTS; do
            echo "  { path = \"$mount\", uri = \"file:$mount\" },"
        done
    fi
}

# Count dependencies
DEP_COUNT=$(echo "$UNIQUE_DEPS" | grep -c . || echo "0")
echo "# Found $DEP_COUNT unique library dependencies" >&2

# Debug: Show libcurl entries in final dependency list
echo "# DEBUG: libcurl entries in UNIQUE_DEPS:" >&2
echo "$UNIQUE_DEPS" | grep -i curl >&2 || echo "# DEBUG: No libcurl entries found in UNIQUE_DEPS!" >&2
echo "# DEBUG: End of libcurl entries" >&2

if [ -n "$OUTPUT_FILE" ]; then
    generate_output > "$OUTPUT_FILE"
    echo "# Output written to: $OUTPUT_FILE" >&2
    
    # Also output auto-discovered mounts to a separate file
    AUTO_MOUNTS_FILE="${OUTPUT_FILE%.toml}.auto_mounts"
    generate_auto_mounts > "$AUTO_MOUNTS_FILE"
    if [ -s "$AUTO_MOUNTS_FILE" ]; then
        echo "# Auto-discovered mounts written to: $AUTO_MOUNTS_FILE" >&2
    else
        echo "# No additional mounts needed (all paths covered by known mounts)" >&2
    fi
else
    generate_output
    echo ""
    echo "# Auto-discovered mounts:"
    generate_auto_mounts
fi

echo "# Done!" >&2
