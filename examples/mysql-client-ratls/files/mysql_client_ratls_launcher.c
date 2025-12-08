/*
 * MySQL RA-TLS Client Launcher
 * 
 * This program runs inside a Gramine SGX enclave and:
 * 1. Reads whitelist configuration from a smart contract (if CONTRACT_ADDRESS is set)
 * 2. Sets up RA-TLS environment variables
 * 3. Sets LD_PRELOAD for RA-TLS injection
 * 4. Uses execve() to replace itself with Node.js running the MySQL client
 *
 * The launcher avoids creating child processes in the enclave by using execve()
 * to directly replace the current process with Node.js.
 * 
 * IMPORTANT: The launcher itself does NOT have LD_PRELOAD set in the manifest.
 * It sets LD_PRELOAD just before execve() so only Node.js gets RA-TLS injection.
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
#define DEFAULT_CERT_PATH "/var/lib/mysql-client-ssl/client-cert.pem"
#define DEFAULT_KEY_PATH "/app/wallet/mysql-client-keys/client-key.pem"
#define CLIENT_SCRIPT_PATH "/app/mysql-client.js"

/* Node.js binary candidate paths (searched in order) */
static const char *NODE_PATHS[] = {
    "/opt/node-install/bin/node",
    "/usr/local/bin/node",
    "/usr/bin/node",
    NULL
};

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
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    res = curl_easy_perform(curl);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        fprintf(stderr, "[Launcher] curl failed: %s\n", curl_easy_strerror(res));
        free(response.data);
        return NULL;
    }
    
    /* Parse JSON response */
    cJSON *json = cJSON_Parse(response.data);
    free(response.data);
    
    if (!json) {
        fprintf(stderr, "[Launcher] Failed to parse JSON response\n");
        return NULL;
    }
    
    cJSON *result_obj = cJSON_GetObjectItem(json, "result");
    if (result_obj && cJSON_IsString(result_obj)) {
        result = strdup(result_obj->valuestring);
    } else {
        cJSON *error_obj = cJSON_GetObjectItem(json, "error");
        if (error_obj) {
            cJSON *msg = cJSON_GetObjectItem(error_obj, "message");
            if (msg && cJSON_IsString(msg)) {
                fprintf(stderr, "[Launcher] RPC error: %s\n", msg->valuestring);
            }
        }
    }
    
    cJSON_Delete(json);
    return result;
}

/* Read whitelist configuration from smart contract */
static char *read_whitelist_from_contract(const char *rpc_url, const char *contract_address) {
    char *result = NULL;
    char *json_str = NULL;
    
    printf("[Launcher] Reading whitelist from contract %s\n", contract_address);
    
    /* Call getSGXConfig() */
    char *hex_result = eth_call(rpc_url, contract_address, GET_SGX_CONFIG_SELECTOR);
    if (!hex_result) {
        fprintf(stderr, "[Launcher] Failed to call contract\n");
        return NULL;
    }
    
    /* Check for empty result */
    if (strcmp(hex_result, "0x") == 0 || strlen(hex_result) < 4) {
        printf("[Launcher] Contract returned empty result, no whitelist configured\n");
        free(hex_result);
        return NULL;
    }
    
    /* Decode ABI-encoded string */
    json_str = decode_abi_string(hex_result);
    free(hex_result);
    
    if (!json_str) {
        fprintf(stderr, "[Launcher] Failed to decode ABI string\n");
        return NULL;
    }
    
    /* Parse JSON to extract RATLS_WHITELIST_CONFIG */
    cJSON *json = cJSON_Parse(json_str);
    free(json_str);
    
    if (!json) {
        fprintf(stderr, "[Launcher] Failed to parse config JSON\n");
        return NULL;
    }
    
    cJSON *whitelist = cJSON_GetObjectItem(json, "RATLS_WHITELIST_CONFIG");
    if (whitelist && cJSON_IsString(whitelist)) {
        result = strdup(whitelist->valuestring);
        printf("[Launcher] Successfully read whitelist from contract\n");
    } else {
        printf("[Launcher] Contract response does not contain RATLS_WHITELIST_CONFIG\n");
    }
    
    cJSON_Delete(json);
    return result;
}

/* Set environment variable */
static void set_env(const char *name, const char *value) {
    if (setenv(name, value, 1) != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to set %s: %s\n", name, strerror(errno));
    }
}

/* Set environment variable if not already set */
static void set_env_default(const char *name, const char *value) {
    if (getenv(name) == NULL) {
        set_env(name, value);
    }
}

/* Find first existing file from a list of paths */
static const char *find_first_existing(const char **paths) {
    for (int i = 0; paths[i] != NULL; i++) {
        if (file_exists(paths[i])) {
            return paths[i];
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    printf("[Launcher] MySQL RA-TLS Client Launcher starting...\n");
    
    /* Set RA-TLS configuration environment variables */
    /* These are set by the launcher to ensure consistent configuration */
    /* Reference: mccoysc/gramine tools/sgx/ra-tls/CERTIFICATE_CONFIGURATION.md */
    
    /* Use secp256k1 curve for Ethereum compatibility */
    set_env("RA_TLS_CERT_ALGORITHM", "secp256k1");
    
    /* Enable RA-TLS verification and require peer certificate for mutual TLS */
    set_env("RATLS_ENABLE_VERIFY", "1");
    set_env("RATLS_REQUIRE_PEER_CERT", "1");
    
    /* Set default certificate and key paths if not already set */
    set_env_default("RATLS_CERT_PATH", DEFAULT_CERT_PATH);
    set_env_default("RATLS_KEY_PATH", DEFAULT_KEY_PATH);
    
    /* Get configuration from environment variables */
    const char *contract_address = getenv("CONTRACT_ADDRESS");
    const char *rpc_url = getenv("RPC_URL");
    const char *cert_path = getenv("RATLS_CERT_PATH");
    const char *key_path = getenv("RATLS_KEY_PATH");
    
    /* Create directories for certificates and keys */
    char cert_dir[MAX_PATH_LEN];
    char key_dir[MAX_PATH_LEN];
    get_dirname(cert_path, cert_dir, sizeof(cert_dir));
    get_dirname(key_path, key_dir, sizeof(key_dir));
    
    printf("[Launcher] Creating certificate directory: %s\n", cert_dir);
    if (mkdir_p(cert_dir) != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create certificate directory: %s\n", strerror(errno));
    }
    
    printf("[Launcher] Creating key directory: %s\n", key_dir);
    if (mkdir_p(key_dir) != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create key directory: %s\n", strerror(errno));
    }
    
    /* Read whitelist from contract if configured */
    if (contract_address && rpc_url) {
        char *whitelist = read_whitelist_from_contract(rpc_url, contract_address);
        if (whitelist) {
            set_env("RATLS_WHITELIST_CONFIG", whitelist);
            printf("[Launcher] Whitelist configuration set from contract\n");
            free(whitelist);
        }
    } else if (contract_address && !rpc_url) {
        fprintf(stderr, "[Launcher] Warning: CONTRACT_ADDRESS is set but RPC_URL is not set, skipping whitelist read\n");
    }
    
    /* Find RA-TLS library */
    const char *ratls_lib = find_first_existing(RATLS_LIB_PATHS);
    if (!ratls_lib) {
        fprintf(stderr, "[Launcher] ERROR: RA-TLS library not found\n");
        fprintf(stderr, "[Launcher] Searched paths:\n");
        for (int i = 0; RATLS_LIB_PATHS[i] != NULL; i++) {
            fprintf(stderr, "[Launcher]   - %s\n", RATLS_LIB_PATHS[i]);
        }
        return 1;
    }
    printf("[Launcher] Found RA-TLS library: %s\n", ratls_lib);
    
    /* Find Node.js binary */
    const char *node_path = find_first_existing(NODE_PATHS);
    if (!node_path) {
        fprintf(stderr, "[Launcher] ERROR: Node.js binary not found\n");
        fprintf(stderr, "[Launcher] Searched paths:\n");
        for (int i = 0; NODE_PATHS[i] != NULL; i++) {
            fprintf(stderr, "[Launcher]   - %s\n", NODE_PATHS[i]);
        }
        return 1;
    }
    printf("[Launcher] Found Node.js binary: %s\n", node_path);
    
    /* Check if client script exists */
    if (!file_exists(CLIENT_SCRIPT_PATH)) {
        fprintf(stderr, "[Launcher] ERROR: Client script not found: %s\n", CLIENT_SCRIPT_PATH);
        return 1;
    }
    
    /* Set LD_PRELOAD for RA-TLS injection */
    /* This is set just before execve() so only Node.js gets the injection */
    printf("[Launcher] Setting LD_PRELOAD=%s\n", ratls_lib);
    set_env("LD_PRELOAD", ratls_lib);
    
    /* Log RA-TLS configuration */
    printf("[Launcher] RA-TLS Configuration:\n");
    printf("[Launcher]   - Certificate path: %s\n", cert_path);
    printf("[Launcher]   - Key path: %s\n", key_path);
    printf("[Launcher]   - Verification enabled: %s\n", getenv("RATLS_ENABLE_VERIFY") ?: "1");
    printf("[Launcher]   - Require peer cert: %s\n", getenv("RATLS_REQUIRE_PEER_CERT") ?: "1");
    
    /* Build argv for Node.js */
    /* argv[0] = node, argv[1] = script path, then any additional args */
    int new_argc = 2 + (argc > 1 ? argc - 1 : 0);
    char **new_argv = malloc((new_argc + 1) * sizeof(char *));
    if (!new_argv) {
        fprintf(stderr, "[Launcher] ERROR: Out of memory\n");
        return 1;
    }
    
    new_argv[0] = (char *)node_path;
    new_argv[1] = CLIENT_SCRIPT_PATH;
    
    /* Pass through any additional arguments */
    for (int i = 1; i < argc; i++) {
        new_argv[1 + i] = argv[i];
    }
    new_argv[new_argc] = NULL;
    
    printf("[Launcher] Executing: %s %s\n", node_path, CLIENT_SCRIPT_PATH);
    
    /* Replace this process with Node.js */
    execve(node_path, new_argv, environ);
    
    /* If we get here, execve failed */
    fprintf(stderr, "[Launcher] ERROR: execve failed: %s\n", strerror(errno));
    free(new_argv);
    return 1;
}
