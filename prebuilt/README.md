# Prebuilt Binaries

This directory contains precompiled binaries to speed up Docker image builds.

## Structure

- `gramine/` - Precompiled Gramine binaries
- `openssl/` - Precompiled OpenSSL binaries
- `nodejs/` - Precompiled Node.js binaries

Each subdirectory contains:
- `VERSION` - Version information file
- Compiled binary archives (tar.zst format)

