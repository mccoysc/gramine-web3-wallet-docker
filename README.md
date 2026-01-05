# Gramine Web3 Wallet Docker Image

**English** | [中文文档](README.zh-CN.md)

This repository is dedicated to building and publishing Docker images for Web3 wallet projects based on the Gramine project. Images are automatically built and pushed to GitHub Container Registry (GHCR).

## Features

- ✅ Built from [mccoysc/gramine](https://github.com/mccoysc/gramine) with DCAP mode enabled
- ✅ Intel SGX Trusted Execution Environment support
- ✅ Pre-installed Node.js (latest LTS ≤v22.x, currently v22.21.1), OpenSSL (latest stable, currently 3.6.0), and common Web3 development tools
- ✅ Intelligent build system with GitHub Releases caching to avoid unnecessary recompilation
- ✅ Automatic version detection for Gramine, OpenSSL, and Node.js
- ✅ Multi-job workflow architecture for efficient resource utilization
- ✅ Automatic Dockerfile change detection to trigger image rebuilds
- ✅ Automatic build and push to GitHub Container Registry
- ✅ Multiple tagging strategies (latest, version, SHA, etc.)

## Architecture

### Automated Workflows

The repository uses an intelligent multi-job workflow system that optimizes build times and resource usage through GitHub Releases caching.

#### Main Workflow: Build and Push (`.github/workflows/build-and-push.yml`)

This workflow implements a sophisticated build pipeline with the following jobs:

**1. Version Detection (`check-versions`)**
   - Automatically detects latest versions:
     - **Gramine**: Latest commit SHA from mccoysc/gramine repository
     - **OpenSSL**: Latest stable release (semantic version parsing, excludes pre-releases)
     - **Node.js**: Latest LTS version compatible with GCC 11 (v22.x, excludes v24+ which requires GCC 13+)
   - Checks GitHub Releases for existing prebuilt binaries
   - Detects Dockerfile and related file changes (Dockerfile, scripts/, config/, workflow files)
   - Determines which components need rebuilding based on:
     - Version changes
     - Missing prebuilt binaries in GitHub Releases
     - Dockerfile modifications
   - Runs on: push, pull_request, workflow_dispatch, schedule (every 6 hours)

**2. Component Compilation Jobs**

Each component builds in an independent GitHub Actions environment to avoid resource exhaustion:

- **`build-gramine`**: Compiles Gramine with DCAP support
  - Only runs if Gramine version changed AND prebuilt doesn't exist in Releases
  - Uploads compiled binary to GitHub Releases (tag: `prebuilt-gramine-{SHA8}`)
  - Artifact size: ~10MB

- **`build-openssl`**: Compiles OpenSSL from source
  - Only runs if OpenSSL version changed AND prebuilt doesn't exist in Releases
  - Uploads compiled binary to GitHub Releases (tag: `prebuilt-openssl-{VERSION}`)
  - Artifact size: ~8MB
  - Runs in parallel with `build-gramine`

- **`build-node`**: Compiles Node.js with custom OpenSSL
  - Only runs if Node.js version changed AND prebuilt doesn't exist in Releases
  - Depends on `build-openssl` (uses prebuilt OpenSSL for linking)
  - Uploads compiled binary to GitHub Releases (tag: `prebuilt-{NODE_VERSION}`)
  - Artifact size: ~45MB

**3. Docker Image Build (`build-image`)**
   - Triggers when:
     - Dockerfile or related files changed, OR
     - Any dependency (Gramine/OpenSSL/Node.js) was rebuilt
   - Downloads prebuilt binaries from GitHub Releases
   - Falls back to source compilation if prebuilt unavailable
   - Pushes to GitHub Container Registry with multiple tags
   - Only runs after all dependency jobs complete (success or skipped)

**4. Version Update (`update-versions`)**
   - Updates VERSION files in prebuilt/ directory
   - Commits changes to repository
   - Only runs on push to main/master (not on PRs)

### Key Optimizations

1. **GitHub Releases Caching**: Prebuilt binaries are stored in GitHub Releases, avoiding unnecessary recompilation across workflow runs
2. **Intelligent Skip Logic**: Components are only rebuilt when versions change AND prebuilts don't exist
3. **Independent Environments**: Each compilation job runs in its own GitHub Actions runner to prevent resource exhaustion
4. **Parallel Execution**: OpenSSL and Gramine compile in parallel; Node.js waits for OpenSSL
5. **Dockerfile Change Detection**: Image rebuilds automatically when Dockerfile changes, even if dependencies haven't changed

## Usage

### Pull Image

```bash
# Pull latest version
docker pull ghcr.io/mccoysc/gramine-web3-wallet-docker:latest

# Pull specific version
docker pull ghcr.io/mccoysc/gramine-web3-wallet-docker:v1.0.0
```

### Run Container

#### Using Docker Command

```bash
# Basic run
docker run -it ghcr.io/mccoysc/gramine-web3-wallet-docker:latest

# Enable SGX support (requires SGX hardware)
docker run -it \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -v $(pwd)/wallet:/app/wallet \
  -v $(pwd)/keys:/app/keys \
  -p 8545:8545 \
  ghcr.io/mccoysc/gramine-web3-wallet-docker:latest
```

#### Using Docker Compose

```bash
# Start services
docker-compose up -d

# View logs
docker-compose logs -f

# Enter container
docker-compose exec gramine-web3-wallet bash

# Stop services
docker-compose down
```

### Local Build

```bash
# Clone repository
git clone https://github.com/mccoysc/gramine-web3-wallet-docker.git
cd gramine-web3-wallet-docker

# Build image
docker build -t gramine-web3-wallet:local .

# Run locally built image
docker run -it gramine-web3-wallet:local
```

## Directory Structure

```
gramine-web3-wallet-docker/
├── .github/
│   └── workflows/
│       └── build-and-push.yml      # Main build workflow with intelligent caching
├── config/
│   └── pccs-default.json          # PCCS default configuration
├── scripts/
│   └── entrypoint.sh              # Container startup script
├── prebuilt/                       # Prebuilt binaries metadata (created by workflow)
│   ├── gramine/
│   │   └── VERSION                # Current Gramine SHA (maintained by update-versions job)
│   ├── openssl/
│   │   └── VERSION                # Current OpenSSL version (maintained by update-versions job)
│   └── nodejs/
│       └── VERSION                # Current Node.js version (maintained by update-versions job)
├── Dockerfile                      # Docker image definition
├── docker-compose.yml             # Docker Compose configuration
└── README.md                      # This document
```

## Environment Variables

### Runtime Environment Variables

| Variable | Description | Default | Required |
|----------|-------------|---------|----------|
| `GRAMINE_SGX_MODE` | Enable SGX mode | `1` | No |
| `GRAMINE_DIRECT_MODE` | Enable direct mode | `0` | No |
| `NODE_ENV` | Node.js environment | `production` | No |
| `PCCS_API_KEY` | Intel PCCS API key for DCAP attestation | Empty | Recommended for DCAP |

### RA-TLS Configuration

This Docker image includes the Gramine RA-TLS libraries for remote attestation. To enable RA-TLS in your applications, use the automatic injection feature provided by Gramine's manifest processing.

**Recommended: Automatic Injection via GRAMINE_LD_PRELOAD**

The Gramine manifest processor supports automatic RA-TLS library injection. Set the `GRAMINE_LD_PRELOAD` environment variable before running `gramine-manifest`:

```bash
# Find the library path (common locations)
# /usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so
# /usr/local/lib/libratls-quote-verify.so
# Or use: ldconfig -p | grep libratls-quote-verify.so

export GRAMINE_LD_PRELOAD="file:/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so"
gramine-manifest my-app.manifest.template my-app.manifest
```

This automatically:
- Adds the library to `sgx.trusted_files`
- Sets `loader.env.LD_PRELOAD` to the library path
- Sets `loader.env.RA_TLS_ENABLE_VERIFY=1`
- Creates necessary `fs.mounts` entries

**Manual Configuration**

Alternatively, configure LD_PRELOAD manually in your manifest template:

```toml
sgx.remote_attestation = "dcap"

loader.env.LD_PRELOAD = "/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so"
loader.env.RA_TLS_ENABLE_VERIFY = "1"

sgx.trusted_files = [
    "file:/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so",
]
```

**RA-TLS Environment Variables**

For complete documentation of RA-TLS environment variables (including `RA_TLS_ENABLE_VERIFY`, `RA_TLS_REQUIRE_PEER_CERT`, `RA_TLS_MRSIGNER`, `RA_TLS_MRENCLAVE`, etc.), see the [Gramine RA-TLS documentation](https://github.com/mccoysc/gramine#ra-tls-quick-start).

### API Key Configuration

#### PCCS API Key

PCCS (Provisioning Certificate Caching Service) is used for SGX DCAP remote attestation. If you need to fetch the latest SGX certificates from Intel, you need to configure an API key.

**Obtain API Key**:
1. Visit [Intel PCS Portal](https://api.portal.trustedservices.intel.com/)
2. Register and create a subscription
3. Get API key (Primary Key or Secondary Key)

**Configuration Methods**:

Using Docker command line:
```bash
docker run -it \
  -e PCCS_API_KEY="your-api-key-here" \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  ghcr.io/mccoysc/gramine-web3-wallet-docker:latest
```

Using Docker Compose, add to `docker-compose.yml`:
```yaml
services:
  gramine-web3-wallet:
    image: ghcr.io/mccoysc/gramine-web3-wallet-docker:latest
    environment:
      - PCCS_API_KEY=${PCCS_API_KEY}
    # ... other configurations
```

Then create `.env` file:
```bash
PCCS_API_KEY=your-api-key-here
```

**Important Notes**:
- Keep API keys confidential, do not commit to code repository
- If API key is not configured, PCCS will still start but cannot fetch latest certificates from Intel
- For development and testing environments, API key configuration is optional

### SGX Services Configuration

The Docker image includes three critical SGX services for DCAP remote attestation:

#### 1. aesmd (Architectural Enclave Service Manager)

**Purpose**: Manages SGX architectural enclaves (Quoting Enclave, Provisioning Enclave, etc.) required for attestation.

**Installation Details**:
- Package: `sgx-aesm-service` (not `libsgx-aesm-service`)
- Plugins: `libsgx-aesm-launch-plugin`, `libsgx-aesm-pce-plugin`, `libsgx-aesm-epid-plugin`, `libsgx-aesm-quote-ex-plugin`, `libsgx-aesm-ecdsa-plugin`
- Binary location: `/opt/intel/sgx-aesm-service/aesm/aesm_service`
- Socket: `/var/run/aesmd/aesm.socket`

**Startup**: The entrypoint script automatically starts aesmd in the background without systemd:
```bash
LD_LIBRARY_PATH=/opt/intel/sgx-aesm-service/aesm /opt/intel/sgx-aesm-service/aesm/aesm_service &
```

**Verification**: Check if aesmd is running:
```bash
# Inside container
test -S /var/run/aesmd/aesm.socket && echo "aesmd is running" || echo "aesmd not running"
```

#### 2. PCCS (Provisioning Certificate Caching Service)

**Purpose**: Caches SGX provisioning certificates from Intel PCS (Provisioning Certificate Service) to reduce network latency and provide offline attestation support.

**Installation Details**:
- Package: `sgx-dcap-pccs`
- Installation path: `/opt/intel/sgx-dcap-pccs`
- Configuration: `/opt/intel/sgx-dcap-pccs/config/default.json`
- Ports: HTTP 8080, HTTPS 8081

**Startup**: Conditionally started by entrypoint script when `PCCS_API_KEY` is set:
```bash
# Only starts if PCCS_API_KEY environment variable is provided
if [ -n "$PCCS_API_KEY" ]; then
    cd /opt/intel/sgx-dcap-pccs
    node pccs_server.js &
fi
```

**Configuration**: The entrypoint script automatically injects the API key from the `PCCS_API_KEY` environment variable into the PCCS configuration file.

#### 3. QPL (Quote Provider Library) Configuration

**Purpose**: Configures how Gramine applications fetch attestation collateral (certificates, CRLs, TCB info).

**Configuration File**: `/etc/sgx_default_qcnl.conf`

**Behavior**:
- **When PCCS_API_KEY is set**: QPL is configured to use the local PCCS instance:
  ```json
  {
    "pccs_url": "https://127.0.0.1:8081/sgx/certification/v4/",
    "use_secure_cert": false,
    "collateral_service": "https://api.trustedservices.intel.com/sgx/certification/v4/",
    "retry_times": 6,
    "retry_delay": 10
  }
  ```
  This allows aesmd to fetch attestation collateral from the local PCCS, which in turn uses the API key to fetch from Intel PCS.

- **When PCCS_API_KEY is not set**: QPL is configured to use Intel PCS directly:
  ```json
  {
    "pccs_url": "https://api.trustedservices.intel.com/sgx/certification/v4/",
    "use_secure_cert": true,
    "collateral_service": "https://api.trustedservices.intel.com/sgx/certification/v4/"
  }
  ```
  This requires direct network access to Intel's servers during attestation.

**Note**: The aesmd service does not directly use the PCCS_API_KEY. Instead, aesmd uses QPL to connect to PCCS, and PCCS uses the API key to fetch certificates from Intel PCS.

## Pre-installed Tools

The image includes the following pre-installed tools:

- **Gramine LibOS**: Built from mccoysc/gramine with DCAP mode enabled
  - Latest version automatically detected and compiled
  - Installed to `/opt/gramine-install`
- **OpenSSL**: Latest stable (currently 3.6.0)
  - Prebuilt: Compiled from source with optimizations
  - Fallback: Uses system OpenSSL from Ubuntu 22.04
  - Installed to `/opt/openssl-install` (prebuilt path)
  - Library path: `/opt/openssl-install/lib64` (x86_64)
- **Node.js**: Latest LTS capped at v22.x for GCC 11 compatibility (currently v22.21.1)
  - Prebuilt: Compiled with custom OpenSSL 3.6.0
  - Fallback: Installed from NodeSource repository (v22.x)
  - Compatible with Ubuntu 22.04 GCC 11
  - Installed to `/opt/node-install` (prebuilt path)
  - Note: v24+ LTS requires GCC 13+ and is excluded by compatibility cap
- **Python**: 3.10.x
- **SGX Services**:
  - aesmd (SGX Architectural Enclave Service Manager)
  - PCCS (Provisioning Certificate Caching Service)
  - QPL (Quote Provider Library)
- **Web3 Tools** (installed globally via npm, controlled by `INSTALL_WEB3_TOOLS=true` build arg):
  - web3.js
  - ethers.js
  - Hardhat
  - Truffle
  - Ganache

## Development Guide

### Modify Dockerfile

1. Edit `Dockerfile`
2. Commit changes to repository
3. GitHub Actions will automatically build new image

### Manual Trigger Build

1. Go to repository Actions page
2. Select "Build and Push Docker Image" workflow
3. Click "Run workflow"
4. Select branch and run

### Force Rebuild

If you need to force rebuild all components (even if versions haven't changed):

1. Go to repository Actions page
2. Select "Build and Push Docker Image" workflow
3. Click "Run workflow"
4. Check "Force rebuild all components" option
5. Run workflow

This will recompile Gramine, OpenSSL, and Node.js from source and rebuild the Docker image, regardless of whether prebuilt binaries exist in GitHub Releases.

## Image Tagging Strategy

GitHub Actions automatically generates the following tags:

- `latest`: Latest main/master branch build
- `v1.0.0`: Version tag (if git tag is pushed)
- `v1.0`: Major and minor version
- `v1`: Major version
- `main-abc1234`: Branch name-commit SHA
- `pr-123`: Pull Request number

## Security Considerations

1. **SGX Device Access**: Container needs access to `/dev/sgx_enclave` and `/dev/sgx_provision` devices
2. **Key Management**: Recommend storing private keys in encrypted volumes, do not commit to repository
3. **Network Security**: Configure appropriate firewall rules in production environments
4. **Permission Control**: Run containers with least privilege principle

## Troubleshooting

### SGX Device Not Found

If you see "SGX device not found" warning:

1. Confirm host supports Intel SGX
2. Confirm SGX driver is installed
3. Confirm Docker runtime has permission to access SGX devices

### Build Failure

If GitHub Actions build fails:

1. Check Actions logs for specific job failures
2. Verify Dockerfile syntax
3. Confirm dependencies are accessible
4. Check if GitHub Releases download failed (network issues)
5. For compilation failures, check GCC version compatibility

Common issues:
- **OpenSSL download fails**: Check if Release `prebuilt-openssl-{VERSION}` exists with asset `openssl-install-openssl-{VERSION}.tar.gz` (e.g., `prebuilt-openssl-3.6.0` with `openssl-install-openssl-3.6.0.tar.gz`)
- **Node.js compilation fails**: Verify OpenSSL was built successfully and is available
- **Gramine compilation fails**: Check if mccoysc/gramine repository is accessible
- **VERSION files missing**: These are created by the update-versions job after the first successful run on main branch

### Image Pull Failure

If unable to pull image:

1. Confirm logged in to GitHub Container Registry:
   ```bash
   echo $GITHUB_TOKEN | docker login ghcr.io -u USERNAME --password-stdin
   ```
2. Confirm image repository permissions are set correctly

### OpenSSL Library Not Found

If you see "OpenSSL version not found" errors in the container:

1. Verify `LD_LIBRARY_PATH` includes `/opt/openssl-install/lib64`
2. Check if OpenSSL was properly extracted: `ls -la /opt/openssl-install`
3. Test OpenSSL manually: `LD_LIBRARY_PATH=/opt/openssl-install/lib64 /opt/openssl-install/bin/openssl version`

## Contributing

Issues and Pull Requests are welcome!

1. Fork this repository
2. Create feature branch (`git checkout -b feature/amazing-feature`)
3. Commit changes (`git commit -m 'Add some amazing feature'`)
4. Push to branch (`git push origin feature/amazing-feature`)
5. Create Pull Request

## License

This project uses the LGPL-3.0 license, consistent with the Gramine project.

## Related Links

- [Gramine Official Documentation](https://gramine.readthedocs.io/)
- [mccoysc/gramine Repository](https://github.com/mccoysc/gramine)
- [Intel SGX Documentation](https://www.intel.com/content/www/us/en/developer/tools/software-guard-extensions/overview.html)
- [GitHub Container Registry Documentation](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry)

## Contact

- GitHub: [@mccoysc](https://github.com/mccoysc)

---

**Note**: This image is for development and testing purposes only. Conduct thorough security audits before production use.
