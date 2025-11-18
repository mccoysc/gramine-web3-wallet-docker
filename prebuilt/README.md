# Prebuilt Binaries Cache

This directory contains pre-compiled binaries for Gramine and its dependencies to speed up Docker builds.

## Structure

```
prebuilt/
  versions.json                    # Version manifest
  linux-x86_64/
    gramine/
      gramine-<commit>.tar.zst     # Compressed Gramine binaries
      gramine-<commit>.sha256      # Checksum
    openssl/
      openssl-<version>.tar.zst    # Compressed OpenSSL binaries
      openssl-<version>.sha256     # Checksum
    nodejs/
      node-<version>.tar.zst       # Compressed Node.js binaries
      node-<version>.sha256        # Checksum
```

## How It Works

1. **GitHub Actions** checks the latest versions of Gramine (commit SHA), Node.js (latest stable), and OpenSSL
2. Compares with `versions.json` to determine if recompilation is needed
3. If versions match, Docker build uses prebuilt binaries from this directory
4. If versions differ, GitHub Actions:
   - Compiles the changed components
   - Extracts binaries from the Docker image
   - Compresses them to `.tar.zst` format
   - Calculates SHA256 checksums
   - Updates `versions.json`
   - Commits and pushes back to the repository

## Git LFS

Large binary files (`.tar.zst`) are stored using Git LFS to keep the repository size manageable.

## Manual Updates

To manually rebuild and update prebuilt binaries, trigger the GitHub Actions workflow with the `force_rebuild` option.
