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
    
    /* Build argument list for mysqld */
    /* We need: mysqld --user=mysql --datadir=... --ssl-ca=... --ssl-cert=... --ssl-key=... --require-secure-transport=ON [user args] */
    
    int new_argc = argc + 7;  /* Original args + our SSL args */
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
    printf("\n");
    
    /* Use execve to replace this process with mysqld */
    /* This preserves the environment variables we set */
    execv(MYSQLD_PATH, new_argv);
    
    /* If we get here, execve failed */
    fprintf(stderr, "[Launcher] Failed to execute %s: %s\n", MYSQLD_PATH, strerror(errno));
    free(new_argv);
    
    return 1;
}
