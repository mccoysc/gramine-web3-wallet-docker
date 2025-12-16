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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#include "cJSON.h"

/* Configuration constants */
#define MYSQLD_PATH "/usr/sbin/mysqld"
#define DEFAULT_CERT_PATH "/var/lib/mysql-ssl/server-cert.pem"
#define DEFAULT_KEY_PATH "/app/wallet/mysql-keys/server-key.pem"
#define DEFAULT_DATA_DIR "/app/wallet/mysql-data"
#define INIT_SENTINEL_FILE ".mysql_initialized"
#define INIT_SQL_FILE "init_users.sql"

/* Group Replication constants */
#define GR_CONFIG_FILE "/var/lib/mysql/mysql-gr.cnf"
#define GR_DEFAULT_PORT 33061
#define GR_SERVER_ID_FILE "/app/wallet/.mysql_server_id"
#define GR_GROUP_NAME_FILE "/app/wallet/.mysql_gr_group_name"
#define GR_GROUP_NAME_PLAINTEXT_FILE "/var/lib/mysql/gr_group_name.txt"  /* Plaintext copy for ops */
#define PUBLIC_IP_URL "https://ifconfig.me/ip"
#define MAX_SEEDS_LEN 4096
#define MAX_IP_LEN 64
#define UUID_LEN 36  /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */

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

/* Forward declarations */
struct launcher_config;
static void print_usage(const char *prog_name);
static void parse_args(int argc, char *argv[], struct launcher_config *config);

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

/* Create the initialization SQL file for first boot (non-GR mode) */
static int create_init_sql(const char *data_dir, char *init_sql_path, size_t path_size) {
    snprintf(init_sql_path, path_size, "%s/%s", data_dir, INIT_SQL_FILE);
    
    FILE *f = fopen(init_sql_path, "w");
    if (!f) {
        fprintf(stderr, "[Launcher] Failed to create init SQL file: %s\n", strerror(errno));
        return -1;
    }
    
    /* Build SQL content in memory so we can both write and print it */
    char sql_content[4096];
    int offset = 0;
    
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- MySQL RA-TLS User Initialization\n"
        "-- This file is executed on first boot inside the SGX enclave\n"
        "-- Users are configured with REQUIRE X509 (certificate-only authentication)\n"
        "-- RA-TLS handles the actual SGX attestation verification\n\n");
    
    /* Create application user with X509 requirement */
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- Create application user that requires X.509 certificate\n"
        "CREATE USER IF NOT EXISTS 'app'@'%%' IDENTIFIED BY '' REQUIRE X509;\n"
        "GRANT ALL PRIVILEGES ON *.* TO 'app'@'%%' WITH GRANT OPTION;\n\n");
    
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "FLUSH PRIVILEGES;\n");
    
    /* Write to file */
    fprintf(f, "%s", sql_content);
    fclose(f);
    
    /* Print the generated SQL */
    printf("[Launcher] Created init SQL file: %s\n", init_sql_path);
    printf("[Launcher] ========== Init SQL Content ==========\n");
    printf("%s", sql_content);
    printf("[Launcher] ======================================\n");
    
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

/* ============================================
 * Group Replication Helper Functions
 * ============================================ */

/* Get LAN IP address using UDP connect trick */
static int get_lan_ip(char *ip_buf, size_t buf_size) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[Launcher] Failed to create socket for LAN IP detection: %s\n", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(53);  /* DNS port */
    inet_pton(AF_INET, "8.8.8.8", &serv_addr.sin_addr);
    
    /* Connect (doesn't actually send data for UDP) */
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "[Launcher] Failed to connect for LAN IP detection: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sock, (struct sockaddr *)&local_addr, &addr_len) < 0) {
        fprintf(stderr, "[Launcher] Failed to get local address: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    
    close(sock);
    
    if (inet_ntop(AF_INET, &local_addr.sin_addr, ip_buf, buf_size) == NULL) {
        fprintf(stderr, "[Launcher] Failed to convert IP to string: %s\n", strerror(errno));
        return -1;
    }
    
    printf("[Launcher] Detected LAN IP: %s\n", ip_buf);
    return 0;
}

/* Get public IP address using libcurl */
static int get_public_ip(char *ip_buf, size_t buf_size) {
    CURL *curl;
    CURLcode res;
    struct curl_response response = {0};
    int ret = -1;
    
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[Launcher] Failed to initialize curl for public IP detection\n");
        return -1;
    }
    
    response.data = malloc(1);
    if (!response.data) {
        curl_easy_cleanup(curl);
        return -1;
    }
    response.data[0] = '\0';
    response.size = 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, PUBLIC_IP_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        fprintf(stderr, "[Launcher] Failed to get public IP: %s\n", curl_easy_strerror(res));
        goto cleanup;
    }
    
    /* Trim whitespace from response */
    char *start = response.data;
    while (*start && (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t')) {
        start++;
    }
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) {
        *end = '\0';
        end--;
    }
    
    if (strlen(start) == 0 || strlen(start) >= buf_size) {
        fprintf(stderr, "[Launcher] Invalid public IP response\n");
        goto cleanup;
    }
    
    strncpy(ip_buf, start, buf_size - 1);
    ip_buf[buf_size - 1] = '\0';
    
    printf("[Launcher] Detected public IP: %s\n", ip_buf);
    ret = 0;
    
cleanup:
    curl_easy_cleanup(curl);
    free(response.data);
    return ret;
}

/* Get or create stable server_id based on IP hash */
static unsigned int get_or_create_server_id(const char *lan_ip, const char *public_ip) {
    unsigned int server_id = 0;
    
    /* Try to read existing server_id from file */
    FILE *f = fopen(GR_SERVER_ID_FILE, "r");
    if (f) {
        if (fscanf(f, "%u", &server_id) == 1 && server_id > 0) {
            fclose(f);
            printf("[Launcher] Loaded existing server_id: %u\n", server_id);
            return server_id;
        }
        fclose(f);
    }
    
    /* Generate new server_id based on hash of IPs */
    unsigned int hash = 5381;
    const char *str = lan_ip;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    str = public_ip;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    
    /* Ensure server_id is in valid range (1 to 2^32-1) and not 0 */
    server_id = (hash % 4294967294) + 1;
    
    /* Save server_id to file */
    f = fopen(GR_SERVER_ID_FILE, "w");
    if (f) {
        fprintf(f, "%u\n", server_id);
        fclose(f);
        printf("[Launcher] Created new server_id: %u (saved to %s)\n", server_id, GR_SERVER_ID_FILE);
    } else {
        printf("[Launcher] Created new server_id: %u (could not save to file)\n", server_id);
    }
    
    return server_id;
}

/* Generate a random UUID v4 string */
static void generate_uuid(char *uuid_buf, size_t buf_size) {
    if (buf_size < UUID_LEN + 1) {
        uuid_buf[0] = '\0';
        return;
    }
    
    /* Read random bytes from /dev/urandom */
    unsigned char random_bytes[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t read = fread(random_bytes, 1, 16, f);
        fclose(f);
        if (read != 16) {
            /* Fallback to time-based seed if urandom fails */
            srand((unsigned int)time(NULL) ^ getpid());
            for (int i = 0; i < 16; i++) {
                random_bytes[i] = rand() & 0xFF;
            }
        }
    } else {
        /* Fallback to time-based seed */
        srand((unsigned int)time(NULL) ^ getpid());
        for (int i = 0; i < 16; i++) {
            random_bytes[i] = rand() & 0xFF;
        }
    }
    
    /* Set version (4) and variant (RFC 4122) bits */
    random_bytes[6] = (random_bytes[6] & 0x0F) | 0x40;  /* Version 4 */
    random_bytes[8] = (random_bytes[8] & 0x3F) | 0x80;  /* Variant RFC 4122 */
    
    /* Format as UUID string: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    snprintf(uuid_buf, buf_size,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3],
        random_bytes[4], random_bytes[5],
        random_bytes[6], random_bytes[7],
        random_bytes[8], random_bytes[9],
        random_bytes[10], random_bytes[11], random_bytes[12], random_bytes[13],
        random_bytes[14], random_bytes[15]);
}

/* Write group name to plaintext file for ops visibility */
static void write_plaintext_group_name(const char *group_name) {
    FILE *f = fopen(GR_GROUP_NAME_PLAINTEXT_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", group_name);
        fclose(f);
        printf("[Launcher] Written plaintext group name to %s (for ops)\n", GR_GROUP_NAME_PLAINTEXT_FILE);
    } else {
        fprintf(stderr, "[Launcher] Warning: Could not write plaintext group name to %s: %s\n", 
                GR_GROUP_NAME_PLAINTEXT_FILE, strerror(errno));
    }
    fflush(stdout);
    fflush(stderr);
}

/* Get or create Group Replication group name
 * Priority: 1. CLI argument (already in config)
 *           2. Environment variable MYSQL_GR_GROUP_NAME
 *           3. Persisted file in encrypted partition
 *           4. Auto-generate new UUID (GR enabled by default)
 * Returns: pointer to static buffer with group name (always returns valid name)
 */
static const char *get_or_create_gr_group_name(const char *cli_group_name, int is_bootstrap) {
    static char group_name_buf[UUID_LEN + 1];
    (void)is_bootstrap;  /* is_bootstrap is now only used for GR bootstrap SQL, not for enabling GR */
    
    /* Priority 1: CLI argument (already checked by caller, but double-check) */
    if (cli_group_name && strlen(cli_group_name) > 0) {
        strncpy(group_name_buf, cli_group_name, UUID_LEN);
        group_name_buf[UUID_LEN] = '\0';
        printf("[Launcher] Using group name from command line: %s\n", group_name_buf);
        fflush(stdout);
        write_plaintext_group_name(group_name_buf);
        return group_name_buf;
    }
    
    /* Priority 2: Environment variable */
    const char *env_group_name = getenv("MYSQL_GR_GROUP_NAME");
    if (env_group_name && strlen(env_group_name) > 0) {
        strncpy(group_name_buf, env_group_name, UUID_LEN);
        group_name_buf[UUID_LEN] = '\0';
        printf("[Launcher] Using group name from environment variable: %s\n", group_name_buf);
        fflush(stdout);
        
        /* Also persist it for future use */
        FILE *f = fopen(GR_GROUP_NAME_FILE, "w");
        if (f) {
            fprintf(f, "%s\n", group_name_buf);
            fclose(f);
            printf("[Launcher] Persisted group name to %s\n", GR_GROUP_NAME_FILE);
        }
        write_plaintext_group_name(group_name_buf);
        return group_name_buf;
    }
    
    /* Priority 3: Persisted file */
    FILE *f = fopen(GR_GROUP_NAME_FILE, "r");
    if (f) {
        if (fgets(group_name_buf, sizeof(group_name_buf), f)) {
            /* Remove trailing newline */
            size_t len = strlen(group_name_buf);
            if (len > 0 && group_name_buf[len-1] == '\n') {
                group_name_buf[len-1] = '\0';
            }
            if (strlen(group_name_buf) > 0) {
                fclose(f);
                printf("[Launcher] Using group name from persisted file: %s\n", group_name_buf);
                fflush(stdout);
                write_plaintext_group_name(group_name_buf);
                return group_name_buf;
            }
        }
        fclose(f);
    }
    
    /* Priority 4: Auto-generate new UUID (GR is enabled by default) */
    generate_uuid(group_name_buf, sizeof(group_name_buf));
    printf("[Launcher] Auto-generated new group name: %s\n", group_name_buf);
    fflush(stdout);
    
    /* Persist the generated UUID for future use */
    f = fopen(GR_GROUP_NAME_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", group_name_buf);
        fclose(f);
        printf("[Launcher] Persisted new group name to %s\n", GR_GROUP_NAME_FILE);
    } else {
        fprintf(stderr, "[Launcher] Warning: Could not persist group name to %s\n", GR_GROUP_NAME_FILE);
    }
    write_plaintext_group_name(group_name_buf);
    return group_name_buf;
}

/* Check if IP is already in seeds list */
/* Check if an ip:port pair is already in the seeds list (exact match) */
static int seed_in_list(const char *seeds, const char *seed_with_port) {
    if (!seeds || !seed_with_port || strlen(seeds) == 0) return 0;
    
    /* Make a copy to tokenize */
    char *seeds_copy = strdup(seeds);
    if (!seeds_copy) return 0;
    
    int found = 0;
    char *saveptr;
    char *token = strtok_r(seeds_copy, ",", &saveptr);
    while (token) {
        /* Trim whitespace */
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') {
            *end = '\0';
            end--;
        }
        
        /* Exact match comparison */
        if (strcmp(token, seed_with_port) == 0) {
            found = 1;
            break;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    
    free(seeds_copy);
    return found;
}

/* Build deduplicated seeds list from self IPs and additional seeds */
static int build_seeds_list(char *seeds_buf, size_t buf_size,
                            const char *lan_ip, const char *public_ip,
                            const char *extra_seeds, int gr_port) {
    seeds_buf[0] = '\0';
    size_t len = 0;
    
    /* Add LAN IP if valid */
    if (lan_ip && strlen(lan_ip) > 0) {
        len += snprintf(seeds_buf + len, buf_size - len, "%s:%d", lan_ip, gr_port);
    }
    
    /* Add public IP if valid and different from LAN IP */
    if (public_ip && strlen(public_ip) > 0) {
        if (!lan_ip || strcmp(lan_ip, public_ip) != 0) {
            if (len > 0) {
                len += snprintf(seeds_buf + len, buf_size - len, ",");
            }
            len += snprintf(seeds_buf + len, buf_size - len, "%s:%d", public_ip, gr_port);
        }
    }
    
    /* Add extra seeds (deduplicated) */
    if (extra_seeds && strlen(extra_seeds) > 0) {
        char *extra_copy = strdup(extra_seeds);
        if (!extra_copy) return -1;
        
        char *saveptr;
        char *token = strtok_r(extra_copy, ",", &saveptr);
        while (token) {
            /* Trim whitespace */
            while (*token == ' ') token++;
            char *end = token + strlen(token) - 1;
            while (end > token && *end == ' ') {
                *end = '\0';
                end--;
            }
            
            if (strlen(token) > 0) {
                /* Check if token has port, if not add default port */
                char seed_with_port[MAX_IP_LEN + 16];
                if (strchr(token, ':') == NULL) {
                    snprintf(seed_with_port, sizeof(seed_with_port), "%s:%d", token, gr_port);
                } else {
                    strncpy(seed_with_port, token, sizeof(seed_with_port) - 1);
                    seed_with_port[sizeof(seed_with_port) - 1] = '\0';
                }
                
                /* Check if already in seeds list (exact ip:port pair deduplication) */
                if (!seed_in_list(seeds_buf, seed_with_port)) {
                    if (len > 0) {
                        len += snprintf(seeds_buf + len, buf_size - len, ",");
                    }
                    len += snprintf(seeds_buf + len, buf_size - len, "%s", seed_with_port);
                }
            }
            
            token = strtok_r(NULL, ",", &saveptr);
        }
        
        free(extra_copy);
    }
    
    printf("[Launcher] Built seeds list: %s\n", seeds_buf);
    return 0;
}

/* Create Group Replication configuration file */
static int create_gr_config(const char *config_path, unsigned int server_id,
                            const char *group_name, const char *local_address,
                            const char *seeds, int is_bootstrap,
                            const char *cert_path, const char *key_path) {
    FILE *f = fopen(config_path, "w");
    if (!f) {
        fprintf(stderr, "[Launcher] Failed to create GR config file %s: %s\n", config_path, strerror(errno));
        return -1;
    }
    
    /* Build config content in memory so we can both write and print it */
    char config_content[8192];
    int offset = 0;
    
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "# MySQL Group Replication Configuration\n"
        "# Generated by mysql-ratls-launcher\n\n"
        "[mysqld]\n");
    
    /* Server identification */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "server_id=%u\n", server_id);
    
    /* GTID settings (required for GR) */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "gtid_mode=ON\n"
        "enforce_gtid_consistency=ON\n");
    
    /* Binary logging (required for GR) */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "log_bin=binlog\n"
        "binlog_format=ROW\n"
        "binlog_checksum=NONE\n");
    
    /* Replication info repositories */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "master_info_repository=TABLE\n"
        "relay_log_info_repository=TABLE\n");
    
    /* Transaction write set extraction */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "transaction_write_set_extraction=XXHASH64\n");
    
    /* Group Replication plugin settings */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "\n# Group Replication Settings\n"
        "plugin_load_add=group_replication.so\n"
        "loose-group_replication_group_name=%s\n"
        "loose-group_replication_local_address=%s\n"
        "loose-group_replication_group_seeds=%s\n"
        "loose-group_replication_start_on_boot=OFF\n"
        "loose-group_replication_bootstrap_group=OFF\n",
        group_name, local_address, seeds);
    
    /* Multi-primary mode (mutual primary-replica) */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "loose-group_replication_single_primary_mode=OFF\n"
        "loose-group_replication_enforce_update_everywhere_checks=ON\n");
    
    /* Recovery channel SSL settings (use same certs as main connection) */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "\n# Recovery Channel SSL Settings\n"
        "loose-group_replication_recovery_use_ssl=ON\n"
        "loose-group_replication_recovery_ssl_cert=%s\n"
        "loose-group_replication_recovery_ssl_key=%s\n"
        "loose-group_replication_recovery_ssl_verify_server_cert=OFF\n",
        cert_path, key_path);
    
    /* IP allowlist - allow all private networks and public IPs */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "\n# IP Allowlist\n"
        "loose-group_replication_ip_allowlist=AUTOMATIC\n");
    
    /* Suppress unused variable warning */
    (void)is_bootstrap;
    
    /* Write to file */
    fprintf(f, "%s", config_content);
    fclose(f);
    
    /* Print the generated config */
    printf("[Launcher] Created GR config file: %s\n", config_path);
    printf("[Launcher] ========== GR Config Content ==========\n");
    printf("%s", config_content);
    printf("[Launcher] ======================================\n");
    fflush(stdout);
    
    return 0;
}

/* Create Group Replication init SQL (stored in encrypted partition) */
static int create_gr_init_sql(const char *data_dir, char *init_sql_path, size_t path_size,
                              int is_bootstrap, const char *cert_path, const char *key_path) {
    /* SQL file is stored in data_dir which is in encrypted partition (/app/wallet/mysql-data) */
    snprintf(init_sql_path, path_size, "%s/%s", data_dir, INIT_SQL_FILE);
    
    FILE *f = fopen(init_sql_path, "w");
    if (!f) {
        fprintf(stderr, "[Launcher] Failed to create init SQL file: %s\n", strerror(errno));
        return -1;
    }
    
    /* Build SQL content in memory so we can both write and print it */
    /* Buffer reduced since INSTALL PLUGIN logic moved to cnf file */
    char sql_content[8192];
    int offset = 0;
    
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- MySQL RA-TLS User Initialization with Group Replication\n"
        "-- This file is executed on EVERY startup inside the SGX enclave\n"
        "-- All statements are idempotent (safe to run multiple times)\n"
        "-- Users are configured with REQUIRE X509 (certificate-only authentication)\n"
        "-- RA-TLS handles the actual SGX attestation verification\n\n");
    
    /* Create application user with X509 requirement (only app user needed) */
    /* CREATE USER IF NOT EXISTS is already idempotent */
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- Create application user that requires X.509 certificate (idempotent)\n"
        "CREATE USER IF NOT EXISTS 'app'@'%%' IDENTIFIED BY '' REQUIRE X509;\n"
        "GRANT ALL PRIVILEGES ON *.* TO 'app'@'%%' WITH GRANT OPTION;\n\n");
    
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "FLUSH PRIVILEGES;\n\n");
    
    /* Note: Group Replication plugin is loaded via plugin_load_add in mysql-gr.cnf */
    /* No need for INSTALL PLUGIN here - the plugin is already loaded at startup */
    
    /* Group Replication recovery channel setup */
    /* CHANGE REPLICATION SOURCE is idempotent - it just updates the configuration */
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- Group Replication Setup\n"
        "-- Configure recovery channel to use certificate authentication (idempotent)\n"
        "CHANGE REPLICATION SOURCE TO\n"
        "  SOURCE_USER='app',\n"
        "  SOURCE_SSL=1,\n"
        "  SOURCE_SSL_CERT='%s',\n"
        "  SOURCE_SSL_KEY='%s'\n"
        "  FOR CHANNEL 'group_replication_recovery';\n\n",
        cert_path, key_path);
    
    /* Start Group Replication (idempotent - check if already running) */
    if (is_bootstrap) {
        offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
            "-- Bootstrap the group (first node) - idempotent\n"
            "-- Only bootstrap if GR is not already running\n"
            "SET @gr_running = (SELECT COUNT(*) FROM performance_schema.replication_group_members WHERE member_state = 'ONLINE');\n"
            "SET @bootstrap_sql = IF(@gr_running = 0, \n"
            "    'SET GLOBAL group_replication_bootstrap_group=ON', \n"
            "    'SELECT \"Group Replication already running, skipping bootstrap\" AS status');\n"
            "PREPARE bootstrap_stmt FROM @bootstrap_sql;\n"
            "EXECUTE bootstrap_stmt;\n"
            "DEALLOCATE PREPARE bootstrap_stmt;\n\n"
            "-- Start GR if not running\n"
            "SET @start_sql = IF(@gr_running = 0, \n"
            "    'START GROUP_REPLICATION', \n"
            "    'SELECT \"Group Replication already started\" AS status');\n"
            "PREPARE start_stmt FROM @start_sql;\n"
            "EXECUTE start_stmt;\n"
            "DEALLOCATE PREPARE start_stmt;\n\n"
            "-- Turn off bootstrap mode\n"
            "SET @unbootstrap_sql = IF(@gr_running = 0, \n"
            "    'SET GLOBAL group_replication_bootstrap_group=OFF', \n"
            "    'SELECT 1');\n"
            "PREPARE unbootstrap_stmt FROM @unbootstrap_sql;\n"
            "EXECUTE unbootstrap_stmt;\n"
            "DEALLOCATE PREPARE unbootstrap_stmt;\n");
    } else {
        offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
            "-- Join existing group - idempotent\n"
            "-- Only start GR if not already running\n"
            "SET @gr_running = (SELECT COUNT(*) FROM performance_schema.replication_group_members WHERE member_state = 'ONLINE');\n"
            "SET @start_sql = IF(@gr_running = 0, \n"
            "    'START GROUP_REPLICATION', \n"
            "    'SELECT \"Group Replication already running\" AS status');\n"
            "PREPARE start_stmt FROM @start_sql;\n"
            "EXECUTE start_stmt;\n"
            "DEALLOCATE PREPARE start_stmt;\n");
    }
    
    /* Write to file */
    fprintf(f, "%s", sql_content);
    fclose(f);
    
    /* Print the generated SQL */
    printf("[Launcher] Created GR init SQL file: %s (in encrypted partition)\n", init_sql_path);
    printf("[Launcher] ========== GR Init SQL Content ==========\n");
    printf("%s", sql_content);
    printf("[Launcher] =========================================\n");
    
    return 0;
}

/* Structure to hold all launcher configuration options */
struct launcher_config {
    /* Existing environment variables - can be overridden by args */
    const char *contract_address;      /* CONTRACT_ADDRESS */
    const char *rpc_url;               /* RPC_URL */
    const char *whitelist_config;      /* RATLS_WHITELIST_CONFIG */
    const char *cert_path;             /* RATLS_CERT_PATH */
    const char *key_path;              /* RATLS_KEY_PATH */
    const char *data_dir;              /* MYSQL_DATA_DIR */
    
    /* RA-TLS configuration (from manifest, can be overridden) */
    const char *ra_tls_cert_algorithm;           /* RA_TLS_CERT_ALGORITHM */
    const char *ratls_enable_verify;             /* RATLS_ENABLE_VERIFY */
    const char *ratls_require_peer_cert;         /* RATLS_REQUIRE_PEER_CERT */
    const char *ra_tls_allow_outdated_tcb;       /* RA_TLS_ALLOW_OUTDATED_TCB_INSECURE */
    const char *ra_tls_allow_hw_config_needed;   /* RA_TLS_ALLOW_HW_CONFIG_NEEDED */
    const char *ra_tls_allow_sw_hardening_needed;/* RA_TLS_ALLOW_SW_HARDENING_NEEDED */
    
    /* Group Replication options */
    const char *gr_group_name;         /* --gr-group-name */
    const char *gr_seeds;              /* --gr-seeds */
    const char *gr_local_address;      /* --gr-local-address */
    int gr_bootstrap;                  /* --gr-bootstrap */
    
    /* Testing options */
    int dry_run;                       /* --dry-run: run all logic but skip execve() */
    const char *test_lan_ip;           /* --test-lan-ip: override LAN IP for testing */
    const char *test_public_ip;        /* --test-public-ip: override public IP for testing */
    const char *test_output_dir;       /* --test-output-dir: override output directory for testing */
    
    /* Additional MySQL args to pass through */
    int mysql_argc;
    char **mysql_argv;
};

/* Parse command line arguments - args take priority over environment variables */
static void parse_args(int argc, char *argv[], struct launcher_config *config) {
    /* Initialize with environment variables as defaults */
    config->contract_address = getenv("CONTRACT_ADDRESS");
    config->rpc_url = getenv("RPC_URL");
    config->whitelist_config = getenv("RATLS_WHITELIST_CONFIG");
    config->cert_path = getenv("RATLS_CERT_PATH");
    config->key_path = getenv("RATLS_KEY_PATH");
    config->data_dir = getenv("MYSQL_DATA_DIR");
    
    /* RA-TLS configuration from environment */
    config->ra_tls_cert_algorithm = getenv("RA_TLS_CERT_ALGORITHM");
    config->ratls_enable_verify = getenv("RATLS_ENABLE_VERIFY");
    config->ratls_require_peer_cert = getenv("RATLS_REQUIRE_PEER_CERT");
    config->ra_tls_allow_outdated_tcb = getenv("RA_TLS_ALLOW_OUTDATED_TCB_INSECURE");
    config->ra_tls_allow_hw_config_needed = getenv("RA_TLS_ALLOW_HW_CONFIG_NEEDED");
    config->ra_tls_allow_sw_hardening_needed = getenv("RA_TLS_ALLOW_SW_HARDENING_NEEDED");
    
    /* GR options default to NULL/0 */
    config->gr_group_name = NULL;
    config->gr_seeds = NULL;
    config->gr_local_address = NULL;
    config->gr_bootstrap = 0;
    
    /* Testing options default to NULL/0 */
    config->dry_run = 0;
    config->test_lan_ip = NULL;
    config->test_public_ip = NULL;
    config->test_output_dir = NULL;
    
    /* Allocate array for MySQL passthrough args */
    config->mysql_argv = malloc(argc * sizeof(char *));
    config->mysql_argc = 0;
    
    /* Parse arguments - args override environment variables */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--contract-address=", 19) == 0) {
            config->contract_address = argv[i] + 19;
        } else if (strncmp(argv[i], "--rpc-url=", 10) == 0) {
            config->rpc_url = argv[i] + 10;
        } else if (strncmp(argv[i], "--whitelist-config=", 19) == 0) {
            config->whitelist_config = argv[i] + 19;
        } else if (strncmp(argv[i], "--cert-path=", 12) == 0) {
            config->cert_path = argv[i] + 12;
        /* NOTE: --key-path and --data-dir are NOT allowed via command-line to prevent data leakage */
        /* They can only be set via environment variables in the manifest */
        } else if (strncmp(argv[i], "--ra-tls-cert-algorithm=", 24) == 0) {
            config->ra_tls_cert_algorithm = argv[i] + 24;
        } else if (strncmp(argv[i], "--ratls-enable-verify=", 22) == 0) {
            config->ratls_enable_verify = argv[i] + 22;
        } else if (strncmp(argv[i], "--ratls-require-peer-cert=", 26) == 0) {
            config->ratls_require_peer_cert = argv[i] + 26;
        } else if (strncmp(argv[i], "--ra-tls-allow-outdated-tcb=", 28) == 0) {
            config->ra_tls_allow_outdated_tcb = argv[i] + 28;
        } else if (strncmp(argv[i], "--ra-tls-allow-hw-config-needed=", 32) == 0) {
            config->ra_tls_allow_hw_config_needed = argv[i] + 32;
        } else if (strncmp(argv[i], "--ra-tls-allow-sw-hardening-needed=", 35) == 0) {
            config->ra_tls_allow_sw_hardening_needed = argv[i] + 35;
        } else if (strncmp(argv[i], "--gr-group-name=", 16) == 0) {
            config->gr_group_name = argv[i] + 16;
        } else if (strncmp(argv[i], "--gr-seeds=", 11) == 0) {
            config->gr_seeds = argv[i] + 11;
        } else if (strcmp(argv[i], "--gr-bootstrap") == 0) {
            config->gr_bootstrap = 1;
        } else if (strncmp(argv[i], "--gr-local-address=", 19) == 0) {
            config->gr_local_address = argv[i] + 19;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            config->dry_run = 1;
        } else if (strncmp(argv[i], "--test-lan-ip=", 14) == 0) {
            config->test_lan_ip = argv[i] + 14;
        } else if (strncmp(argv[i], "--test-public-ip=", 17) == 0) {
            config->test_public_ip = argv[i] + 17;
        } else if (strncmp(argv[i], "--test-output-dir=", 18) == 0) {
            config->test_output_dir = argv[i] + 18;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            /* Pass through to MySQL */
            config->mysql_argv[config->mysql_argc++] = argv[i];
        }
    }
    
    /* Apply defaults for paths if not set */
    if (!config->cert_path || strlen(config->cert_path) == 0) {
        config->cert_path = DEFAULT_CERT_PATH;
    }
    if (!config->key_path || strlen(config->key_path) == 0) {
        config->key_path = DEFAULT_KEY_PATH;
    }
    if (!config->data_dir || strlen(config->data_dir) == 0) {
        config->data_dir = DEFAULT_DATA_DIR;
    }
}

/* Print usage information */
static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS] [MYSQL_OPTIONS...]\n\n", prog_name);
    printf("MySQL RA-TLS Launcher with Group Replication Support\n");
    printf("Runs inside SGX enclave, sets up RA-TLS, and execve() to mysqld.\n\n");
    
    printf("GENERAL OPTIONS:\n");
    printf("  -h, --help                Show this help message and exit\n\n");
    
    printf("SMART CONTRACT OPTIONS (for whitelist from blockchain):\n");
    printf("  --contract-address=ADDR   Smart contract address for whitelist\n");
    printf("                            (env: CONTRACT_ADDRESS)\n");
    printf("  --rpc-url=URL             Ethereum JSON-RPC endpoint URL\n");
    printf("                            (env: RPC_URL)\n");
    printf("  --whitelist-config=CFG    Direct whitelist configuration (Base64-encoded CSV)\n");
    printf("                            (env: RATLS_WHITELIST_CONFIG)\n\n");
    
    printf("PATH OPTIONS:\n");
    printf("  --cert-path=PATH          Path for RA-TLS certificate\n");
    printf("                            (env: RATLS_CERT_PATH, default: %s)\n", DEFAULT_CERT_PATH);
    printf("\n");
    printf("  NOTE: The following paths can ONLY be set via manifest environment variables\n");
    printf("        (not command-line) to prevent data leakage:\n");
    printf("        - RATLS_KEY_PATH: RA-TLS private key path (default: %s)\n", DEFAULT_KEY_PATH);
    printf("        - MYSQL_DATA_DIR: MySQL data directory (default: %s)\n\n", DEFAULT_DATA_DIR);
    
    printf("RA-TLS CONFIGURATION OPTIONS:\n");
    printf("  --ra-tls-cert-algorithm=ALG\n");
    printf("                            Certificate algorithm (e.g., secp256r1, secp256k1)\n");
    printf("                            (env: RA_TLS_CERT_ALGORITHM)\n");
    printf("  --ratls-enable-verify=0|1\n");
    printf("                            Enable RA-TLS verification (default: 1)\n");
    printf("                            (env: RATLS_ENABLE_VERIFY)\n");
    printf("  --ratls-require-peer-cert=0|1\n");
    printf("                            Require peer certificate for mutual TLS (default: 1)\n");
    printf("                            (env: RATLS_REQUIRE_PEER_CERT)\n");
    printf("  --ra-tls-allow-outdated-tcb=0|1\n");
    printf("                            Allow outdated TCB (INSECURE, default: from manifest)\n");
    printf("                            (env: RA_TLS_ALLOW_OUTDATED_TCB_INSECURE)\n");
    printf("  --ra-tls-allow-hw-config-needed=0|1\n");
    printf("                            Allow HW configuration needed status (default: from manifest)\n");
    printf("                            (env: RA_TLS_ALLOW_HW_CONFIG_NEEDED)\n");
    printf("  --ra-tls-allow-sw-hardening-needed=0|1\n");
    printf("                            Allow SW hardening needed status (default: from manifest)\n");
    printf("                            (env: RA_TLS_ALLOW_SW_HARDENING_NEEDED)\n\n");
    
    printf("GROUP REPLICATION OPTIONS:\n");
    printf("  NOTE: Group Replication is ENABLED BY DEFAULT. A group name will be auto-generated\n");
    printf("        and persisted if not specified via CLI, env var, or persisted file.\n\n");
    printf("  --gr-group-name=UUID      Group Replication group name (UUID format)\n");
    printf("                            Priority: CLI > env var > persisted file > auto-generate\n");
    printf("                            (env: MYSQL_GR_GROUP_NAME)\n");
    printf("  --gr-seeds=SEEDS          Comma-separated list of additional seed nodes\n");
    printf("                            Format: host1:port1,host2:port2 or host1,host2\n");
    printf("                            (port defaults to %d if not specified)\n", GR_DEFAULT_PORT);
    printf("                            Note: Local LAN IP and public IP are automatically added\n");
    printf("  --gr-local-address=ADDR   Override local address for GR communication\n");
    printf("                            Format: host:port (default: auto-detect LAN IP:%d)\n", GR_DEFAULT_PORT);
    printf("  --gr-bootstrap            Bootstrap a new replication group (first node only)\n");
    printf("                            Without this flag, node will try to join existing group\n\n");
    
    printf("TESTING OPTIONS:\n");
    printf("  --dry-run                 Run all logic but skip execve() to mysqld\n");
    printf("                            Useful for testing configuration generation\n");
    printf("  --test-lan-ip=IP          Override LAN IP detection (for testing)\n");
    printf("  --test-public-ip=IP       Override public IP detection (for testing)\n");
    printf("  --test-output-dir=DIR     Override output directory for config files (for testing)\n\n");
    
    printf("MYSQL OPTIONS:\n");
    printf("  Any unrecognized options are passed through to mysqld.\n\n");
    
    printf("EXAMPLES:\n");
    printf("  # Start MySQL with GR enabled (auto-generates group name on first boot):\n");
    printf("  %s\n\n", prog_name);
    printf("  # Bootstrap a new Group Replication cluster (first node):\n");
    printf("  %s --gr-bootstrap\n\n", prog_name);
    printf("  # Join an existing cluster (use same group name from first node):\n");
    printf("  # Option 1: Set env var in manifest\n");
    printf("  MYSQL_GR_GROUP_NAME=<uuid-from-first-node> %s --gr-seeds=192.168.1.100:33061\n\n", prog_name);
    printf("  # Option 2: Copy persisted file from first node to /app/wallet/.mysql_gr_group_name\n");
    printf("  %s --gr-seeds=192.168.1.100:33061\n\n", prog_name);
    printf("  # Explicit group name (overrides auto-generation):\n");
    printf("  %s --gr-group-name=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee --gr-bootstrap\n\n", prog_name);
    
    printf("ENVIRONMENT VARIABLES:\n");
    printf("  All options can also be set via environment variables as noted above.\n");
    printf("  Command-line arguments take priority over environment variables.\n");
}

/* Validate and normalize configuration - handle mutual exclusions and dependencies */
static int validate_config(struct launcher_config *config) {
    int has_errors = 0;
    
    printf("\n[Launcher] Validating configuration...\n");
    
    /* === Whitelist / Contract / RPC precedence === */
    /* Rule: If --rpc-url is specified, ignore --whitelist-config (will use contract whitelist) */
    if (config->rpc_url && strlen(config->rpc_url) > 0) {
        if (config->whitelist_config && strlen(config->whitelist_config) > 0) {
            printf("[Launcher] Warning: --rpc-url specified, ignoring --whitelist-config (will use contract whitelist if available)\n");
            config->whitelist_config = NULL;  /* Clear whitelist config */
        }
        
        /* If rpc-url is set but contract-address is missing, warn */
        if (!config->contract_address || strlen(config->contract_address) == 0) {
            printf("[Launcher] Warning: --rpc-url specified but --contract-address is missing\n");
            printf("[Launcher]          Cannot read whitelist from contract without contract address\n");
        }
    }
    
    /* If contract-address is set but rpc-url is missing, warn and fall back to whitelist-config */
    if (config->contract_address && strlen(config->contract_address) > 0) {
        if (!config->rpc_url || strlen(config->rpc_url) == 0) {
            printf("[Launcher] Warning: --contract-address specified but --rpc-url is missing\n");
            printf("[Launcher]          Falling back to --whitelist-config or environment whitelist\n");
        }
    }
    
    /* === Group Replication dependencies === */
    /* Group name can come from: CLI, env var, persisted file, or auto-generated (if bootstrap) */
    int has_cli_group_name = (config->gr_group_name && strlen(config->gr_group_name) > 0);
    const char *env_group_name = getenv("MYSQL_GR_GROUP_NAME");
    int has_env_group_name = (env_group_name && strlen(env_group_name) > 0);
    
    /* GR is potentially enabled if we have a group name OR if bootstrap mode is set (auto-generate) */
    int gr_potentially_enabled = has_cli_group_name || has_env_group_name || config->gr_bootstrap;
    
    /* GR-specific options only valid when GR is potentially enabled */
    if (!gr_potentially_enabled) {
        /* Only warn about seeds/local-address if they're specified without any GR enablement */
        if (config->gr_seeds && strlen(config->gr_seeds) > 0) {
            printf("[Launcher] Warning: --gr-seeds specified but no GR mode enabled (ignored)\n");
            printf("[Launcher]          Use --gr-group-name, MYSQL_GR_GROUP_NAME env var, or --gr-bootstrap\n");
        }
        if (config->gr_local_address && strlen(config->gr_local_address) > 0) {
            printf("[Launcher] Warning: --gr-local-address specified but no GR mode enabled (ignored)\n");
            printf("[Launcher]          Use --gr-group-name, MYSQL_GR_GROUP_NAME env var, or --gr-bootstrap\n");
        }
    }
    
    /* === Certificate and key path validation === */
    if (config->cert_path && strlen(config->cert_path) > 0) {
        if (!config->key_path || strlen(config->key_path) == 0) {
            printf("[Launcher] Warning: --cert-path specified but --key-path is missing, using default key path\n");
        }
    }
    if (config->key_path && strlen(config->key_path) > 0) {
        if (!config->cert_path || strlen(config->cert_path) == 0) {
            printf("[Launcher] Warning: --key-path specified but --cert-path is missing, using default cert path\n");
        }
        /* Warn if key path is not in encrypted partition */
        if (strstr(config->key_path, "/app/wallet") == NULL) {
            printf("[Launcher] Warning: --key-path '%s' is not in encrypted partition (/app/wallet/)\n", config->key_path);
            printf("[Launcher]          Private key may not be protected by SGX encryption\n");
        }
    }
    
    /* === Data directory validation === */
    if (config->data_dir && strlen(config->data_dir) > 0) {
        if (strstr(config->data_dir, "/app/wallet") == NULL) {
            printf("[Launcher] Warning: --data-dir '%s' is not in encrypted partition (/app/wallet/)\n", config->data_dir);
            printf("[Launcher]          MySQL data may not be protected by SGX encryption\n");
        }
    }
    
    printf("[Launcher] Configuration validation %s\n", has_errors ? "FAILED" : "passed");
    
    return has_errors ? -1 : 0;
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
    printf("With Group Replication Support\n");
    printf("==========================================\n\n");
    
    /* Initialize curl globally */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    /* Parse command line arguments - args take priority over environment variables */
    struct launcher_config config;
    parse_args(argc, argv, &config);
    
    /* Validate configuration - handle mutual exclusions and dependencies */
    if (validate_config(&config) != 0) {
        fprintf(stderr, "[Launcher] Configuration validation failed, exiting\n");
        curl_global_cleanup();
        return 1;
    }
    
    /* Set RA-TLS configuration from parsed config (args override env vars) */
    printf("[Launcher] Setting up RA-TLS configuration...\n");
    
    /* Apply RA-TLS settings from config (command-line args take priority) */
    if (config.ra_tls_cert_algorithm && strlen(config.ra_tls_cert_algorithm) > 0) {
        set_env("RA_TLS_CERT_ALGORITHM", config.ra_tls_cert_algorithm, 1);
    }
    if (config.ratls_enable_verify && strlen(config.ratls_enable_verify) > 0) {
        set_env("RATLS_ENABLE_VERIFY", config.ratls_enable_verify, 1);
    } else {
        set_env("RATLS_ENABLE_VERIFY", "1", 1);  /* Default: enable verification */
    }
    if (config.ratls_require_peer_cert && strlen(config.ratls_require_peer_cert) > 0) {
        set_env("RATLS_REQUIRE_PEER_CERT", config.ratls_require_peer_cert, 1);
    } else {
        set_env("RATLS_REQUIRE_PEER_CERT", "1", 1);  /* Default: require peer cert */
    }
    if (config.ra_tls_allow_outdated_tcb && strlen(config.ra_tls_allow_outdated_tcb) > 0) {
        set_env("RA_TLS_ALLOW_OUTDATED_TCB_INSECURE", config.ra_tls_allow_outdated_tcb, 1);
    }
    if (config.ra_tls_allow_hw_config_needed && strlen(config.ra_tls_allow_hw_config_needed) > 0) {
        set_env("RA_TLS_ALLOW_HW_CONFIG_NEEDED", config.ra_tls_allow_hw_config_needed, 1);
    }
    if (config.ra_tls_allow_sw_hardening_needed && strlen(config.ra_tls_allow_sw_hardening_needed) > 0) {
        set_env("RA_TLS_ALLOW_SW_HARDENING_NEEDED", config.ra_tls_allow_sw_hardening_needed, 1);
    }
    
    /* Set certificate and key paths */
    set_env("RATLS_CERT_PATH", config.cert_path, 1);
    set_env("RATLS_KEY_PATH", config.key_path, 1);
    
    /* Use paths from config */
    const char *cert_path = config.cert_path;
    const char *key_path = config.key_path;
    
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
    const char *data_dir_to_create = config.data_dir;
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
    
    /* Create GR config directory for mysql-gr.cnf and gr_group_name.txt */
    /* This directory is NOT encrypted - GR config doesn't contain secrets (auth is via RA-TLS) */
    printf("[Launcher] Creating GR config directory: /var/lib/mysql\n");
    if (mkdir_p("/var/lib/mysql") != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create GR config directory: %s\n", strerror(errno));
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
    
    if (config.contract_address && strlen(config.contract_address) > 0) {
        printf("[Launcher] Contract address specified: %s\n", config.contract_address);
        
        if (!config.rpc_url || strlen(config.rpc_url) == 0) {
            fprintf(stderr, "[Launcher] Warning: RPC_URL not set, cannot read from contract\n");
            printf("[Launcher] Falling back to environment-based whitelist (if set)\n");
        } else {
            /* Try to read whitelist from contract */
            char *whitelist = read_whitelist_from_contract(config.contract_address, config.rpc_url);
            
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
    
    const char *data_dir = config.data_dir;
    
    /* Check if this is first boot (need to initialize users) */
    int first_boot = !is_mysql_initialized(data_dir);
    char init_sql_path[MAX_PATH_LEN] = {0};
    char init_file_arg[MAX_PATH_LEN] = {0};
    
    /* Group Replication variables */
    /* Get group name using priority: CLI > env var > persisted file > generate (if bootstrap) */
    const char *gr_group_name = get_or_create_gr_group_name(config.gr_group_name, config.gr_bootstrap);
    int gr_enabled = (gr_group_name != NULL && strlen(gr_group_name) > 0);
    char gr_config_path[MAX_PATH_LEN] = {0};
    char defaults_extra_file_arg[MAX_PATH_LEN] = {0};
    char lan_ip[MAX_IP_LEN] = {0};
    char public_ip[MAX_IP_LEN] = {0};
    char seeds_list[MAX_SEEDS_LEN] = {0};
    uint32_t server_id = 0;
    
    /* Handle Group Replication setup if enabled */
    if (gr_enabled) {
        printf("\n[Launcher] Group Replication Configuration:\n");
        printf("[Launcher] Group name: %s\n", gr_group_name);
        printf("[Launcher] Bootstrap mode: %s\n", config.gr_bootstrap ? "YES" : "NO");
        
        /* Get LAN IP - use test override if provided */
        if (config.test_lan_ip && strlen(config.test_lan_ip) > 0) {
            strncpy(lan_ip, config.test_lan_ip, sizeof(lan_ip) - 1);
            printf("[Launcher] Using test LAN IP: %s\n", lan_ip);
        } else if (get_lan_ip(lan_ip, sizeof(lan_ip)) != 0) {
            fprintf(stderr, "[Launcher] Warning: Could not detect LAN IP\n");
        } else {
            printf("[Launcher] Detected LAN IP: %s\n", lan_ip);
        }
        
        /* Get public IP - use test override if provided */
        if (config.test_public_ip && strlen(config.test_public_ip) > 0) {
            strncpy(public_ip, config.test_public_ip, sizeof(public_ip) - 1);
            printf("[Launcher] Using test public IP: %s\n", public_ip);
        } else if (get_public_ip(public_ip, sizeof(public_ip)) != 0) {
            fprintf(stderr, "[Launcher] Warning: Could not detect public IP\n");
        } else {
            printf("[Launcher] Detected public IP: %s\n", public_ip);
        }
        
        /* Get or create stable server ID */
        server_id = get_or_create_server_id(lan_ip, public_ip);
        printf("[Launcher] Server ID: %u\n", server_id);
        
        /* Build seeds list (self IPs + extra seeds, deduplicated) */
        build_seeds_list(seeds_list, sizeof(seeds_list), lan_ip, public_ip, config.gr_seeds, GR_DEFAULT_PORT);
        printf("[Launcher] Seeds list: %s\n", seeds_list);
        
        /* Determine local address for GR */
        char gr_local_address[MAX_IP_LEN + 16] = {0};
        if (config.gr_local_address && strlen(config.gr_local_address) > 0) {
            strncpy(gr_local_address, config.gr_local_address, sizeof(gr_local_address) - 1);
        } else if (strlen(lan_ip) > 0) {
            snprintf(gr_local_address, sizeof(gr_local_address), "%s:%d", lan_ip, GR_DEFAULT_PORT);
        } else {
            fprintf(stderr, "[Launcher] ERROR: Cannot determine local address for Group Replication\n");
            return 1;
        }
        printf("[Launcher] GR local address: %s\n", gr_local_address);
        
        /* Create GR config file */
        strncpy(gr_config_path, GR_CONFIG_FILE, sizeof(gr_config_path) - 1);
        if (create_gr_config(GR_CONFIG_FILE, server_id,
                            gr_group_name, gr_local_address, seeds_list,
                            config.gr_bootstrap, cert_path, key_path) != 0) {
            fprintf(stderr, "[Launcher] ERROR: Failed to create GR config file\n");
            return 1;
        }
        
        /* Prepare --defaults-extra-file argument */
        snprintf(defaults_extra_file_arg, sizeof(defaults_extra_file_arg),
                 "--defaults-extra-file=%s", GR_CONFIG_FILE);
    }
    
    if (first_boot) {
        printf("[Launcher] First boot detected - will initialize MySQL data directory\n");
        /* Create sentinel file to mark data directory as initialized */
        create_sentinel_file(data_dir);
    } else {
        printf("[Launcher] MySQL data directory already initialized\n");
    }
    
    /* Always create and execute init SQL on every startup */
    /* The SQL is idempotent (safe to run multiple times) */
    /* This ensures users are created and GR plugin is installed even on subsequent boots */
    int init_sql_created = 0;
    printf("[Launcher] Creating init SQL (executed on every startup, idempotent)\n");
    
    if (gr_enabled) {
        /* GR mode: create init SQL with GR setup */
        if (create_gr_init_sql(data_dir, init_sql_path, sizeof(init_sql_path),
                               config.gr_bootstrap, cert_path, key_path) == 0) {
            snprintf(init_file_arg, sizeof(init_file_arg), "--init-file=%s", init_sql_path);
            printf("[Launcher] Will execute GR init SQL on startup: %s\n", init_sql_path);
            init_sql_created = 1;
        } else {
            fprintf(stderr, "[Launcher] Warning: Could not create GR init SQL file\n");
        }
    } else {
        /* Non-GR mode: create standard init SQL */
        if (create_init_sql(data_dir, init_sql_path, sizeof(init_sql_path)) == 0) {
            snprintf(init_file_arg, sizeof(init_file_arg), "--init-file=%s", init_sql_path);
            printf("[Launcher] Will execute init SQL on startup: %s\n", init_sql_path);
            init_sql_created = 1;
        } else {
            fprintf(stderr, "[Launcher] Warning: Could not create init SQL file\n");
        }
    }
    
    /* Build argument list for mysqld */
    /* We need: mysqld [--defaults-extra-file=...] --datadir=... --ssl-cert=... --ssl-key=... --require-secure-transport=ON --log-error=... --console [--init-file=...] */
    /* Note: No --user=mysql since we run as root in container (user said it's not needed) */
    /* Note: --log-error is needed to override any config file setting that points to encrypted partition */
    /* Note: --console outputs logs to stderr for easier debugging in container environments */
    /* Note: --defaults-extra-file MUST be the first argument after mysqld */
    
    /* Calculate number of arguments needed */
    int base_args = 7;  /* mysqld, datadir, ssl-cert, ssl-key, require-secure-transport, log-error, console */
    int extra_args = 0;
    if (gr_enabled) extra_args++;  /* --defaults-extra-file */
    if (init_sql_created && init_file_arg[0] != '\0') extra_args++;  /* --init-file (always on every startup) */
    
    int new_argc = base_args + extra_args;
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
    
    /* --defaults-extra-file MUST be the first argument after mysqld */
    if (gr_enabled && defaults_extra_file_arg[0] != '\0') {
        new_argv[idx++] = defaults_extra_file_arg;
    }
    
    new_argv[idx++] = datadir_arg;
    new_argv[idx++] = ssl_cert_arg;
    new_argv[idx++] = ssl_key_arg;
    new_argv[idx++] = "--require-secure-transport=ON";
    new_argv[idx++] = "--log-error=/var/log/mysql/error.log";  /* Override config file setting */
    new_argv[idx++] = "--console";  /* Output logs to stderr for easier debugging */
    
    /* Add --init-file on every startup (SQL is idempotent) */
    if (init_sql_created && init_file_arg[0] != '\0') {
        new_argv[idx++] = init_file_arg;
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
    if (gr_enabled) {
        printf("[Launcher]   GR config: %s\n", gr_config_path);
        printf("[Launcher]   GR mode: %s\n", config.gr_bootstrap ? "BOOTSTRAP" : "JOIN");
    }
    if (init_sql_created && init_file_arg[0] != '\0') {
        printf("[Launcher]   Init file: %s (executed every startup)\n", init_sql_path);
    }
    printf("\n");
    
    /* Print full command line that would be executed */
    printf("[Launcher] Full command line:\n");
    printf("  ");
    for (int i = 0; new_argv[i] != NULL; i++) {
        printf("%s ", new_argv[i]);
    }
    printf("\n\n");
    
    /* In dry-run mode, skip execve() and exit successfully */
    if (config.dry_run) {
        printf("==========================================\n");
        printf("DRY RUN MODE - Skipping execve()\n");
        printf("==========================================\n");
        printf("[Launcher] All configuration generated successfully.\n");
        printf("[Launcher] In normal mode, mysqld would be started with the above command.\n");
        free(new_argv);
        return 0;
    }
    
    /* Use execv to replace this process with mysqld */
    /* execv preserves the environment variables we set (including LD_PRELOAD) */
    execv(MYSQLD_PATH, new_argv);
    
    /* If we get here, execve failed */
    fprintf(stderr, "[Launcher] Failed to execute %s: %s\n", MYSQLD_PATH, strerror(errno));
    free(new_argv);
    
    return 1;
}
