# Gramine Web3 Wallet Docker Image
# This Dockerfile creates a secure environment for running Web3 wallet applications
# using Gramine LibOS with Intel SGX support

FROM ubuntu:22.04

LABEL org.opencontainers.image.source=https://github.com/mccoysc/gramine-web3-wallet-docker
LABEL org.opencontainers.image.description="Gramine-based Web3 Wallet Docker Image with SGX support"
LABEL org.opencontainers.image.licenses=LGPL-3.0

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install system dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    curl \
    wget \
    git \
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
    python3-pip \
    python3-protobuf \
    && rm -rf /var/lib/apt/lists/*

# Install Meson build system
RUN pip3 install meson tomli tomli-w

# Install Intel SGX dependencies (for SGX-enabled systems)
RUN curl -fsSL https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key | apt-key add - \
    && echo "deb [arch=amd64] https://download.01.org/intel-sgx/sgx_repo/ubuntu jammy main" > /etc/apt/sources.list.d/intel-sgx.list \
    && apt-get update \
    && apt-get install -y \
    libsgx-dcap-ql \
    libsgx-dcap-quote-verify-dev \
    libsgx-dcap-default-qpl \
    libsgx-urts \
    libsgx-enclave-common \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /opt

# Clone and build Gramine from the mccoysc/gramine repository
# This will be the base for running Web3 wallet applications
RUN git clone https://github.com/mccoysc/gramine.git gramine

WORKDIR /opt/gramine

# Build Gramine with SGX support
RUN meson setup build/ --buildtype=release -Ddirect=enabled -Dsgx=enabled -Ddcap=enabled \
    && ninja -C build/ \
    && ninja -C build/ install

# Add Gramine binaries to PATH
ENV PATH="/usr/local/bin:${PATH}"
ENV LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH}"

# Create directory for Web3 wallet applications
WORKDIR /app

# Install Node.js (commonly used for Web3 applications)
RUN curl -fsSL https://deb.nodesource.com/setup_20.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# Install common Web3 libraries
RUN npm install -g web3 ethers hardhat truffle ganache

# Create directories for wallet data and configurations
RUN mkdir -p /app/wallet /app/manifests /app/keys

# Copy entrypoint script (to be created)
COPY scripts/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# Set environment variables for Gramine
ENV GRAMINE_DIRECT_MODE=0
ENV GRAMINE_SGX_MODE=1

# Expose common Web3 ports
EXPOSE 8545 8546 30303

# Set entrypoint
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
