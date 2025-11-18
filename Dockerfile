# Gramine Web3 Wallet Docker Image
# Multi-stage build: Builder stage compiles Gramine, Runtime stage contains only installed binaries
# This Dockerfile creates a secure environment for running Web3 wallet applications
# using Gramine LibOS with Intel SGX support

#==============================================================================
# Builder Stage: Compile Gramine from source
#==============================================================================
FROM ubuntu:22.04 AS builder

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    curl \
    wget \
    git \
    ca-certificates \
    gnupg \
    python3 \
    python3-pip \
    autoconf \
    bison \
    gawk \
    nasm \
    ninja-build \
    pkg-config \
    libcurl4-openssl-dev \
    libprotobuf-c-dev \
    protobuf-c-compiler \
    protobuf-compiler \
    python3-cryptography \
    python3-protobuf \
    && rm -rf /var/lib/apt/lists/*

# Install Meson build system
RUN pip3 install meson tomli tomli-w

# Install Intel SGX development dependencies
RUN curl -fsSL https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key | gpg --dearmor -o /usr/share/keyrings/intel-sgx.gpg \
    && echo "deb [arch=amd64 signed-by=/usr/share/keyrings/intel-sgx.gpg] https://download.01.org/intel-sgx/sgx_repo/ubuntu jammy main" > /etc/apt/sources.list.d/intel-sgx.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    libsgx-dcap-ql-dev \
    libsgx-dcap-quote-verify-dev \
    libsgx-dcap-default-qpl-dev \
    libsgx-urts \
    libsgx-enclave-common-dev \
    && rm -rf /var/lib/apt/lists/*

# Accept build argument for Gramine version/branch/commit
ARG GRAMINE_REF=master

# Clone Gramine from the mccoysc/gramine repository
WORKDIR /opt
RUN git clone https://github.com/mccoysc/gramine.git gramine && \
    cd gramine && \
    git checkout "$GRAMINE_REF"

# Build entire Gramine project with SGX and DCAP support
WORKDIR /opt/gramine
RUN meson setup build/ \
    --buildtype=release \
    -Ddirect=enabled \
    -Dsgx=enabled \
    -Ddcap=enabled \
    -Dtests=disabled \
    && ninja -C build/

# Install Gramine to a staging directory (DESTDIR)
RUN DESTDIR=/opt/gramine-install ninja -C build/ install

#==============================================================================
# Runtime Stage: Clean image with only installed Gramine and runtime dependencies
#==============================================================================
FROM ubuntu:22.04

LABEL org.opencontainers.image.source=https://github.com/mccoysc/gramine-web3-wallet-docker
LABEL org.opencontainers.image.description="Gramine-based Web3 Wallet Docker Image with SGX support"
LABEL org.opencontainers.image.licenses=LGPL-3.0

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies only (no build tools)
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    wget \
    gnupg \
    python3 \
    python3-pip \
    python3-venv \
    libcurl4 \
    libprotobuf-c1 \
    && rm -rf /var/lib/apt/lists/*

# Install Intel SGX runtime dependencies and aesmd service
RUN curl -fsSL https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key | gpg --dearmor -o /usr/share/keyrings/intel-sgx.gpg \
    && echo "deb [arch=amd64 signed-by=/usr/share/keyrings/intel-sgx.gpg] https://download.01.org/intel-sgx/sgx_repo/ubuntu jammy main" > /etc/apt/sources.list.d/intel-sgx.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    libsgx-dcap-ql \
    libsgx-dcap-quote-verify \
    libsgx-dcap-default-qpl \
    libsgx-urts \
    libsgx-enclave-common \
    libsgx-aesm-service \
    libsgx-aesm-launch-plugin \
    libsgx-aesm-pce-plugin \
    libsgx-aesm-quote-ex-plugin \
    libsgx-aesm-ecdsa-plugin \
    && rm -rf /var/lib/apt/lists/*

# Copy installed Gramine from builder stage
COPY --from=builder /opt/gramine-install/ /

# Update dynamic linker cache to recognize Gramine libraries
RUN ldconfig

# Install Node.js (latest LTS version)
ARG NODE_MAJOR=20
RUN curl -fsSL https://deb.nodesource.com/setup_${NODE_MAJOR}.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# Install common Web3 libraries (optional, can be disabled with build arg)
ARG INSTALL_WEB3_TOOLS=true
RUN if [ "$INSTALL_WEB3_TOOLS" = "true" ]; then \
        npm install -g web3 ethers hardhat truffle ganache; \
    fi

# Create directories for wallet data and configurations
RUN mkdir -p /app/wallet /app/manifests /app/keys /var/run/aesmd

# Copy entrypoint script
COPY scripts/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# Set environment variables
ENV PATH="/usr/local/bin:${PATH}"
ENV LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH}"
ENV GRAMINE_DIRECT_MODE=0
ENV GRAMINE_SGX_MODE=1

# Set working directory
WORKDIR /app

# Expose common Web3 ports
EXPOSE 8545 8546 30303

# Health check to verify aesmd is running
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD test -S /var/run/aesmd/aesm.socket || exit 1

# Set entrypoint
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
