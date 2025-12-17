# MySQL 8 with RA-TLS (Remote Attestation TLS) Support in SGX Enclave
#
# This Dockerfile downloads required source files from the repository.
# External files are stored in the files/ directory and downloaded via curl.
#
# Architecture:
# - A C launcher program runs inside the SGX enclave
# - The launcher reads whitelist from smart contract (if configured)
# - The launcher sets LD_PRELOAD to inject RA-TLS library for mysqld
# - The launcher uses execve() to replace itself with mysqld (same enclave)
# - Only mysqld gets RA-TLS hooks (launcher runs without them)
# - DCAP attestation is enabled for remote verification
#
# Features:
# - Mutual RA-TLS authentication (both client and server must be SGX-based)
# - Certificate-only MySQL authentication (no passwords)
# - Certificates generated at startup from SGX quotes using secp256k1 curve
# - Optional whitelist from smart contract (via CONTRACT_ADDRESS env var)
# - Pre-compiled Gramine manifest for immediate SGX execution
#
# Environment Variables:
#   CONTRACT_ADDRESS - Smart contract address to read whitelist from (optional)
#   RPC_URL - Ethereum RPC endpoint URL (required if CONTRACT_ADDRESS is set)
#
# Usage:
#   docker build -t mysql-ratls .
#   docker run -d \
#     --device=/dev/sgx_enclave \
#     --device=/dev/sgx_provision \
#     -e PCCS_API_KEY=your_api_key \
#     -e CONTRACT_ADDRESS=0x... \
#     -e RPC_URL=https://... \
#     -p 3306:3306 \
#     mysql-ratls
#

ARG BASE_IMAGE=ghcr.io/mccoysc/gramine-web3-wallet-docker:latest
FROM ${BASE_IMAGE}

LABEL org.opencontainers.image.description="MySQL 8 with RA-TLS (Remote Attestation TLS) support for SGX-based mutual authentication"
LABEL org.opencontainers.image.licenses=GPL-2.0

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Prevent services from starting during package installation
# This is standard practice for Docker builds - we don't want MySQL to start during apt-get install
RUN printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d && \
    chmod +x /usr/sbin/policy-rc.d

# MySQL version for pre-compiled binary download
ARG MYSQL_VERSION=8.0.40

# Install runtime dependencies for MySQL and launcher compilation
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4-openssl-dev \
    libaio1 \
    libnuma1 \
    libmecab2 \
    && rm -rf /var/lib/apt/lists/*

# Download and install pre-compiled MySQL with GR patches from GitHub Releases
# This MySQL build includes custom patches for SGX/Gramine compatibility:
# - group_replication_skip_local_address_check (skip IP validation)
# - group_replication_ssl_require_peer_cert (strict mutual TLS)
# - group_replication_ssl_allow_self_signed (accept RA-TLS self-signed certs)
RUN MYSQL_TARBALL="mysql-install-${MYSQL_VERSION}.tar.gz" && \
    MYSQL_URL="https://github.com/mccoysc/gramine-web3-wallet-docker/releases/download/prebuilt-mysql-${MYSQL_VERSION}/${MYSQL_TARBALL}" && \
    echo "Downloading pre-compiled MySQL from: ${MYSQL_URL}" && \
    curl -fsSL -o "/tmp/${MYSQL_TARBALL}" "${MYSQL_URL}" && \
    cd /opt && \
    tar -xzf "/tmp/${MYSQL_TARBALL}" && \
    rm -f "/tmp/${MYSQL_TARBALL}" && \
    # Create symlinks for MySQL binaries
    ln -sf /opt/mysql-install/bin/mysqld /usr/sbin/mysqld && \
    ln -sf /opt/mysql-install/bin/mysql /usr/bin/mysql && \
    ln -sf /opt/mysql-install/bin/mysqladmin /usr/bin/mysqladmin && \
    ln -sf /opt/mysql-install/bin/mysql_secure_installation /usr/bin/mysql_secure_installation && \
    # Create mysql user and group if not exists
    groupadd -r mysql 2>/dev/null || true && \
    useradd -r -g mysql -s /bin/false mysql 2>/dev/null || true && \
    echo "Pre-compiled MySQL ${MYSQL_VERSION} with GR patches installed successfully"

# Clean up default MySQL data directory (we use encrypted partition at runtime)
# and remove policy-rc.d since we're done with apt installations
RUN rm -rf /var/lib/mysql/* && rm -f /usr/sbin/policy-rc.d

# Create directories for MySQL data, SSL certificates, and secure file operations
RUN mkdir -p /var/lib/mysql /var/lib/mysql-ssl /var/run/mysqld /var/lib/mysql-files \
    && chown -R mysql:mysql /var/lib/mysql /var/lib/mysql-ssl /var/run/mysqld /var/lib/mysql-files \
    && chmod 755 /var/run/mysqld /var/lib/mysql-files

# Pre-initialize MySQL data directory during build
# This creates the system tables and initial data files that MySQL needs
# At runtime, the launcher will copy these files to the encrypted partition
# Benefits:
# - Initialization happens outside the SGX enclave (more reliable)
# - No need to fork child processes inside the enclave
# - Pre-initialized data can be added to trusted_files for integrity protection
RUN mkdir -p /app/mysql-init-data /var/log/mysql && \
    /usr/sbin/mysqld --initialize-insecure \
        --datadir=/app/mysql-init-data \
        --log-error=/var/log/mysql/init-error.log \
        --console && \
    echo "MySQL pre-initialization completed successfully" && \
    ls -la /app/mysql-init-data/

# Create launcher directory
RUN mkdir -p /opt/mysql-ratls/launcher

# Download cJSON library (MIT licensed JSON parser)
RUN curl -fsSL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c \
      -o /opt/mysql-ratls/launcher/cJSON.c && \
    curl -fsSL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h \
      -o /opt/mysql-ratls/launcher/cJSON.h

# Download the C launcher source code from repository
RUN curl -fsSL https://raw.githubusercontent.com/mccoysc/gramine-web3-wallet-docker/main/examples/mysql-ratls/files/mysql_ratls_launcher.c \
      -o /opt/mysql-ratls/launcher/mysql_ratls_launcher.c

# Compile and install the launcher directly with gcc
# Note: Using direct gcc instead of Makefile to ensure compatibility with
# both legacy Docker builder and BuildKit
RUN cd /opt/mysql-ratls/launcher && \
    gcc -Wall -Wextra -O2 -g -o mysql-ratls-launcher \
        mysql_ratls_launcher.c cJSON.c \
        -lcurl && \
    install -m 755 mysql-ratls-launcher /usr/local/bin/mysql-ratls-launcher && \
    echo "Launcher compiled and installed successfully"

# Download the Gramine manifest template from repository
RUN curl -fsSL https://raw.githubusercontent.com/mccoysc/gramine-web3-wallet-docker/main/examples/mysql-ratls/files/mysql-ratls.manifest.template \
      -o /opt/mysql-ratls/mysql-ratls.manifest.template.base

# Download the trusted_files generator script from repository
RUN curl -fsSL https://raw.githubusercontent.com/mccoysc/gramine-web3-wallet-docker/main/examples/mysql-ratls/files/generate-trusted-files.sh \
      -o /opt/mysql-ratls/generate-trusted-files.sh && \
    chmod +x /opt/mysql-ratls/generate-trusted-files.sh

# Generate trusted_files dynamically and inject into manifest template
# This ensures only the minimal set of required files are included
RUN /opt/mysql-ratls/generate-trusted-files.sh /opt/mysql-ratls/trusted_files.toml && \
    # Replace the placeholder in the manifest template with the generated trusted_files
    sed -e '/^# __TRUSTED_FILES_PLACEHOLDER__$/r /opt/mysql-ratls/trusted_files.toml' \
        -e '/^# __TRUSTED_FILES_PLACEHOLDER__$/d' \
        /opt/mysql-ratls/mysql-ratls.manifest.template.base > /opt/mysql-ratls/mysql-ratls.manifest.template && \
    # Clean up temporary files
    rm -f /opt/mysql-ratls/mysql-ratls.manifest.template.base /opt/mysql-ratls/trusted_files.toml && \
    echo "Trusted files generated and injected into manifest template"

# Configure MySQL for certificate-only authentication
# This configuration enforces SSL/TLS and prepares for RA-TLS certificates
# Note: ssl_mode is a CLIENT-side option, not valid for [mysqld] server config
# Note: ssl_ca is not set because RA-TLS uses self-signed certificates without a CA
RUN printf '\n# RA-TLS Configuration\n[mysqld]\n# Require secure transport (SSL/TLS)\nrequire_secure_transport = ON\n\n# SSL certificate path (generated at runtime by RA-TLS)\n# Certificate is in normal directory, private key is in encrypted partition\nssl_cert = /var/lib/mysql-ssl/server-cert.pem\n# Private key path is set via command line argument (from encrypted partition)\n# Note: ssl_ca is NOT set because RA-TLS uses self-signed certificates\n# Note: ssl_mode is a CLIENT option, not valid for server config\n\n# Bind to all interfaces\nbind-address = 0.0.0.0\n\n# Skip name resolution for faster connections\nskip-name-resolve\n\n# Performance settings\nmax_connections = 100\ninnodb_buffer_pool_size = 256M\n\n# Data directory in encrypted partition to prevent data leakage\ndatadir = /app/wallet/mysql-data\n\n# Log files outside encrypted partition for debugging visibility\n# Note: This is overridden by --log-error command line argument from launcher\nlog_error = /var/log/mysql/error.log\ngeneral_log_file = /var/log/mysql/general.log\nslow_query_log_file = /var/log/mysql/slow.log\n\n' >> /etc/mysql/mysql.conf.d/mysqld.cnf

# Create necessary directories and build the Gramine manifest
# This generates the signed manifest so it's ready to run immediately
# Note: Directories in /app/wallet/ are created by the launcher inside the enclave
# (encrypted partition), so we only create non-encrypted directories here.
# No chown/chmod needed since we run as root in container.
#
# IMPORTANT: We do NOT set GRAMINE_LD_PRELOAD here. Instead, the launcher sets
# LD_PRELOAD before execve() to mysqld. This means:
# - The launcher runs WITHOUT RA-TLS hooks (no LD_PRELOAD for launcher)
# - Only mysqld gets the RA-TLS library injected via LD_PRELOAD
# - The RA-TLS library is already in trusted_files via generate-trusted-files.sh
RUN mkdir -p /var/lib/mysql-ssl /var/run/mysqld && \
    # Generate signing key for manifest
    mkdir -p /root/.config/gramine && \
    gramine-sgx-gen-private-key && \
    # Generate manifest (no GRAMINE_LD_PRELOAD - launcher handles LD_PRELOAD)
    mkdir -p /var/lib/mysql && \
    cd /var/lib/mysql && \
    gramine-manifest \
        -Dentrypoint=/usr/local/bin/mysql-ratls-launcher \
        -Dlog_level=error \
        /opt/mysql-ratls/mysql-ratls.manifest.template \
        /var/lib/mysql/mysql-ratls.manifest && \
    # Sign manifest
    gramine-sgx-sign \
        --manifest /var/lib/mysql/mysql-ratls.manifest \
        --output /var/lib/mysql/mysql-ratls.manifest.sgx && \
    # Verify files were created
    ls -la /var/lib/mysql/mysql-ratls.manifest* && \
    # Copy manifests to /opt/mysql-ratls for reference
    cp /var/lib/mysql/mysql-ratls.manifest* /opt/mysql-ratls/

# Clean up build artifacts (keep only what's needed at runtime)
RUN rm -rf /opt/mysql-ratls/launcher/*.c \
    /opt/mysql-ratls/launcher/*.h \
    /opt/mysql-ratls/launcher/*.o \
    /opt/mysql-ratls/launcher/mysql-ratls-launcher

# Note: All RA-TLS environment variables are configured in the Gramine manifest
# (loader.env.*) and are NOT set here because host environment variables are
# isolated from the SGX enclave. The manifest directly sets:
# - RA_TLS_CERT_ALGORITHM=secp256k1
# - RATLS_ENABLE_VERIFY=1
# - RATLS_REQUIRE_PEER_CERT=1
# - RATLS_CERT_PATH=/var/lib/mysql-ssl/server-cert.pem
# - RATLS_KEY_PATH=/app/wallet/mysql-keys/server-key.pem
# - MYSQL_DATA_DIR=/app/wallet/mysql-data

# Expose MySQL port and Group Replication XCom port
EXPOSE 3306 33061

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD mysqladmin ping -h localhost --silent || exit 1

# Set working directory to encrypted MySQL data directory
WORKDIR /app/
RUN rm -rf ra-tls-mbedtls
RUN rm -rf keys
RUN rm -rf manifests

# The entrypoint runs the base image's entrypoint first (starts aesmd, PCCS, etc.)
# Then runs gramine-sgx with the pre-compiled manifest to start the launcher in SGX enclave
# The launcher (inside enclave) will then execve() to mysqld
# Note: Use full path to manifest because WORKDIR is /app/wallet/mysql-data (encrypted partition)
ENTRYPOINT ["/usr/local/bin/entrypoint.sh","gramine-sgx","/var/lib/mysql/mysql-ratls"]
CMD []
