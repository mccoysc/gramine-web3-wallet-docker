# Gramine Web3 Wallet Docker Image
# Multi-stage build: Builder stage compiles Gramine, Runtime stage contains only installed binaries
# This Dockerfile creates a secure environment for running Web3 wallet applications
# using Gramine LibOS with Intel SGX support

#==============================================================================
# Builder Stage: Compile Gramine from source OR use prebuilt
#==============================================================================
FROM ubuntu:22.04 AS builder

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Build arguments
ARG GRAMINE_OWNER=mccoysc
ARG GRAMINE_REF=master
ARG USE_PREBUILT=false

# Copy prebuilt directory (downloaded by workflow before build)
COPY prebuilt/ /tmp/prebuilt/

# Check if we should use prebuilt Gramine
RUN if [ "$USE_PREBUILT" = "true" ] && [ -f /tmp/prebuilt/gramine/gramine-install.tar.gz ]; then \
        echo "Using prebuilt Gramine"; \
        mkdir -p /opt; \
        cd /opt; \
        tar -xzf /tmp/prebuilt/gramine/gramine-install.tar.gz; \
        echo "Prebuilt Gramine extracted successfully"; \
    else \
        echo "Building Gramine from source"; \
        apt-get update && apt-get install -y --no-install-recommends \
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
            && rm -rf /var/lib/apt/lists/*; \
        curl -fsSLo /etc/apt/keyrings/intel-sgx-deb.asc https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key; \
        echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/intel-sgx-deb.asc] https://download.01.org/intel-sgx/sgx_repo/ubuntu jammy main" > /etc/apt/sources.list.d/intel-sgx.list; \
        apt-get update; \
        apt-get install -y --no-install-recommends \
            libsgx-dcap-quote-verify-dev \
            libsgx-urts \
            libsgx-enclave-common-dev; \
        rm -rf /var/lib/apt/lists/*; \
        cd /opt; \
        git clone https://github.com/${GRAMINE_OWNER}/gramine.git gramine; \
        cd gramine; \
        git checkout "$GRAMINE_REF"; \
        meson setup build/ \
            --buildtype=release \
            -Ddirect=enabled \
            -Dsgx=enabled \
            -Ddcap=enabled \
            -Dtests=disabled; \
        ninja -C build/; \
        DESTDIR=/opt/gramine-install ninja -C build/ install; \
        echo "Gramine built from source successfully"; \
    fi && \
    if [ ! -d /opt/gramine ]; then \
        mkdir -p /opt/gramine; \
        echo "Gramine source not available (using prebuilt)"; \
    fi

#==============================================================================
# Runtime Stage: Clean image with only installed Gramine and runtime dependencies
#==============================================================================
FROM ubuntu:22.04

# Labels are set by docker/metadata-action in the workflow for fork-friendly builds
LABEL org.opencontainers.image.description="Gramine-based Web3 Wallet Docker Image with SGX support"
LABEL org.opencontainers.image.licenses=LGPL-3.0

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies (including Python deps for Gramine CLI tools and build tools for testing)
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
    vim \
    lrzsz \
    build-essential \
    gcc \
    g++ \
    make \
    cmake \
    autoconf \
    automake \
    libtool \
    pkg-config \
    git \
    netcat-openbsd \
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

# Copy installed Gramine from builder stage
COPY --from=builder /opt/gramine-install/ /

# Copy Gramine source (including CI-Examples) from builder stage if it exists
# This is needed for RA-TLS testing. The directory only exists when building from source.
COPY --from=builder /opt/gramine /opt/gramine

# Re-declare build args for runtime stage (needed for fallback clone)
# GRAMINE_OWNER is dynamically set by workflow from github.repository_owner
ARG GRAMINE_OWNER
ARG GRAMINE_REF=master

# Copy RA-TLS example to /app for testing
# If using prebuilt Gramine, CI-Examples won't exist, so we clone only the single example directory
RUN set -eux && \
    mkdir -p /app && \
    if [ -d /opt/gramine/CI-Examples/ra-tls-mbedtls ]; then \
        echo "Using RA-TLS example from built-from-source Gramine"; \
        cp -r /opt/gramine/CI-Examples/ra-tls-mbedtls /app/ra-tls-mbedtls; \
    else \
        echo "CI-Examples not found in /opt/gramine; cloning ra-tls-mbedtls from ${GRAMINE_OWNER}/gramine@${GRAMINE_REF}"; \
        git init /tmp/gramine && \
        git -C /tmp/gramine remote add origin https://github.com/${GRAMINE_OWNER}/gramine.git && \
        git -C /tmp/gramine sparse-checkout init --cone && \
        git -C /tmp/gramine sparse-checkout set CI-Examples/ra-tls-mbedtls && \
        git -C /tmp/gramine fetch --depth=1 --filter=blob:none origin "${GRAMINE_REF}" && \
        git -C /tmp/gramine checkout FETCH_HEAD && \
        cp -r /tmp/gramine/CI-Examples/ra-tls-mbedtls /app/ && \
        rm -rf /tmp/gramine; \
    fi && \
    test -d /app/ra-tls-mbedtls && \
    echo "RA-TLS example available at /app/ra-tls-mbedtls"

# Update dynamic linker cache to recognize Gramine libraries
RUN ldconfig

# Install OpenSSL (prebuilt or system)
ARG USE_PREBUILT=false
COPY prebuilt/ /tmp/prebuilt/
RUN if [ "$USE_PREBUILT" = "true" ] && [ -f /tmp/prebuilt/openssl/openssl-install.tar.gz ]; then \
        echo "Using prebuilt OpenSSL"; \
        cd /opt; \
        tar -xzf /tmp/prebuilt/openssl/openssl-install.tar.gz; \
        echo "Prebuilt OpenSSL extracted successfully"; \
        OPENSSL_PREFIX=/opt/openssl-install; \
        if [ -d "$OPENSSL_PREFIX/lib64" ]; then \
            OPENSSL_LIB_DIR="$OPENSSL_PREFIX/lib64"; \
        else \
            OPENSSL_LIB_DIR="$OPENSSL_PREFIX/lib"; \
        fi; \
        echo "OpenSSL library directory: $OPENSSL_LIB_DIR"; \
        LD_LIBRARY_PATH="$OPENSSL_LIB_DIR" /opt/openssl-install/bin/openssl version; \
    else \
        echo "Using system OpenSSL"; \
    fi

# Install Node.js (prebuilt or from NodeSource)
ARG NODE_MAJOR=22
RUN if [ "$USE_PREBUILT" = "true" ] && [ -f /tmp/prebuilt/nodejs/node-install.tar.gz ]; then \
        echo "Using prebuilt Node.js"; \
        cd /opt; \
        tar -xzf /tmp/prebuilt/nodejs/node-install.tar.gz; \
        echo "Prebuilt Node.js extracted successfully"; \
        /opt/node-install/bin/node --version; \
    else \
        echo "Installing Node.js from NodeSource"; \
        curl -fsSL https://deb.nodesource.com/setup_${NODE_MAJOR}.x | bash -; \
        apt-get install -y nodejs; \
        rm -rf /var/lib/apt/lists/*; \
    fi

# Create wrapper scripts for openssl and node to limit OpenSSL library path scope
# This ensures only openssl CLI and Node.js use the custom OpenSSL, not other system binaries
RUN rm -f /usr/local/bin/node /usr/local/bin/npm /usr/local/bin/npx /usr/local/bin/openssl && \
    echo '#!/bin/sh' > /usr/local/bin/openssl && \
    echo 'if [ -x /opt/openssl-install/bin/openssl ]; then' >> /usr/local/bin/openssl && \
    echo '  export LD_LIBRARY_PATH="/opt/openssl-install/lib64:/opt/openssl-install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"' >> /usr/local/bin/openssl && \
    echo '  exec /opt/openssl-install/bin/openssl "$@"' >> /usr/local/bin/openssl && \
    echo 'else' >> /usr/local/bin/openssl && \
    echo '  exec /usr/bin/openssl "$@"' >> /usr/local/bin/openssl && \
    echo 'fi' >> /usr/local/bin/openssl && \
    chmod +x /usr/local/bin/openssl && \
    echo '#!/bin/sh' > /usr/local/bin/node && \
    echo 'if [ -d /opt/openssl-install ]; then' >> /usr/local/bin/node && \
    echo '  export LD_LIBRARY_PATH="/opt/openssl-install/lib64:/opt/openssl-install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"' >> /usr/local/bin/node && \
    echo 'fi' >> /usr/local/bin/node && \
    echo 'if [ -x /opt/node-install/bin/node ]; then' >> /usr/local/bin/node && \
    echo '  exec /opt/node-install/bin/node "$@"' >> /usr/local/bin/node && \
    echo 'else' >> /usr/local/bin/node && \
    echo '  exec /usr/bin/node "$@"' >> /usr/local/bin/node && \
    echo 'fi' >> /usr/local/bin/node && \
    chmod +x /usr/local/bin/node && \
    echo '#!/bin/sh' > /usr/local/bin/npm && \
    echo 'if [ -d /opt/openssl-install ]; then' >> /usr/local/bin/npm && \
    echo '  export LD_LIBRARY_PATH="/opt/openssl-install/lib64:/opt/openssl-install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"' >> /usr/local/bin/npm && \
    echo 'fi' >> /usr/local/bin/npm && \
    echo 'if [ -x /opt/node-install/bin/npm ]; then' >> /usr/local/bin/npm && \
    echo '  exec /opt/node-install/bin/npm "$@"' >> /usr/local/bin/npm && \
    echo 'else' >> /usr/local/bin/npm && \
    echo '  exec /usr/bin/npm "$@"' >> /usr/local/bin/npm && \
    echo 'fi' >> /usr/local/bin/npm && \
    chmod +x /usr/local/bin/npm && \
    echo '#!/bin/sh' > /usr/local/bin/npx && \
    echo 'if [ -d /opt/openssl-install ]; then' >> /usr/local/bin/npx && \
    echo '  export LD_LIBRARY_PATH="/opt/openssl-install/lib64:/opt/openssl-install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"' >> /usr/local/bin/npx && \
    echo 'fi' >> /usr/local/bin/npx && \
    echo 'if [ -x /opt/node-install/bin/npx ]; then' >> /usr/local/bin/npx && \
    echo '  exec /opt/node-install/bin/npx "$@"' >> /usr/local/bin/npx && \
    echo 'else' >> /usr/local/bin/npx && \
    echo '  exec /usr/bin/npx "$@"' >> /usr/local/bin/npx && \
    echo 'fi' >> /usr/local/bin/npx && \
    chmod +x /usr/local/bin/npx

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

# Generate self-signed SSL certificates for PCCS HTTPS server
# PCCS expects: ./ssl_key/private.pem and ./ssl_key/file.crt
RUN mkdir -p /opt/intel/sgx-dcap-pccs/ssl_key && \
    /usr/local/bin/openssl req -x509 -nodes -newkey rsa:3072 \
        -keyout /opt/intel/sgx-dcap-pccs/ssl_key/private.pem \
        -out /opt/intel/sgx-dcap-pccs/ssl_key/file.crt \
        -days 3650 \
        -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" && \
    chmod 600 /opt/intel/sgx-dcap-pccs/ssl_key/private.pem && \
    chmod 644 /opt/intel/sgx-dcap-pccs/ssl_key/file.crt

# Configure QPL with default settings (will be updated by entrypoint based on PCCS availability)
# Default: point to Intel PCS directly (no local PCCS)
RUN echo '{\n\
  "pccs_url": "https://api.trustedservices.intel.com/sgx/certification/v4/",\n\
  "use_secure_cert": true,\n\
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

# Install RA-TLS LD_PRELOAD injection module and patch gramine-manifest
# This enables transparent RA-TLS quote verification without renaming the command
# COPY scripts/ratls_inject.py /usr/local/lib/python3.10/dist-packages/ratls_inject.py
# RUN chmod 644 /usr/local/lib/python3.10/dist-packages/ratls_inject.py

# Verify libratls-quote-verify.so is available and update ldconfig cache
# The library is installed by Gramine's ninja install (typically to /usr/local/lib/x86_64-linux-gnu/)
RUN ldconfig && \
    echo "Verifying libratls-quote-verify.so installation:" && \
    RATLS_LIB_PATH=$(ldconfig -p | grep 'libratls-quote-verify.so ' | awk '{print $NF}' | head -1) && \
    if [ -n "$RATLS_LIB_PATH" ] && [ -f "$RATLS_LIB_PATH" ]; then \
        echo "✓ Found libratls-quote-verify.so at: $RATLS_LIB_PATH"; \
    else \
        echo "ERROR: libratls-quote-verify.so not found in ldconfig cache!" >&2; \
        echo "Checking common installation paths..." >&2; \
        find /usr/local/lib -name 'libratls-quote-verify.so*' 2>/dev/null || true; \
        exit 1; \
    fi

# Patch gramine-manifest in-place to add RA-TLS injection (maintains command name for compatibility)
# COPY scripts/patch-gramine-manifest.sh /tmp/patch-gramine-manifest.sh
# RUN chmod +x /tmp/patch-gramine-manifest.sh && \
#     /tmp/patch-gramine-manifest.sh && \
#     rm /tmp/patch-gramine-manifest.sh

# Set environment variables
# PATH is reordered so /usr/local/bin (wrappers) comes first
ENV PATH="/usr/local/bin:/opt/node-install/bin:/opt/openssl-install/bin:${PATH}"
# LD_LIBRARY_PATH is removed from global scope - wrappers handle it for openssl and node only
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
