# MySQL 8 with RA-TLS (Remote Attestation TLS) Support
#
# This Dockerfile creates a MySQL 8 server image with transparent RA-TLS support
# using the gramine-web3-wallet-docker base image.
#
# Features:
# - Mutual RA-TLS authentication (both client and server must be SGX-based)
# - Certificate-only MySQL authentication (no passwords)
# - Certificates generated at startup from SGX quotes using secp256k1 curve
# - Optional whitelist from smart contract (via CONTRACT_ADDRESS env var)
#
# Environment Variables:
#   CONTRACT_ADDRESS - Smart contract address to read whitelist from (optional)
#   RPC_URL - Ethereum RPC endpoint URL (required if CONTRACT_ADDRESS is set)
#   RATLS_WHITELIST_CONFIG - Manual whitelist override (optional)
#   MYSQL_ROOT_PASSWORD - MySQL root password (optional, cert auth preferred)
#   MYSQL_DATABASE - Database to create on first run (optional)
#   MYSQL_USER - User to create on first run (optional)
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

# Install MySQL 8 server
RUN apt-get update && apt-get install -y --no-install-recommends \
    mysql-server \
    mysql-client \
    && rm -rf /var/lib/apt/lists/*

# Create directories for MySQL data and SSL certificates
RUN mkdir -p /var/lib/mysql /var/lib/mysql-ssl /var/run/mysqld \
    && chown -R mysql:mysql /var/lib/mysql /var/lib/mysql-ssl /var/run/mysqld \
    && chmod 755 /var/run/mysqld

# Copy the MySQL RA-TLS launcher script
COPY mysql-launcher.sh /usr/local/bin/mysql-launcher.sh
RUN chmod +x /usr/local/bin/mysql-launcher.sh

# Configure MySQL for certificate-only authentication
# This configuration enforces SSL/TLS and prepares for RA-TLS certificates
RUN cat >> /etc/mysql/mysql.conf.d/mysqld.cnf <<'EOF'

# RA-TLS Configuration
[mysqld]
# Require secure transport (SSL/TLS)
require_secure_transport = ON

# SSL certificate paths (generated at runtime by RA-TLS)
# Certificates are in normal directory, private key is in encrypted partition
ssl_ca = /var/lib/mysql-ssl/ca.pem
ssl_cert = /var/lib/mysql-ssl/server-cert.pem
# Private key path is set via command line argument (from encrypted partition)

# Require client certificates for authentication
ssl_mode = VERIFY_IDENTITY

# Bind to all interfaces
bind-address = 0.0.0.0

# Skip name resolution for faster connections
skip-name-resolve

# Performance settings
max_connections = 100
innodb_buffer_pool_size = 256M

EOF

# Install ethers.js for contract interaction
# This is needed by the launcher script to read whitelist from smart contract
RUN npm install -g ethers@6 && npm cache clean --force

# Set environment variables for RA-TLS
# Use secp256k1 curve for Ethereum compatibility
ENV RA_TLS_CERT_ALGORITHM=secp256k1
ENV RATLS_ENABLE_VERIFY=1
ENV RATLS_REQUIRE_PEER_CERT=1

# MySQL data directory
ENV MYSQL_DATA_DIR=/var/lib/mysql
# RA-TLS certificate and key paths (used by libratls-quote-verify.so and MySQL)
# Certificate can be in normal directory
ENV RATLS_CERT_PATH=/var/lib/mysql-ssl/server-cert.pem
# Private key MUST be in encrypted partition for security
ENV RATLS_KEY_PATH=/app/wallet/mysql-keys/server-key.pem

# Expose MySQL port
EXPOSE 3306

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD mysqladmin ping -h localhost --silent || exit 1

# Set working directory
WORKDIR /var/lib/mysql

# The entrypoint runs the base image's entrypoint first (starts aesmd, PCCS, etc.)
# Then runs the MySQL launcher which handles RA-TLS setup and starts mysqld
ENTRYPOINT ["/usr/local/bin/entrypoint.sh", "/usr/local/bin/mysql-launcher.sh"]
CMD []
