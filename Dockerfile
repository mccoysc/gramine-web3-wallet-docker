# Gramine Web3 Wallet Docker Image
# Multi-stage build with conditional prebuilt vs source compilation
# This Dockerfile creates a secure environment for running Web3 wallet applications
# using Gramine LibOS with Intel SGX support

#==============================================================================
# Global Build Arguments: Define before any FROM to enable variable expansion
#==============================================================================
ARG GRAMINE_SRC=gramine-builder
ARG OPENSSL_SRC=openssl-builder
ARG NODEJS_SRC=nodejs-builder
ARG GRAMINE_TARBALL
ARG OPENSSL_TARBALL
ARG NODEJS_TARBALL
ARG ARCH=linux-x86_64

#==============================================================================
# Prebuilt Import Stages: Use cached binaries from repository
#==============================================================================

# Gramine prebuilt import stage
FROM ubuntu:22.04 AS gramine-prebuilt
ARG GRAMINE_TARBALL
ARG ARCH
RUN apt-get update && apt-get install -y --no-install-recommends zstd && rm -rf /var/lib/apt/lists/*
COPY ${GRAMINE_TARBALL} /tmp/gramine.tar.zst
RUN mkdir -p /opt/gramine-install && \
    tar -I zstd -xf /tmp/gramine.tar.zst -C / && \
    rm /tmp/gramine.tar.zst

# OpenSSL prebuilt import stage
FROM ubuntu:22.04 AS openssl-prebuilt
ARG OPENSSL_TARBALL
ARG ARCH
RUN apt-get update && apt-get install -y --no-install-recommends zstd && rm -rf /var/lib/apt/lists/*
COPY ${OPENSSL_TARBALL} /tmp/openssl.tar.zst
RUN mkdir -p /opt/node-ssl && \
    tar -I zstd -xf /tmp/openssl.tar.zst -C / && \
    rm /tmp/openssl.tar.zst

# Node.js prebuilt import stage
FROM ubuntu:22.04 AS nodejs-prebuilt
ARG NODEJS_TARBALL
ARG ARCH
RUN apt-get update && apt-get install -y --no-install-recommends zstd && rm -rf /var/lib/apt/lists/*
COPY ${NODEJS_TARBALL} /tmp/nodejs.tar.zst
RUN mkdir -p /opt/nodejs && \
    tar -I zstd -xf /tmp/nodejs.tar.zst -C / && \
    rm /tmp/nodejs.tar.zst

#==============================================================================
# Builder Stage: Compile Gramine from source
#==============================================================================
FROM ubuntu:22.04 AS gramine-builder

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies (following Gramine documentation)
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    autoconf \
    bison \
    gawk \
    meson \
    nasm \
    pkg-config \
    python3 \
    python3-click \
    python3-jinja2 \
    python3-pyelftools \
    python3-tomli \
    python3-tomli-w \
    python3-voluptuous \
    wget \
    curl \
    git \
    ca-certificates \
    gnupg \
    cmake \
    libprotobuf-c-dev \
    protobuf-c-compiler \
    protobuf-compiler \
    python3-cryptography \
    python3-pip \
    python3-protobuf \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Intel SGX development dependencies (following Gramine documentation)
RUN curl -fsSLo /etc/apt/keyrings/intel-sgx-deb.asc https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key \
    && echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/intel-sgx-deb.asc] https://download.01.org/intel-sgx/sgx_repo/ubuntu jammy main" > /etc/apt/sources.list.d/intel-sgx.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    libsgx-dcap-quote-verify-dev \
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
# OpenSSL Builder Stage: Compile OpenSSL from source
#==============================================================================
FROM ubuntu:22.04 AS openssl-builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies for OpenSSL
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-12 \
    g++-12 \
    wget \
    ca-certificates \
    perl \
    && rm -rf /var/lib/apt/lists/*

# Set g++-12 as the default compiler
ENV CC=gcc-12
ENV CXX=g++-12

# Build OpenSSL from source to private location
# Force --libdir=lib to avoid lib64 and ensure linker can find libraries
ARG OPENSSL_VERSION=3.0.15
WORKDIR /tmp/openssl-build
RUN wget -q https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz \
    && tar -xzf openssl-${OPENSSL_VERSION}.tar.gz \
    && cd openssl-${OPENSSL_VERSION} \
    && ./Configure linux-x86_64 --prefix=/opt/node-ssl --openssldir=/opt/node-ssl/ssl --libdir=lib shared \
    && make -j"$(nproc)" \
    && make install_sw install_dev install_ssldirs \
    && cd / && rm -rf /tmp/openssl-build

#==============================================================================
# OpenSSL Selector Stage: Choose between prebuilt and builder
#==============================================================================
# This selector must be defined before nodejs-builder since nodejs-builder uses it
ARG OPENSSL_SRC
FROM ${OPENSSL_SRC} AS openssl-selected

#==============================================================================
# Node.js Builder Stage: Compile Node.js from source with dynamic OpenSSL linking
#==============================================================================
FROM ubuntu:22.04 AS nodejs-builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies for Node.js
# Use g++-12 for better C++20 support required by Node.js v25+ ada dependency
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-12 \
    g++-12 \
    curl \
    wget \
    ca-certificates \
    python3 \
    python3-pip \
    pkg-config \
    libatomic1 \
    xz-utils \
    jq \
    && rm -rf /var/lib/apt/lists/*

# Set g++-12 as the default compiler for Node.js build
ENV CC=gcc-12
ENV CXX=g++-12
ENV LDFLAGS="-L/opt/node-ssl/lib"
ENV CPPFLAGS="-I/opt/node-ssl/include"
ENV PKG_CONFIG_PATH="/opt/node-ssl/lib/pkgconfig"

# Copy OpenSSL from selected stage
# This allows Node.js to link against OpenSSL during compilation
COPY --from=openssl-selected /opt/node-ssl /opt/node-ssl

# Download and build Node.js from source with shared OpenSSL
ARG NODE_VERSION
WORKDIR /tmp/node-build
RUN if [ -z "$NODE_VERSION" ]; then \
        NODE_VERSION=$(curl -s https://nodejs.org/dist/index.json | jq -r '[.[] | select(.version | test("^v[0-9]+\\.[0-9]+\\.[0-9]+$"))] | .[0].version'); \
    fi \
    && echo "Building Node.js version: $NODE_VERSION" \
    && wget -q https://nodejs.org/dist/${NODE_VERSION}/node-${NODE_VERSION}.tar.gz \
    && tar -xzf node-${NODE_VERSION}.tar.gz \
    && cd node-${NODE_VERSION} \
    && ./configure \
        --prefix=/opt/nodejs \
        --shared-openssl \
        --shared-openssl-includes=/opt/node-ssl/include \
        --shared-openssl-libpath=/opt/node-ssl/lib \
        --openssl-use-def-ca-store \
    && make -j"$(nproc)" \
    && make install \
    && cd / && rm -rf /tmp/node-build

# Verify Node.js is dynamically linked to OpenSSL
RUN export LD_LIBRARY_PATH="/opt/node-ssl/lib:$LD_LIBRARY_PATH" \
    && /opt/nodejs/bin/node -v \
    && /opt/nodejs/bin/npm -v \
    && ldd /opt/nodejs/bin/node | grep -E 'libssl|libcrypto' || (echo "ERROR: Node.js is not dynamically linked to OpenSSL" && exit 1)

#==============================================================================
# Remaining Selector Stages: Choose between prebuilt and builder stages
#==============================================================================
# These stages use ARGs from global scope to select the appropriate source stage
# This is the Docker-supported way to achieve conditional COPY operations
# Note: openssl-selected is defined earlier (before nodejs-builder) since nodejs-builder uses it

ARG GRAMINE_SRC
FROM ${GRAMINE_SRC} AS gramine-selected

ARG NODEJS_SRC
FROM ${NODEJS_SRC} AS nodejs-selected

#==============================================================================
# Runtime Stage: Clean image with only installed Gramine and runtime dependencies
#==============================================================================
FROM ubuntu:22.04

LABEL org.opencontainers.image.source=https://github.com/mccoysc/gramine-web3-wallet-docker
LABEL org.opencontainers.image.description="Gramine-based Web3 Wallet Docker Image with SGX support"
LABEL org.opencontainers.image.licenses=LGPL-3.0

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies (including Python deps for Gramine CLI tools and Node.js runtime deps)
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    wget \
    gnupg \
    python3 \
    python3-click \
    python3-jinja2 \
    python3-pyelftools \
    python3-tomli \
    python3-tomli-w \
    python3-voluptuous \
    python3-cryptography \
    python3-pip \
    python3-venv \
    libcurl4 \
    libprotobuf-c1 \
    libatomic1 \
    && rm -rf /var/lib/apt/lists/*

# Install Intel SGX runtime dependencies and aesmd service (following Gramine documentation)
RUN curl -fsSLo /etc/apt/keyrings/intel-sgx-deb.asc https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key \
    && echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/intel-sgx-deb.asc] https://download.01.org/intel-sgx/sgx_repo/ubuntu jammy main" > /etc/apt/sources.list.d/intel-sgx.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    libsgx-dcap-ql \
    libsgx-dcap-quote-verify \
    libsgx-dcap-default-qpl \
    libsgx-urts \
    libsgx-enclave-common \
    sgx-aesm-service \
    libsgx-aesm-launch-plugin \
    libsgx-aesm-pce-plugin \
    libsgx-aesm-epid-plugin \
    libsgx-aesm-quote-ex-plugin \
    libsgx-aesm-ecdsa-plugin \
    && rm -rf /var/lib/apt/lists/*

# Copy installed Gramine from selected stage
COPY --from=gramine-selected /opt/gramine-install/ /

# Update dynamic linker cache to recognize Gramine libraries
RUN ldconfig

# Install gramine-manifest wrapper to auto-inject RA-TLS library into LD_PRELOAD
# The wrapper renames the original gramine-manifest to gramine-manifest.real
# and installs a Python wrapper that post-processes generated manifests
COPY scripts/gramine-manifest-wrapper.py /usr/local/bin/gramine-manifest-wrapper.py
RUN chmod +x /usr/local/bin/gramine-manifest-wrapper.py && \
    if [ -f /usr/local/bin/gramine-manifest ]; then \
        mv /usr/local/bin/gramine-manifest /usr/local/bin/gramine-manifest.real && \
        ln -s /usr/local/bin/gramine-manifest-wrapper.py /usr/local/bin/gramine-manifest; \
    fi

# Copy compiled Node.js and private OpenSSL from selected stages
COPY --from=nodejs-selected /opt/nodejs /opt/nodejs
COPY --from=openssl-selected /opt/node-ssl /opt/node-ssl

# Create Node.js wrapper script to set up private OpenSSL environment
RUN echo '#!/usr/bin/env sh\n\
export LD_LIBRARY_PATH="/opt/node-ssl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n\
export SSL_CERT_FILE="${SSL_CERT_FILE:-/etc/ssl/certs/ca-certificates.crt}"\n\
exec /opt/nodejs/bin/node "$@"\n' > /usr/local/bin/node \
    && chmod +x /usr/local/bin/node

# Create npm and npx wrapper scripts
RUN echo '#!/usr/bin/env sh\n\
export LD_LIBRARY_PATH="/opt/node-ssl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n\
export SSL_CERT_FILE="${SSL_CERT_FILE:-/etc/ssl/certs/ca-certificates.crt}"\n\
exec /opt/nodejs/bin/npm "$@"\n' > /usr/local/bin/npm \
    && chmod +x /usr/local/bin/npm \
    && echo '#!/usr/bin/env sh\n\
export LD_LIBRARY_PATH="/opt/node-ssl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n\
export SSL_CERT_FILE="${SSL_CERT_FILE:-/etc/ssl/certs/ca-certificates.crt}"\n\
exec /opt/nodejs/bin/npx "$@"\n' > /usr/local/bin/npx \
    && chmod +x /usr/local/bin/npx

# Verify Node.js installation and dynamic OpenSSL linking
RUN node -v && npm -v \
    && echo "Node.js OpenSSL version:" && node -p "process.versions.openssl" \
    && echo "Verifying dynamic OpenSSL linking:" \
    && ldd /opt/nodejs/bin/node | grep -E 'libssl|libcrypto'

# Install PCCS by extracting .deb to temp directory (avoids systemd postinst issues)
RUN curl -fsSLo /etc/apt/keyrings/intel-sgx-deb.asc https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key \
    && echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/intel-sgx-deb.asc] https://download.01.org/intel-sgx/sgx_repo/ubuntu jammy main" > /etc/apt/sources.list.d/intel-sgx.list \
    && apt-get update \
    && cd /tmp \
    && apt-get download sgx-dcap-pccs \
    && dpkg-deb -x sgx-dcap-pccs_*.deb /tmp/pccs-extract \
    && mkdir -p /opt/intel \
    && cp -r /tmp/pccs-extract/opt/intel/sgx-dcap-pccs /opt/intel/ \
    && rm -rf /tmp/pccs-extract /tmp/sgx-dcap-pccs_*.deb \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Install PCCS dependencies (node_modules)
RUN cd /opt/intel/sgx-dcap-pccs \
    && if [ -f package-lock.json ]; then \
        npm ci --omit=dev; \
    else \
        npm install --omit=dev; \
    fi \
    && npm cache clean --force

# Copy PCCS default configuration with SQLite support
COPY config/pccs-default.json /opt/intel/sgx-dcap-pccs/config/default.json

# Create PCCS data directory for SQLite database
RUN mkdir -p /opt/intel/sgx-dcap-pccs/data

# Configure QPL to use local PCCS
RUN echo '{\n\
  "pccs_url": "https://localhost:8081/sgx/certification/v4/",\n\
  "use_secure_cert": false,\n\
  "collateral_service": "https://api.trustedservices.intel.com/sgx/certification/v4/",\n\
  "retry_times": 6,\n\
  "retry_delay": 10,\n\
  "local_pck_url": "",\n\
  "pck_cache_expire_hours": 168\n\
}' > /etc/sgx_default_qcnl.conf

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
ENV LD_LIBRARY_PATH="/usr/local/lib"
ENV GRAMINE_DIRECT_MODE=0
ENV GRAMINE_SGX_MODE=1

# Set working directory
WORKDIR /app

# Expose common Web3 ports and PCCS ports
EXPOSE 8545 8546 30303 8080 8081

# Health check to verify aesmd is running
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD test -S /var/run/aesmd/aesm.socket || exit 1

# Set entrypoint
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
