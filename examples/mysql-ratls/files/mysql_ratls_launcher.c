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
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <curl/curl.h>

#include "cJSON.h"

/* Configuration constants */
#define MYSQLD_PATH "/usr/sbin/mysqld"
#define DEFAULT_CERT_PATH "/var/lib/mysql-ssl/server-cert.pem"
#define DEFAULT_KEY_PATH "/app/wallet/mysql-keys/server-key.pem"
#define DEFAULT_DATA_DIR "/app/wallet/mysql-data"
#define INIT_SENTINEL_FILE ".mysql_initialized"
#define INIT_SQL_FILE "init_users.sql"

/* RA-TLS library candidate paths (searched in order) */
static const char *RATLS_LIB_PATHS[] = {
    "/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so",
    "/usr/local/lib/libratls-quote-verify.so",
    "/usr/lib/x86_64-linux-gnu/libratls-quote-verify.so",
    NULL
};

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

/* Check if directory is empty or doesn't exist */
static int is_dir_empty(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 1;  /* Directory doesn't exist, treat as empty */
    }
    if (!S_ISDIR(st.st_mode)) {
        return 1;  /* Not a directory */
    }
    
    /* Check for MySQL system files that indicate initialization */
    char ibdata_path[MAX_PATH_LEN];
    snprintf(ibdata_path, sizeof(ibdata_path), "%s/ibdata1", path);
    if (file_exists(ibdata_path)) {
        return 0;  /* ibdata1 exists, not empty */
    }
    
    return 1;  /* No MySQL system files found */
}

/* Check if MySQL data directory needs initialization */
static int needs_mysql_init(const char *data_dir) {
    /* Check if the data directory has MySQL system files (ibdata1) */
    return is_dir_empty(data_dir);
}

/* Pre-initialized MySQL data template directory */
#define MYSQL_TEMPLATE_DIR "/app/mysql-init-data"

/* Copy a single file from src to dst, preserving mode */
static int copy_file(const char *src, const char *dst, mode_t mode) {
    int src_fd = -1, dst_fd = -1;
    char buf[8192];
    ssize_t nread;
    int ret = -1;
    
    src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "[Launcher] Failed to open source file %s: %s\n", src, strerror(errno));
        goto cleanup;
    }
    
    dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (dst_fd < 0) {
        fprintf(stderr, "[Launcher] Failed to create destination file %s: %s\n", dst, strerror(errno));
        goto cleanup;
    }
    
    while ((nread = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t nwritten = 0;
        while (nwritten < nread) {
            ssize_t n = write(dst_fd, buf + nwritten, nread - nwritten);
            if (n < 0) {
                fprintf(stderr, "[Launcher] Failed to write to %s: %s\n", dst, strerror(errno));
                goto cleanup;
            }
            nwritten += n;
        }
    }
    
    if (nread < 0) {
        fprintf(stderr, "[Launcher] Failed to read from %s: %s\n", src, strerror(errno));
        goto cleanup;
    }
    
    ret = 0;
    
cleanup:
    if (src_fd >= 0) close(src_fd);
    if (dst_fd >= 0) close(dst_fd);
    return ret;
}

/* Recursively copy directory tree from src_root to dst_root */
static int copy_tree(const char *src_root, const char *dst_root) {
    DIR *dir = NULL;
    struct dirent *entry;
    struct stat st;
    char src_path[MAX_PATH_LEN];
    char dst_path[MAX_PATH_LEN];
    int ret = -1;
    
    dir = opendir(src_root);
    if (!dir) {
        fprintf(stderr, "[Launcher] Failed to open directory %s: %s\n", src_root, strerror(errno));
        return -1;
    }
    
    /* Create destination directory if it doesn't exist */
    if (mkdir(dst_root, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[Launcher] Failed to create directory %s: %s\n", dst_root, strerror(errno));
        goto cleanup;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(src_path, sizeof(src_path), "%s/%s", src_root, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, entry->d_name);
        
        if (lstat(src_path, &st) != 0) {
            fprintf(stderr, "[Launcher] Failed to stat %s: %s\n", src_path, strerror(errno));
            goto cleanup;
        }
        
        if (S_ISDIR(st.st_mode)) {
            /* Recursively copy subdirectory */
            if (copy_tree(src_path, dst_path) != 0) {
                goto cleanup;
            }
        } else if (S_ISREG(st.st_mode)) {
            /* Copy regular file */
            if (copy_file(src_path, dst_path, st.st_mode) != 0) {
                goto cleanup;
            }
        } else if (S_ISLNK(st.st_mode)) {
            /* Handle symlinks - read link target and create new symlink */
            char link_target[MAX_PATH_LEN];
            ssize_t len = readlink(src_path, link_target, sizeof(link_target) - 1);
            if (len < 0) {
                fprintf(stderr, "[Launcher] Failed to read symlink %s: %s\n", src_path, strerror(errno));
                goto cleanup;
            }
            link_target[len] = '\0';
            
            /* Remove existing symlink if any */
            unlink(dst_path);
            
            if (symlink(link_target, dst_path) != 0) {
                fprintf(stderr, "[Launcher] Failed to create symlink %s -> %s: %s\n", dst_path, link_target, strerror(errno));
                goto cleanup;
            }
        } else {
            fprintf(stderr, "[Launcher] Skipping unsupported file type: %s\n", src_path);
        }
    }
    
    ret = 0;
    
cleanup:
    if (dir) closedir(dir);
    return ret;
}

/* Copy pre-initialized MySQL data from template to encrypted partition */
static int copy_mysql_template_data(const char *data_dir) {
    printf("[Launcher] Copying MySQL template data from %s to %s\n", MYSQL_TEMPLATE_DIR, data_dir);
    
    /* Check if template directory exists */
    struct stat st;
    if (stat(MYSQL_TEMPLATE_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[Launcher] ERROR: MySQL template directory not found: %s\n", MYSQL_TEMPLATE_DIR);
        fprintf(stderr, "[Launcher] This directory should be created during Docker build\n");
        return -1;
    }
    
    /* Check if template has ibdata1 (indicates valid initialization) */
    char ibdata_path[MAX_PATH_LEN];
    snprintf(ibdata_path, sizeof(ibdata_path), "%s/ibdata1", MYSQL_TEMPLATE_DIR);
    if (!file_exists(ibdata_path)) {
        fprintf(stderr, "[Launcher] ERROR: MySQL template directory is not properly initialized\n");
        fprintf(stderr, "[Launcher] Missing: %s\n", ibdata_path);
        return -1;
    }
    
    /* Copy the entire template directory to the data directory */
    if (copy_tree(MYSQL_TEMPLATE_DIR, data_dir) != 0) {
        fprintf(stderr, "[Launcher] ERROR: Failed to copy MySQL template data\n");
        return -1;
    }
    
    printf("[Launcher] MySQL template data copied successfully\n");
    return 0;
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

/* Find the RA-TLS library by searching candidate paths */
static const char *find_ratls_library(void) {
    for (int i = 0; RATLS_LIB_PATHS[i] != NULL; i++) {
        if (file_exists(RATLS_LIB_PATHS[i])) {
            return RATLS_LIB_PATHS[i];
        }
    }
    return NULL;
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
    
    /* Create key directory (encrypted partition provides protection) */
    printf("[Launcher] Creating key directory: %s\n", key_dir);
    if (mkdir_p(key_dir) != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create key directory: %s\n", strerror(errno));
    }
    
    /* Create data directory inside the enclave (encrypted partition) */
    const char *data_dir_env = getenv("MYSQL_DATA_DIR");
    const char *data_dir_to_create = data_dir_env ? data_dir_env : DEFAULT_DATA_DIR;
    printf("[Launcher] Creating data directory: %s\n", data_dir_to_create);
    if (mkdir_p(data_dir_to_create) != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create data directory: %s\n", strerror(errno));
    }
    
    /* Create logs directory outside encrypted partition for debugging visibility */
    /* Note: This is a trade-off - logs are readable but may contain sensitive query data */
    printf("[Launcher] Creating logs directory: /var/log/mysql\n");
    if (mkdir_p("/var/log/mysql") != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create logs directory: %s\n", strerror(errno));
    }
    
    /* Check if MySQL data directory needs initialization */
    /* Instead of running mysqld --initialize-insecure inside the enclave (which was failing),
     * we copy pre-initialized data from a template directory that was created during Docker build.
     * This approach:
     * 1. Avoids running initialization inside the enclave (more reliable)
     * 2. Avoids forking child processes inside the enclave (better performance)
     * 3. The copied data gets automatically encrypted by Gramine's encrypted filesystem
     */
    if (needs_mysql_init(data_dir_to_create)) {
        printf("[Launcher] MySQL data directory is empty or missing system files\n");
        if (copy_mysql_template_data(data_dir_to_create) != 0) {
            fprintf(stderr, "[Launcher] ERROR: Failed to copy MySQL template data, cannot continue\n");
            return 1;
        }
    } else {
        printf("[Launcher] MySQL data directory already initialized (ibdata1 exists)\n");
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
    /* We need: mysqld --datadir=... --ssl-cert=... --ssl-key=... --require-secure-transport=ON --log-error=... --console [--init-file=...] [user args] */
    /* Note: No --user=mysql since we run as root in container (user said it's not needed) */
    /* Note: --log-error is needed to override any config file setting that points to encrypted partition */
    /* Note: --console outputs logs to stderr for easier debugging in container environments */
    
    int extra_args = first_boot ? 8 : 7;  /* Add 1 for --init-file on first boot, +1 for --log-error, +1 for --console */
    int new_argc = argc + extra_args;
    char **new_argv = malloc((new_argc + 1) * sizeof(char *));
    if (!new_argv) {
        fprintf(stderr, "[Launcher] Failed to allocate memory for arguments\n");
        return 1;
    }
    
    /* Build SSL argument strings */
    char ssl_cert_arg[MAX_PATH_LEN];
    char ssl_key_arg[MAX_PATH_LEN];
    char datadir_arg[MAX_PATH_LEN];
    
    snprintf(ssl_cert_arg, sizeof(ssl_cert_arg), "--ssl-cert=%s", cert_path);
    snprintf(ssl_key_arg, sizeof(ssl_key_arg), "--ssl-key=%s", key_path);
    snprintf(datadir_arg, sizeof(datadir_arg), "--datadir=%s", data_dir);
    
    int idx = 0;
    new_argv[idx++] = MYSQLD_PATH;
    new_argv[idx++] = datadir_arg;
    new_argv[idx++] = ssl_cert_arg;
    new_argv[idx++] = ssl_key_arg;
    new_argv[idx++] = "--require-secure-transport=ON";
    new_argv[idx++] = "--log-error=/var/log/mysql/error.log";  /* Override config file setting */
    new_argv[idx++] = "--console";  /* Output logs to stderr for easier debugging */
    
    /* Add --init-file on first boot */
    if (first_boot && init_file_arg[0] != '\0') {
        new_argv[idx++] = init_file_arg;
    }
    
    /* Copy any additional arguments from command line */
    for (int i = 1; i < argc; i++) {
        new_argv[idx++] = argv[i];
    }
    new_argv[idx] = NULL;
    
    /* Set LD_PRELOAD for mysqld to load the RA-TLS library */
    /* The launcher does NOT use LD_PRELOAD itself; only mysqld needs it */
    /* This allows the launcher to run without RA-TLS hooks, while mysqld gets them */
    const char *ratls_lib = find_ratls_library();
    if (ratls_lib) {
        printf("[Launcher] Found RA-TLS library: %s\n", ratls_lib);
        set_env("LD_PRELOAD", ratls_lib, 1); 
    } else {
        fprintf(stderr, "[Launcher] Warning: RA-TLS library not found in any candidate path\n");
        fprintf(stderr, "[Launcher] MySQL will start without RA-TLS injection\n");
    }
    
    printf("[Launcher] Executing: %s\n", MYSQLD_PATH);
    printf("[Launcher]   Data directory: %s\n", data_dir);
    printf("[Launcher]   Certificate: %s\n", cert_path);
    printf("[Launcher]   Private key: %s\n", key_path);
    printf("[Launcher]   Log file: /var/log/mysql/error.log\n");
    printf("[Launcher]   Log output: console (stderr) + file\n");
    if (ratls_lib) {
        printf("[Launcher]   LD_PRELOAD: %s\n", ratls_lib);
    }
    if (first_boot && init_file_arg[0] != '\0') {
        printf("[Launcher]   Init file: %s\n", init_sql_path);
    }
    printf("\n");
    
    /* Use execv to replace this process with mysqld */
    /* execv preserves the environment variables we set (including LD_PRELOAD) */
    execv(MYSQLD_PATH, new_argv);
    
    /* If we get here, execve failed */
    fprintf(stderr, "[Launcher] Failed to execute %s: %s\n", MYSQLD_PATH, strerror(errno));
    free(new_argv);
    
    return 1;
}
