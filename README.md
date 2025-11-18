# Gramine Web3 Wallet Docker Image

**English** | [中文文档](README.zh-CN.md)

This repository is dedicated to building and publishing Docker images for Web3 wallet projects based on the Gramine project. Images are automatically built and pushed to GitHub Container Registry (GHCR).

## Features

- ✅ Built from [mccoysc/gramine](https://github.com/mccoysc/gramine)
- ✅ Intel SGX Trusted Execution Environment support
- ✅ Pre-installed Node.js and common Web3 development tools
- ✅ Automatic monitoring of Gramine repository updates and rebuild triggers
- ✅ Automatic build and push to GitHub Container Registry
- ✅ Multiple tagging strategies (latest, version, SHA, etc.)

## Architecture

### Automated Workflows

1. **Build and Push Image** (`.github/workflows/build-and-push.yml`)
   - Triggers:
     - Push to main/master branch
     - Create version tags (v*)
     - Manual trigger
   - Actions:
     - Build Docker image
     - Push to GitHub Container Registry
     - Generate multiple tags (latest, version, SHA, etc.)

2. **Monitor Gramine Repository** (`.github/workflows/monitor-gramine.yml`)
   - Triggers:
     - Automatic check every 6 hours
     - Manual trigger
   - Actions:
     - Check for updates in mccoysc/gramine repository
     - Automatically trigger image rebuild if updates found
     - Update `.gramine-version` file to record version

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
│       ├── build-and-push.yml      # Build and push image workflow
│       └── monitor-gramine.yml     # Monitor Gramine repository workflow
├── config/
│   └── pccs-default.json          # PCCS default configuration
├── scripts/
│   └── entrypoint.sh              # Container startup script
├── Dockerfile                      # Docker image definition
├── docker-compose.yml             # Docker Compose configuration
├── .gramine-version               # Record current Gramine version
└── README.md                      # This document
```

## Environment Variables

| Variable | Description | Default | Required |
|----------|-------------|---------|----------|
| `GRAMINE_SGX_MODE` | Enable SGX mode | `1` | No |
| `GRAMINE_DIRECT_MODE` | Enable direct mode | `0` | No |
| `NODE_ENV` | Node.js environment | `production` | No |
| `PCCS_API_KEY` | Intel PCCS API key | Empty | Recommended |

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

## Pre-installed Tools

The image includes the following pre-installed tools:

- **Gramine LibOS**: Built from mccoysc/gramine (DCAP mode supported)
- **Node.js**: v24.x
- **Python**: 3.10.x
- **SGX Services**:
  - aesmd (SGX Architectural Enclave Service Manager)
  - PCCS (Provisioning Certificate Caching Service)
  - QPL (Quote Provider Library)
- **Web3 Tools**:
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

If you need to force rebuild the image (even if Gramine repository has no updates):

1. Go to repository Actions page
2. Select "Monitor Gramine Repository" workflow
3. Click "Run workflow"
4. Check "Force rebuild" option
5. Run workflow

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

1. Check Actions logs
2. Verify Dockerfile syntax
3. Confirm dependencies are accessible

### Image Pull Failure

If unable to pull image:

1. Confirm logged in to GitHub Container Registry:
   ```bash
   echo $GITHUB_TOKEN | docker login ghcr.io -u USERNAME --password-stdin
   ```
2. Confirm image repository permissions are set correctly

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
- Email: hbzgzr@gmail.com

---

**Note**: This image is for development and testing purposes only. Conduct thorough security audits before production use.
