# MySQL 8 with RA-TLS (Remote Attestation TLS) Support in SGX Enclave
#
# This is a SELF-CONTAINED Dockerfile - no external files are needed.
# All source code (C launcher, manifest template, build scripts) is embedded.
#
# Architecture:
# - A C launcher program runs inside the SGX enclave
# - The launcher reads whitelist from smart contract (if configured)
# - The launcher uses execve() to replace itself with mysqld (same enclave)
# - RA-TLS is injected via LD_PRELOAD configured in the Gramine manifest
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

# Prevent services from starting during package installation
# This is standard practice for Docker builds - we don't want MySQL to start during apt-get install
RUN printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d && \
    chmod +x /usr/sbin/policy-rc.d

# Install MySQL 8 server and development dependencies for launcher compilation
RUN apt-get update && apt-get install -y --no-install-recommends \
    mysql-server \
    mysql-client \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Clean up default MySQL data directory (we use encrypted partition at runtime)
# and remove policy-rc.d since we're done with apt installations
RUN rm -rf /var/lib/mysql/* && rm -f /usr/sbin/policy-rc.d

# Create directories for MySQL data and SSL certificates
RUN mkdir -p /var/lib/mysql /var/lib/mysql-ssl /var/run/mysqld \
    && chown -R mysql:mysql /var/lib/mysql /var/lib/mysql-ssl /var/run/mysqld \
    && chmod 755 /var/run/mysqld

# Create launcher directory
RUN mkdir -p /opt/mysql-ratls/launcher

# Download cJSON library (MIT licensed JSON parser)
RUN curl -fsSL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c \
      -o /opt/mysql-ratls/launcher/cJSON.c && \
    curl -fsSL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h \
      -o /opt/mysql-ratls/launcher/cJSON.h

# Embed the C launcher source code
RUN cat <<'LAUNCHER_EOF' > /opt/mysql-ratls/launcher/mysql_ratls_launcher.c
/*
 * MySQL RA-TLS Launcher
 * 
 * This program runs inside a Gramine SGX enclave and:
 * 1. Reads whitelist configuration from a smart contract (if CONTRACT_ADDRESS is set)
 * 2. Sets up RA-TLS environment variables
 * 3. Uses execve() to replace itself with mysqld
 *
 * The launcher avoids creating child processes in the enclave by using execve()
 * to directly replace the current process with mysqld.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>

#include "cJSON.h"

/* Configuration constants */
#define MYSQLD_PATH "/usr/sbin/mysqld"
#define DEFAULT_CERT_PATH "/var/lib/mysql-ssl/server-cert.pem"
#define DEFAULT_KEY_PATH "/app/wallet/mysql-keys/server-key.pem"
#define DEFAULT_CA_PATH "/var/lib/mysql-ssl/ca.pem"
#define DEFAULT_DATA_DIR "/app/wallet/mysql-data"
#define INIT_SENTINEL_FILE ".mysql_initialized"
#define INIT_SQL_FILE "init_users.sql"

/* getSGXConfig() function selector: keccak256("getSGXConfig()")[0:4] */
#define GET_SGX_CONFIG_SELECTOR "0x062e2252"

/* Maximum sizes */
#define MAX_URL_LEN 2048
#define MAX_RESPONSE_LEN (1024 * 1024)  /* 1MB max response */
#define MAX_PATH_LEN 4096

/* Structure to hold curl response */
struct curl_response {
    char *data;
    size_t size;
};

/* Curl write callback */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct curl_response *resp = (struct curl_response *)userp;
    
    if (resp->size + realsize > MAX_RESPONSE_LEN) {
        fprintf(stderr, "[Launcher] Response too large, truncating\n");
        return 0;
    }
    
    char *ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "[Launcher] Out of memory\n");
        return 0;
    }
    
    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = '\0';
    
    return realsize;
}

/* Create directory recursively */
static int mkdir_p(const char *path) {
    char tmp[MAX_PATH_LEN];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

/* Get directory from path */
static void get_dirname(const char *path, char *dir, size_t dir_size) {
    strncpy(dir, path, dir_size - 1);
    dir[dir_size - 1] = '\0';
    
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    }
}

/* Check if file exists */
static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Check if MySQL is initialized (sentinel file exists) */
static int is_mysql_initialized(const char *data_dir) {
    char sentinel_path[MAX_PATH_LEN];
    snprintf(sentinel_path, sizeof(sentinel_path), "%s/%s", data_dir, INIT_SENTINEL_FILE);
    return file_exists(sentinel_path);
}

/* Create the initialization SQL file for first boot */
static int create_init_sql(const char *data_dir, char *init_sql_path, size_t path_size) {
    snprintf(init_sql_path, path_size, "%s/%s", data_dir, INIT_SQL_FILE);
    
    FILE *f = fopen(init_sql_path, "w");
    if (!f) {
        fprintf(stderr, "[Launcher] Failed to create init SQL file: %s\n", strerror(errno));
        return -1;
    }
    
    /* Write idempotent SQL to create X.509-only users */
    fprintf(f, "-- MySQL RA-TLS User Initialization\n");
    fprintf(f, "-- This file is executed on first boot inside the SGX enclave\n");
    fprintf(f, "-- Users are configured with REQUIRE X509 (certificate-only authentication)\n");
    fprintf(f, "-- RA-TLS handles the actual SGX attestation verification\n\n");
    
    
    /* Create application user with X509 requirement */
    fprintf(f, "-- Create application user that requires X.509 certificate\n");
    fprintf(f, "CREATE USER IF NOT EXISTS 'app'@'%%' IDENTIFIED BY '' REQUIRE X509;\n");
    fprintf(f, "GRANT ALL PRIVILEGES ON *.* TO 'app'@'%%' WITH GRANT OPTION;\n\n");
    
    /* Create a read-only user with X509 requirement */
    fprintf(f, "-- Create read-only user that requires X.509 certificate\n");
    fprintf(f, "CREATE USER IF NOT EXISTS 'reader'@'%%' IDENTIFIED BY '' REQUIRE X509;\n");
    fprintf(f, "GRANT SELECT ON *.* TO 'reader'@'%%';\n\n");
    
    fprintf(f, "FLUSH PRIVILEGES;\n");
    
    fclose(f);
    
    printf("[Launcher] Created init SQL file: %s\n", init_sql_path);
    return 0;
}

/* Create the sentinel file to mark MySQL as initialized */
static int create_sentinel_file(const char *data_dir) {
    char sentinel_path[MAX_PATH_LEN];
    snprintf(sentinel_path, sizeof(sentinel_path), "%s/%s", data_dir, INIT_SENTINEL_FILE);
    
    FILE *f = fopen(sentinel_path, "w");
    if (!f) {
        fprintf(stderr, "[Launcher] Failed to create sentinel file: %s\n", strerror(errno));
        return -1;
    }
    
    fprintf(f, "MySQL initialized with RA-TLS X.509 users\n");
    fclose(f);
    
    printf("[Launcher] Created sentinel file: %s\n", sentinel_path);
    return 0;
}

/* Hex character to integer */
static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode hex string to bytes */
static int hex_decode(const char *hex, unsigned char *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    
    /* Skip 0x prefix if present */
    if (hex_len >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
        hex_len -= 2;
    }
    
    if (hex_len % 2 != 0 || hex_len / 2 > out_len) {
        return -1;
    }
    
    for (size_t i = 0; i < hex_len / 2; i++) {
        int high = hex_char_to_int(hex[i * 2]);
        int low = hex_char_to_int(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return -1;
        out[i] = (high << 4) | low;
    }
    
    return hex_len / 2;
}

/* Decode ABI-encoded string from eth_call result */
static char *decode_abi_string(const char *hex_result) {
    unsigned char *bytes = NULL;
    char *result = NULL;
    
    /* Skip 0x prefix */
    if (strlen(hex_result) < 2) return NULL;
    const char *hex = hex_result;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }
    
    size_t hex_len = strlen(hex);
    if (hex_len < 128) {  /* Minimum: offset (64) + length (64) */
        return NULL;
    }
    
    /* Allocate buffer for decoded bytes */
    size_t bytes_len = hex_len / 2;
    bytes = malloc(bytes_len);
    if (!bytes) return NULL;
    
    if (hex_decode(hex_result, bytes, bytes_len) < 0) {
        free(bytes);
        return NULL;
    }
    
    /* First 32 bytes: offset to string data (should be 0x20 = 32) */
    /* Next 32 bytes at offset: string length */
    /* Following bytes: string data */
    
    /* Read offset (big-endian, last 4 bytes of first 32) */
    size_t offset = 0;
    for (int i = 28; i < 32; i++) {
        offset = (offset << 8) | bytes[i];
    }
    
    if (offset + 32 > bytes_len) {
        free(bytes);
        return NULL;
    }
    
    /* Read string length at offset */
    size_t str_len = 0;
    for (int i = 28; i < 32; i++) {
        str_len = (str_len << 8) | bytes[offset + i];
    }
    
    if (offset + 32 + str_len > bytes_len) {
        free(bytes);
        return NULL;
    }
    
    /* Extract string */
    result = malloc(str_len + 1);
    if (!result) {
        free(bytes);
        return NULL;
    }
    
    memcpy(result, bytes + offset + 32, str_len);
    result[str_len] = '\0';
    
    free(bytes);
    return result;
}

/* Call Ethereum JSON-RPC eth_call */
static char *eth_call(const char *rpc_url, const char *contract_address, const char *data) {
    CURL *curl;
    CURLcode res;
    struct curl_response response = {0};
    char *result = NULL;
    char post_data[MAX_URL_LEN];
    struct curl_slist *headers = NULL;
    
    /* Build JSON-RPC request */
    snprintf(post_data, sizeof(post_data),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_call\","
        "\"params\":[{\"to\":\"%s\",\"data\":\"%s\"},\"latest\"]}",
        contract_address, data);
    
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[Launcher] Failed to initialize curl\n");
        return NULL;
    }
    
    response.data = malloc(1);
    if (!response.data) {
        curl_easy_cleanup(curl);
        return NULL;
    }
    response.data[0] = '\0';
    response.size = 0;
    
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl_easy_setopt(curl, CURLOPT_URL, rpc_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    
    res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        fprintf(stderr, "[Launcher] curl error: %s\n", curl_easy_strerror(res));
        goto cleanup;
    }
    
    /* Parse JSON response */
    cJSON *json = cJSON_Parse(response.data);
    if (!json) {
        fprintf(stderr, "[Launcher] Failed to parse JSON response\n");
        goto cleanup;
    }
    
    /* Check for error */
    cJSON *error = cJSON_GetObjectItem(json, "error");
    if (error) {
        cJSON *message = cJSON_GetObjectItem(error, "message");
        fprintf(stderr, "[Launcher] RPC error: %s\n", 
            message ? message->valuestring : "unknown");
        cJSON_Delete(json);
        goto cleanup;
    }
    
    /* Get result */
    cJSON *result_json = cJSON_GetObjectItem(json, "result");
    if (result_json && result_json->valuestring) {
        result = strdup(result_json->valuestring);
    }
    
    cJSON_Delete(json);
    
cleanup:
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(response.data);
    
    return result;
}

/* Read whitelist from smart contract */
static char *read_whitelist_from_contract(const char *contract_address, const char *rpc_url) {
    char *whitelist = NULL;
    
    printf("[Launcher] Reading whitelist from contract...\n");
    printf("[Launcher]   Contract: %s\n", contract_address);
    printf("[Launcher]   RPC URL: %s\n", rpc_url);
    
    /* Call getSGXConfig() */
    char *hex_result = eth_call(rpc_url, contract_address, GET_SGX_CONFIG_SELECTOR);
    if (!hex_result) {
        fprintf(stderr, "[Launcher] Failed to call getSGXConfig()\n");
        return NULL;
    }
    
    /* Check for empty result (0x or very short) */
    if (strlen(hex_result) < 66) {  /* 0x + 64 chars minimum */
        fprintf(stderr, "[Launcher] Empty or invalid response from getSGXConfig()\n");
        free(hex_result);
        return NULL;
    }
    
    /* Decode ABI-encoded string */
    char *sgx_config = decode_abi_string(hex_result);
    free(hex_result);
    
    if (!sgx_config || strlen(sgx_config) == 0) {
        fprintf(stderr, "[Launcher] SGX config is empty\n");
        free(sgx_config);
        return NULL;
    }
    
    printf("[Launcher] Got SGX config, parsing JSON...\n");
    
    /* Parse as JSON */
    cJSON *json = cJSON_Parse(sgx_config);
    if (!json) {
        fprintf(stderr, "[Launcher] Failed to parse SGX config as JSON\n");
        free(sgx_config);
        return NULL;
    }
    
    /* Extract RATLS_WHITELIST_CONFIG field */
    cJSON *whitelist_config = cJSON_GetObjectItem(json, "RATLS_WHITELIST_CONFIG");
    if (whitelist_config && whitelist_config->valuestring) {
        whitelist = strdup(whitelist_config->valuestring);
        printf("[Launcher] Found RATLS_WHITELIST_CONFIG in SGX config\n");
    } else {
        fprintf(stderr, "[Launcher] RATLS_WHITELIST_CONFIG field not found in SGX config\n");
    }
    
    cJSON_Delete(json);
    free(sgx_config);
    
    return whitelist;
}

/* Set environment variable with logging */
static void set_env(const char *name, const char *value, int overwrite) {
    if (setenv(name, value, overwrite) == 0) {
        printf("[Launcher] Set %s=%s\n", name, value);
    } else {
        fprintf(stderr, "[Launcher] Failed to set %s: %s\n", name, strerror(errno));
    }
}

/* Set environment variable if not already set */
static void set_env_default(const char *name, const char *default_value) {
    const char *current = getenv(name);
    if (!current || strlen(current) == 0) {
        set_env(name, default_value, 1);
    } else {
        printf("[Launcher] Using existing %s=%s\n", name, current);
    }
}

int main(int argc, char *argv[]) {
    printf("==========================================\n");
    printf("MySQL RA-TLS Launcher (SGX Enclave)\n");
    printf("==========================================\n\n");
    
    /* Initialize curl globally */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    /* Get environment variables */
    const char *contract_address = getenv("CONTRACT_ADDRESS");
    const char *rpc_url = getenv("RPC_URL");
    const char *existing_whitelist = getenv("RATLS_WHITELIST_CONFIG");
    
    /* Suppress unused variable warning */
    (void)existing_whitelist;
    
    /* Set RA-TLS configuration */
    printf("[Launcher] Setting up RA-TLS configuration...\n");
    
    /* Use secp256k1 curve for Ethereum compatibility */
    set_env("RA_TLS_CERT_ALGORITHM", "secp256k1", 1);
    
    /* Enable RA-TLS verification and require peer certificate */
    set_env("RATLS_ENABLE_VERIFY", "1", 1);
    set_env("RATLS_REQUIRE_PEER_CERT", "1", 1);
    
    /* Set default certificate and key paths */
    set_env_default("RATLS_CERT_PATH", DEFAULT_CERT_PATH);
    set_env_default("RATLS_KEY_PATH", DEFAULT_KEY_PATH);
    
    /* Get the actual paths being used */
    const char *cert_path = getenv("RATLS_CERT_PATH");
    const char *key_path = getenv("RATLS_KEY_PATH");
    
    /* Create directories for certificate and key */
    char cert_dir[MAX_PATH_LEN];
    char key_dir[MAX_PATH_LEN];
    
    get_dirname(cert_path, cert_dir, sizeof(cert_dir));
    get_dirname(key_path, key_dir, sizeof(key_dir));
    
    printf("[Launcher] Creating certificate directory: %s\n", cert_dir);
    if (mkdir_p(cert_dir) != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create cert directory: %s\n", strerror(errno));
    }
    
    printf("[Launcher] Creating key directory: %s\n", key_dir);
    if (mkdir_p(key_dir) != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create key directory: %s\n", strerror(errno));
    }
    
    /* Handle whitelist configuration */
    printf("\n[Launcher] Whitelist Configuration:\n");
    
    if (contract_address && strlen(contract_address) > 0) {
        printf("[Launcher] Contract address specified: %s\n", contract_address);
        
        if (!rpc_url || strlen(rpc_url) == 0) {
            fprintf(stderr, "[Launcher] Warning: RPC_URL not set, cannot read from contract\n");
            printf("[Launcher] Falling back to environment-based whitelist (if set)\n");
        } else {
            /* Try to read whitelist from contract */
            char *whitelist = read_whitelist_from_contract(contract_address, rpc_url);
            
            if (whitelist) {
                set_env("RATLS_WHITELIST_CONFIG", whitelist, 1);
                free(whitelist);
            } else {
                printf("[Launcher] Could not read valid whitelist from contract\n");
                printf("[Launcher] Using environment-based whitelist (if set)\n");
            }
        }
    } else {
        printf("[Launcher] No CONTRACT_ADDRESS specified\n");
        printf("[Launcher] Using environment-based whitelist (if set)\n");
    }
    
    /* Display whitelist status */
    const char *final_whitelist = getenv("RATLS_WHITELIST_CONFIG");
    if (final_whitelist && strlen(final_whitelist) > 0) {
        printf("[Launcher] RATLS_WHITELIST_CONFIG is set\n");
        printf("[Launcher] Only clients matching the whitelist can connect\n");
    } else {
        printf("[Launcher] No whitelist configured\n");
        printf("[Launcher] Any valid RA-TLS client can connect\n");
    }
    
    /* Clean up curl */
    curl_global_cleanup();
    
    /* Prepare MySQL arguments */
    printf("\n==========================================\n");
    printf("Starting MySQL Server via execve()\n");
    printf("==========================================\n\n");
    
    const char *data_dir = getenv("MYSQL_DATA_DIR");
    if (!data_dir) data_dir = DEFAULT_DATA_DIR;
    
    /* Check if this is first boot (need to initialize users) */
    int first_boot = !is_mysql_initialized(data_dir);
    char init_sql_path[MAX_PATH_LEN] = {0};
    char init_file_arg[MAX_PATH_LEN] = {0};
    
    if (first_boot) {
        printf("[Launcher] First boot detected - will create X.509 users\n");
        
        /* Create the init SQL file in the encrypted data directory */
        if (create_init_sql(data_dir, init_sql_path, sizeof(init_sql_path)) == 0) {
            snprintf(init_file_arg, sizeof(init_file_arg), "--init-file=%s", init_sql_path);
            printf("[Launcher] Will execute init SQL on startup: %s\n", init_sql_path);
        } else {
            fprintf(stderr, "[Launcher] Warning: Could not create init SQL file\n");
            first_boot = 0;  /* Don't add --init-file if we couldn't create the file */
        }
        
        /* Create sentinel file to mark as initialized */
        /* Note: We create it now so that if MySQL crashes during init, we don't retry */
        /* The init SQL is idempotent anyway, so re-running it is safe */
        create_sentinel_file(data_dir);
    } else {
        printf("[Launcher] MySQL already initialized - skipping user creation\n");
    }
    
    /* Build argument list for mysqld */
    /* We need: mysqld --user=mysql --datadir=... --ssl-ca=... --ssl-cert=... --ssl-key=... --require-secure-transport=ON [--init-file=...] [user args] */
    
    int extra_args = first_boot ? 8 : 7;  /* Add 1 for --init-file on first boot */
    int new_argc = argc + extra_args;
    char **new_argv = malloc((new_argc + 1) * sizeof(char *));
    if (!new_argv) {
        fprintf(stderr, "[Launcher] Failed to allocate memory for arguments\n");
        return 1;
    }
    
    /* Build SSL argument strings */
    char ssl_ca_arg[MAX_PATH_LEN];
    char ssl_cert_arg[MAX_PATH_LEN];
    char ssl_key_arg[MAX_PATH_LEN];
    char datadir_arg[MAX_PATH_LEN];
    
    snprintf(ssl_ca_arg, sizeof(ssl_ca_arg), "--ssl-ca=%s", DEFAULT_CA_PATH);
    snprintf(ssl_cert_arg, sizeof(ssl_cert_arg), "--ssl-cert=%s", cert_path);
    snprintf(ssl_key_arg, sizeof(ssl_key_arg), "--ssl-key=%s", key_path);
    snprintf(datadir_arg, sizeof(datadir_arg), "--datadir=%s", data_dir);
    
    int idx = 0;
    new_argv[idx++] = MYSQLD_PATH;
    new_argv[idx++] = "--user=mysql";
    new_argv[idx++] = datadir_arg;
    new_argv[idx++] = ssl_ca_arg;
    new_argv[idx++] = ssl_cert_arg;
    new_argv[idx++] = ssl_key_arg;
    new_argv[idx++] = "--require-secure-transport=ON";
    
    /* Add --init-file on first boot */
    if (first_boot && init_file_arg[0] != '\0') {
        new_argv[idx++] = init_file_arg;
    }
    
    /* Copy any additional arguments from command line */
    for (int i = 1; i < argc; i++) {
        new_argv[idx++] = argv[i];
    }
    new_argv[idx] = NULL;
    
    printf("[Launcher] Executing: %s\n", MYSQLD_PATH);
    printf("[Launcher]   Data directory: %s\n", data_dir);
    printf("[Launcher]   Certificate: %s\n", cert_path);
    printf("[Launcher]   Private key: %s\n", key_path);
    printf("[Launcher]   CA certificate: %s\n", DEFAULT_CA_PATH);
    if (first_boot && init_file_arg[0] != '\0') {
        printf("[Launcher]   Init file: %s\n", init_sql_path);
    }
    printf("\n");
    
    /* Use execve to replace this process with mysqld */
    /* This preserves the environment variables we set */
    execv(MYSQLD_PATH, new_argv);
    
    /* If we get here, execve failed */
    fprintf(stderr, "[Launcher] Failed to execute %s: %s\n", MYSQLD_PATH, strerror(errno));
    free(new_argv);
    
    return 1;
}
LAUNCHER_EOF

# Embed the Makefile for the launcher
RUN cat <<'MAKEFILE_EOF' > /opt/mysql-ratls/launcher/Makefile
# Makefile for MySQL RA-TLS Launcher
# This launcher runs inside a Gramine SGX enclave

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lcurl

TARGET = mysql-ratls-launcher
SRCS = mysql_ratls_launcher.c cJSON.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

mysql_ratls_launcher.o: mysql_ratls_launcher.c cJSON.h
cJSON.o: cJSON.c cJSON.h

clean:
	rm -f $(TARGET) $(OBJS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
MAKEFILE_EOF

# Compile and install the launcher
RUN cd /opt/mysql-ratls/launcher && \
    make clean || true && \
    make && \
    make install

# Embed the Gramine manifest template
RUN cat <<'MANIFEST_EOF' > /opt/mysql-ratls/mysql-ratls.manifest.template
# Gramine manifest template for MySQL RA-TLS
# This manifest runs the mysql-ratls-launcher which then execve() to mysqld
# Both the launcher and mysqld run inside the same SGX enclave

# Note: loader.entrypoint is automatically set by gramine-manifest to the LibOS
# libos.entrypoint specifies the actual application to run inside the enclave
libos.entrypoint = "{{ entrypoint }}"

loader.log_level = "{{ log_level }}"

# Environment variables
loader.env.LD_LIBRARY_PATH = "/lib:/usr/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:/usr/local/lib:/usr/local/lib/x86_64-linux-gnu"
loader.env.PATH = "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
loader.env.HOME = "/app/wallet/mysql-data"

# RA-TLS injection via LD_PRELOAD
# This enables transparent RA-TLS for MySQL connections
loader.env.LD_PRELOAD = "/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so"

# RA-TLS Configuration
# Use secp256k1 curve for Ethereum compatibility
loader.env.RA_TLS_CERT_ALGORITHM = "secp256k1"

# Enable RA-TLS verification and require peer certificate for mutual TLS
loader.env.RATLS_ENABLE_VERIFY = "1"
loader.env.RATLS_REQUIRE_PEER_CERT = "1"

# Default certificate and key paths (can be overridden by environment)
loader.env.RATLS_CERT_PATH = { passthrough = true }
loader.env.RATLS_KEY_PATH = { passthrough = true }

# Contract configuration for whitelist (passthrough from host)
loader.env.CONTRACT_ADDRESS = { passthrough = true }
loader.env.RPC_URL = { passthrough = true }
loader.env.RATLS_WHITELIST_CONFIG = { passthrough = true }

# MySQL configuration (passthrough from host)
loader.env.MYSQL_DATA_DIR = { passthrough = true }
loader.env.MYSQL_ROOT_PASSWORD = { passthrough = true }

# Filesystem mounts
fs.mounts = [
  # Gramine runtime libraries
  { path = "/lib", uri = "file:{{ gramine.runtimedir() }}" },
  
  # System libraries
  { path = "/usr/lib", uri = "file:/usr/lib" },
  { path = "/usr/local/lib", uri = "file:/usr/local/lib" },
  
  # Binaries
  { path = "/usr/bin", uri = "file:/usr/bin" },
  { path = "/usr/sbin", uri = "file:/usr/sbin" },
  { path = "/usr/local/bin", uri = "file:/usr/local/bin" },
  { path = "/bin", uri = "file:/bin" },
  { path = "/sbin", uri = "file:/sbin" },
  
  # System configuration
  { path = "/etc", uri = "file:/etc" },
  
  # MySQL SSL certificate directory (writable for RA-TLS cert generation)
  { path = "/var/lib/mysql-ssl", uri = "file:/var/lib/mysql-ssl" },
  
  # MySQL runtime directory (for socket file)
  { path = "/var/run/mysqld", uri = "file:/var/run/mysqld" },
  
  # Encrypted storage for MySQL data, private keys, and sensitive files
  # MySQL data directory MUST be in encrypted partition to prevent data leakage
  { type = "encrypted", path = "/app/wallet", uri = "file:/app/wallet", key_name = "wallet_key" },
  
  # Temporary directory
  { type = "tmpfs", path = "/tmp" },
]

# SGX configuration
sgx.debug = false
sgx.enclave_size = "4G"
sgx.max_threads = 64
sgx.edmm_enable = true

# Enable DCAP attestation
sgx.remote_attestation = "dcap"

# Trusted files (integrity-checked at load time)
# Note: RA-TLS library (libratls-quote-verify.so) is automatically handled
# by the manifest compiler when LD_PRELOAD is set
sgx.trusted_files = [
  # Gramine LibOS
  "file:{{ gramine.libos }}",
  "file:{{ gramine.runtimedir() }}/",
  
  # Launcher binary (entrypoint)
  "file:{{ entrypoint }}",
  
  # MySQL server binary (target of execve)
  "file:/usr/sbin/mysqld",
  
  # System libraries needed by MySQL and launcher
  "file:/usr/lib/",
  "file:/usr/local/lib/",
  
  # MySQL configuration and support files
  "file:/etc/mysql/",
  "file:/usr/share/mysql/",
]

# Allowed files (can be modified at runtime, not integrity-checked)
# Note: MySQL data directory and logs are in encrypted partition (/app/wallet/)
# and do not need to be listed here as encrypted mounts handle their own files
sgx.allowed_files = [
  # SSL certificates directory (public certificates only, private keys in encrypted partition)
  "file:/var/lib/mysql-ssl/",
  
  # MySQL runtime directory (for socket file)
  "file:/var/run/mysqld/",
  
  # Temporary files
  "file:/tmp/",
  
  # DNS resolution (read-only system files)
  "file:/etc/hosts",
  "file:/etc/resolv.conf",
  "file:/etc/nsswitch.conf",
  
  # Note: MySQL logs are stored in encrypted partition at /app/wallet/mysql-logs
  # to prevent data leakage through log files
]
MANIFEST_EOF

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

# Data and log directories in encrypted partition to prevent data leakage
# All sensitive data MUST be stored in encrypted partition
datadir = /app/wallet/mysql-data
log_error = /app/wallet/mysql-logs/error.log
general_log_file = /app/wallet/mysql-logs/general.log
slow_query_log_file = /app/wallet/mysql-logs/slow.log

EOF

# Create necessary directories and build the Gramine manifest
# This generates the signed manifest so it's ready to run immediately
RUN mkdir -p /var/lib/mysql-ssl /var/run/mysqld \
    /app/wallet/mysql-keys /app/wallet/mysql-data /app/wallet/mysql-logs && \
    chown -R mysql:mysql /app/wallet/mysql-data /app/wallet/mysql-logs && \
    # Generate signing key for manifest
    mkdir -p /root/.config/gramine && \
    gramine-sgx-gen-private-key && \
    # Find RA-TLS library path
    RATLS_LIB_PATH=""; \
    for path in /usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so \
                /usr/local/lib/libratls-quote-verify.so \
                /usr/lib/x86_64-linux-gnu/libratls-quote-verify.so; do \
        if [ -f "$path" ]; then RATLS_LIB_PATH="$path"; break; fi; \
    done; \
    if [ -n "$RATLS_LIB_PATH" ]; then \
        export GRAMINE_LD_PRELOAD="file:${RATLS_LIB_PATH}"; \
        echo "Set GRAMINE_LD_PRELOAD=${GRAMINE_LD_PRELOAD}"; \
    fi; \
    # Generate manifest
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
    /opt/mysql-ratls/launcher/Makefile \
    /opt/mysql-ratls/launcher/mysql-ratls-launcher

# Set environment variables for RA-TLS
# Use secp256k1 curve for Ethereum compatibility
ENV RA_TLS_CERT_ALGORITHM=secp256k1
ENV RATLS_ENABLE_VERIFY=1
ENV RATLS_REQUIRE_PEER_CERT=1

# MySQL data and logs directories MUST be in encrypted partition to prevent data leakage
ENV MYSQL_DATA_DIR=/app/wallet/mysql-data
ENV MYSQL_LOG_DIR=/app/wallet/mysql-logs
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

# Set working directory to encrypted MySQL data directory
WORKDIR /app/wallet/mysql-data

# The entrypoint runs the base image's entrypoint first (starts aesmd, PCCS, etc.)
# Then runs gramine-sgx with the pre-compiled manifest to start the launcher in SGX enclave
# The launcher (inside enclave) will then execve() to mysqld
ENTRYPOINT ["/usr/local/bin/entrypoint.sh", "gramine-sgx", "mysql-ratls"]
CMD []
