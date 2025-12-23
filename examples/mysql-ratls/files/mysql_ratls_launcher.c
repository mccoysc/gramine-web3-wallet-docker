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
#define MAX_SEEDS_LEN 4096
#define MAX_IP_LEN 64
#define UUID_LEN 36  /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */

/* RA-TLS library candidate paths (searched in order) */
static const char *RA_TLS_LIB_PATHS[] = {
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
    
    /* Remove files that must be unique per instance.
     * These files are generated during mysqld --initialize and would cause
     * all containers from the same image to share the same identity.
     * MySQL will automatically regenerate these files on first startup.
     *
     * Files to remove:
     * - auto.cnf: Contains server_uuid (CRITICAL for Group Replication)
     * - SSL/TLS certificates and keys: Should be unique per instance
     *   (We use RA-TLS generated certs, but removing these prevents
     *    accidental use of shared credentials)
     * - RSA key pair: Used for sha256_password authentication
     */
    static const char *unique_files[] = {
        "auto.cnf",           /* server_uuid - CRITICAL for GR */
        "ca-key.pem",         /* CA private key */
        "ca.pem",             /* CA certificate */
        "server-cert.pem",    /* Server certificate */
        "server-key.pem",     /* Server private key */
        "client-cert.pem",    /* Client certificate */
        "client-key.pem",     /* Client private key */
        "private_key.pem",    /* RSA private key */
        "public_key.pem",     /* RSA public key */
        NULL
    };
    
    printf("[Launcher] Removing instance-unique files from copied template...\n");
    char file_path[MAX_PATH_LEN];
    for (int i = 0; unique_files[i] != NULL; i++) {
        snprintf(file_path, sizeof(file_path), "%s/%s", data_dir, unique_files[i]);
        if (file_exists(file_path)) {
            if (unlink(file_path) == 0) {
                printf("[Launcher]   Removed: %s (will be regenerated by MySQL)\n", unique_files[i]);
            } else {
                fprintf(stderr, "[Launcher]   Warning: Failed to remove %s: %s\n", 
                        unique_files[i], strerror(errno));
            }
        }
    }
    printf("[Launcher] Instance-unique files cleanup completed\n");
    
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
    char sql_content[8192];
    int offset = 0;
    
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- MySQL RA-TLS User Initialization\n"
        "-- This file is executed on first boot inside the SGX enclave\n"
        "-- Users are configured with REQUIRE X509 (certificate-only authentication)\n"
        "-- RA-TLS handles the actual SGX attestation verification\n"
        "-- Only 'app' user is allowed; root accounts are removed for security\n\n");
    
    /* Create application user with X509 requirement and highest privileges */
    /* ALL PRIVILEGES grants static privileges, but MySQL 8 dynamic privileges must be granted separately */
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- Create application user that requires X.509 certificate with highest privileges\n"
        "CREATE USER IF NOT EXISTS 'app'@'%%' IDENTIFIED BY '' REQUIRE X509;\n"
        "GRANT ALL PRIVILEGES ON *.* TO 'app'@'%%' WITH GRANT OPTION;\n"
        "-- Grant all MySQL 8 dynamic privileges for full administrative access\n"
        "GRANT APPLICATION_PASSWORD_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT AUDIT_ABORT_EXEMPT ON *.* TO 'app'@'%%';\n"
        "GRANT AUDIT_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT AUTHENTICATION_POLICY_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT BACKUP_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT BINLOG_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT BINLOG_ENCRYPTION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT CLONE_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT CONNECTION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT ENCRYPTION_KEY_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT FIREWALL_EXEMPT ON *.* TO 'app'@'%%';\n"
        "GRANT FLUSH_OPTIMIZER_COSTS ON *.* TO 'app'@'%%';\n"
        "GRANT FLUSH_STATUS ON *.* TO 'app'@'%%';\n"
        "GRANT FLUSH_TABLES ON *.* TO 'app'@'%%';\n"
        "GRANT FLUSH_USER_RESOURCES ON *.* TO 'app'@'%%';\n"
        "GRANT GROUP_REPLICATION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT GROUP_REPLICATION_STREAM ON *.* TO 'app'@'%%';\n"
        "GRANT INNODB_REDO_LOG_ARCHIVE ON *.* TO 'app'@'%%';\n"
        "GRANT INNODB_REDO_LOG_ENABLE ON *.* TO 'app'@'%%';\n"
        "GRANT PASSWORDLESS_USER_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT PERSIST_RO_VARIABLES_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT REPLICATION_APPLIER ON *.* TO 'app'@'%%';\n"
        "GRANT REPLICATION_SLAVE_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT RESOURCE_GROUP_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT RESOURCE_GROUP_USER ON *.* TO 'app'@'%%';\n"
        "GRANT ROLE_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT SENSITIVE_VARIABLES_OBSERVER ON *.* TO 'app'@'%%';\n"
        "GRANT SERVICE_CONNECTION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT SESSION_VARIABLES_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT SET_USER_ID ON *.* TO 'app'@'%%';\n"
        "GRANT SHOW_ROUTINE ON *.* TO 'app'@'%%';\n"
        "GRANT SYSTEM_USER ON *.* TO 'app'@'%%';\n"
        "GRANT SYSTEM_VARIABLES_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT TABLE_ENCRYPTION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT XA_RECOVER_ADMIN ON *.* TO 'app'@'%%';\n\n");
    
    /* Remove root accounts created by --initialize-insecure during Docker build */
    /* This ensures only the 'app' user with X509 certificate authentication exists */
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- Remove root accounts (created by --initialize-insecure)\n"
        "-- Only 'app' user with X509 certificate authentication is allowed\n"
        "DROP USER IF EXISTS 'root'@'localhost';\n"
        "DROP USER IF EXISTS 'root'@'%%';\n"
        "DROP USER IF EXISTS 'root'@'127.0.0.1';\n"
        "DROP USER IF EXISTS 'root'@'::1';\n\n");
    
    /* Note: Password authentication plugins (mysql_native_password, caching_sha2_password, sha256_password)
     * are built-in to mysqld and cannot be uninstalled. Security is enforced at account level:
     * - Only 'app' user exists with REQUIRE X509 (certificate authentication required)
     * - All root accounts are removed
     * - require_secure_transport=ON in mysqld.cnf ensures TLS is mandatory */
    
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

/* Check if a TCP port is available for binding
 * Returns: 1 if port is available, 0 if occupied, -1 on error
 */
static int is_port_available(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[Launcher] Warning: Could not create socket for port check: %s\n", strerror(errno));
        return -1;
    }
    
    /* Set SO_REUSEADDR to avoid false positives from TIME_WAIT sockets */
    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  /* Bind to 0.0.0.0 for conservative check */
    addr.sin_port = htons(port);
    
    int result = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
    
    if (result == 0) {
        return 1;  /* Port is available */
    } else if (errno == EADDRINUSE) {
        return 0;  /* Port is occupied */
    } else {
        fprintf(stderr, "[Launcher] Warning: Port check failed for port %d: %s\n", port, strerror(errno));
        return -1;  /* Error */
    }
}

/* Find an available port starting from the given port
 * Returns: available port number, or -1 if none found up to 65535
 */
static int find_available_port(int start_port) {
    for (int port = start_port; port <= 65535; port++) {
        int available = is_port_available(port);
        if (available == 1) {
            return port;
        } else if (available == -1) {
            /* Error checking port, try next one */
            continue;
        }
        /* Port occupied, try next */
    }
    return -1;  /* No available port found */
}

/* Get or create server_id for MySQL Group Replication based on IP+port hash */
static unsigned int get_or_create_server_id(const char *lan_ip, int gr_port) {
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
    
    /* Generate server_id based on hash of LAN IP and GR port.
     * Including the GR port ensures unique server_ids when multiple nodes
     * run on the same host with network=host (same IP, different ports).
     */
    unsigned int hash = 5381;
    const char *str = lan_ip;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    /* Include GR port in the hash to differentiate nodes on the same host */
    hash = ((hash << 5) + hash) + (gr_port & 0xFF);
    hash = ((hash << 5) + hash) + ((gr_port >> 8) & 0xFF);
    
    /* Ensure server_id is in valid range (1 to 2^32-1) and not 0 */
    server_id = (hash % 4294967294) + 1;
    
    /* Save server_id to file for persistence across restarts */
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

/* Build seeds list from user-specified seeds only.
 * 
 * IMPORTANT: We no longer auto-add the local node's IP (specified_ip/lan_ip) to seeds.
 * This fixes a potential issue where self-connection attempts during Group Replication
 * join could cause SSL handshake failures and connection churn.
 * 
 * For non-bootstrap nodes, seeds should only contain other nodes' addresses.
 * For bootstrap nodes, seeds list can be empty (they don't need to connect to anyone).
 */
static int build_seeds_list(char *seeds_buf, size_t buf_size,
                            const char *extra_seeds, int gr_port) {
    seeds_buf[0] = '\0';
    size_t len = 0;
    
    /* Add user-specified seeds (extra_seeds) */
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
                            const char *cert_path, const char *key_path,
                            int gr_debug) {
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
    
    /* Event scheduler (required for delayed GR startup via EVENTs) */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "event_scheduler=ON\n");
    
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
    
    /* Group communication SSL settings (XCom protocol between nodes) */
    /* Note: This is different from recovery channel SSL - this encrypts
     * the group communication (consensus, membership, etc.) between nodes.
     * MySQL 8.0.44 does not have group_replication_ssl_cert/key variables;
     * it automatically uses the server's --ssl-cert and --ssl-key settings. */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "\n# Group Communication SSL Settings (XCom protocol between nodes)\n"
        "# ssl_mode=REQUIRED ensures all group communication is encrypted\n"
        "# MySQL uses server's --ssl-cert/--ssl-key automatically (no separate GR SSL vars)\n"
        "# RA-TLS library handles SGX quote verification for self-signed certs\n"
        "loose-group_replication_ssl_mode=REQUIRED\n");
    
    /* Recovery channel SSL settings (use same certs as main connection) */
    /* Note: ssl_verify_server_cert=OFF disables traditional PKI certificate chain validation.
     * This is REQUIRED when using self-signed RA-TLS certificates without a CA.
     * Each node has its own self-signed RA-TLS cert with SGX quote embedded.
     * The libratls-quote-verify.so library (via LD_PRELOAD) intercepts TLS handshakes
     * and verifies SGX quotes in certificates, providing attestation-based trust
     * instead of traditional PKI certificate chain validation.
     * 
     * With ssl_verify_server_cert=ON and no CA, MySQL's PKI verification would fail
     * BEFORE RA-TLS can perform SGX quote verification, causing "Error connecting using SSL"
     * errors during Group Replication's local connectivity test. */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "\n# Recovery Channel SSL Settings (Mutual TLS with RA-TLS attestation)\n"
        "# ssl_verify_server_cert=OFF disables PKI chain validation (no CA for self-signed certs)\n"
        "# RA-TLS library handles SGX quote verification for attestation-based trust\n"
        "loose-group_replication_recovery_use_ssl=ON\n"
        "loose-group_replication_recovery_ssl_cert=%s\n"
        "loose-group_replication_recovery_ssl_key=%s\n"
        "loose-group_replication_recovery_ssl_verify_server_cert=OFF\n",
        cert_path, key_path);
    
    /* IP allowlist - explicit list to avoid "Unable to get local IP addresses" error in Gramine
     * When set to AUTOMATIC, MySQL tries to enumerate local network interfaces which fails
     * inside SGX enclaves. We use a permissive CIDR that allows all IPs. */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "\n# IP Allowlist (explicit to avoid interface enumeration in SGX enclave)\n"
        "loose-group_replication_ip_allowlist=0.0.0.0/0,::/0\n");
    
    /* Member expel timeout - increased from default 5s to 30s for SGX/RA-TLS environments.
     * In SGX enclaves with RA-TLS, SSL handshakes take longer due to SGX quote generation
     * and verification. The default 5-second timeout causes nodes to be expelled before
     * they can complete the handshake and establish bidirectional communication.
     * This is separate from XCom's DETECTOR_LIVE_TIMEOUT (also increased to 30s in patches). */
    offset += snprintf(config_content + offset, sizeof(config_content) - offset,
        "\n# Member Expel Timeout (increased for SGX/RA-TLS environments)\n"
        "# Default is 5s which is too short for RA-TLS handshakes with SGX quote verification\n"
        "loose-group_replication_member_expel_timeout=30\n");
    
    /* Verbose logging for GR debugging and troubleshooting (only when --gr-debug is enabled) */
    if (gr_debug) {
        offset += snprintf(config_content + offset, sizeof(config_content) - offset,
            "\n# GR Verbose Logging (enabled via --gr-debug)\n"
            "# Set maximum verbosity to ensure NOTE-level GR debug messages are visible\n"
            "log_error_verbosity=3\n"
            "# Enable all XCom communication debug options\n"
            "loose-group_replication_communication_debug_options=GCS_DEBUG_ALL\n"
            "# Autorejoin tries - number of times to try rejoining after being expelled\n"
            "loose-group_replication_autorejoin_tries=3\n"
            "# Exit state action - what to do when member is expelled (READ_ONLY keeps data accessible)\n"
            "loose-group_replication_exit_state_action=READ_ONLY\n"
            "# Unreachable majority timeout - how long to wait for majority before taking action\n"
            "loose-group_replication_unreachable_majority_timeout=0\n");
        printf("[Launcher] GR debug logging enabled (--gr-debug)\n");
        printf("[Launcher] GR debug logs will be written to /var/log/mysql/error.log and console (stderr)\n");
    }
    
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
                              int is_bootstrap) {
    /* SQL file is stored in data_dir which is in encrypted partition (/app/wallet/mysql-data) */
    snprintf(init_sql_path, path_size, "%s/%s", data_dir, INIT_SQL_FILE);
    
    FILE *f = fopen(init_sql_path, "w");
    if (!f) {
        fprintf(stderr, "[Launcher] Failed to create init SQL file: %s\n", strerror(errno));
        return -1;
    }
    
    /* Build SQL content in memory so we can both write and print it */
    /* Buffer size increased for all dynamic privileges */
    char sql_content[16384];
    int offset = 0;
    
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- MySQL RA-TLS User Initialization with Group Replication\n"
        "-- This file is executed on EVERY startup inside the SGX enclave\n"
        "-- All statements are idempotent (safe to run multiple times)\n"
        "-- Users are configured with REQUIRE X509 (certificate-only authentication)\n"
        "-- RA-TLS handles the actual SGX attestation verification\n"
        "-- Only 'app' user is allowed; root accounts are removed for security\n\n");
    
    /* Create application user with X509 requirement and highest privileges */
    /* ALL PRIVILEGES grants static privileges, but MySQL 8 dynamic privileges must be granted separately */
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- Create application user that requires X.509 certificate with highest privileges (idempotent)\n"
        "CREATE USER IF NOT EXISTS 'app'@'%%' IDENTIFIED BY '' REQUIRE X509;\n"
        "GRANT ALL PRIVILEGES ON *.* TO 'app'@'%%' WITH GRANT OPTION;\n"
        "-- Grant all MySQL 8 dynamic privileges for full administrative access\n"
        "GRANT APPLICATION_PASSWORD_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT AUDIT_ABORT_EXEMPT ON *.* TO 'app'@'%%';\n"
        "GRANT AUDIT_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT AUTHENTICATION_POLICY_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT BACKUP_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT BINLOG_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT BINLOG_ENCRYPTION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT CLONE_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT CONNECTION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT ENCRYPTION_KEY_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT FIREWALL_EXEMPT ON *.* TO 'app'@'%%';\n"
        "GRANT FLUSH_OPTIMIZER_COSTS ON *.* TO 'app'@'%%';\n"
        "GRANT FLUSH_STATUS ON *.* TO 'app'@'%%';\n"
        "GRANT FLUSH_TABLES ON *.* TO 'app'@'%%';\n"
        "GRANT FLUSH_USER_RESOURCES ON *.* TO 'app'@'%%';\n"
        "GRANT GROUP_REPLICATION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT GROUP_REPLICATION_STREAM ON *.* TO 'app'@'%%';\n"
        "GRANT INNODB_REDO_LOG_ARCHIVE ON *.* TO 'app'@'%%';\n"
        "GRANT INNODB_REDO_LOG_ENABLE ON *.* TO 'app'@'%%';\n"
        "GRANT PASSWORDLESS_USER_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT PERSIST_RO_VARIABLES_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT REPLICATION_APPLIER ON *.* TO 'app'@'%%';\n"
        "GRANT REPLICATION_SLAVE_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT RESOURCE_GROUP_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT RESOURCE_GROUP_USER ON *.* TO 'app'@'%%';\n"
        "GRANT ROLE_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT SENSITIVE_VARIABLES_OBSERVER ON *.* TO 'app'@'%%';\n"
        "GRANT SERVICE_CONNECTION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT SESSION_VARIABLES_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT SET_USER_ID ON *.* TO 'app'@'%%';\n"
        "GRANT SHOW_ROUTINE ON *.* TO 'app'@'%%';\n"
        "GRANT SYSTEM_USER ON *.* TO 'app'@'%%';\n"
        "GRANT SYSTEM_VARIABLES_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT TABLE_ENCRYPTION_ADMIN ON *.* TO 'app'@'%%';\n"
        "GRANT XA_RECOVER_ADMIN ON *.* TO 'app'@'%%';\n\n");
    
    /* Remove root accounts created by --initialize-insecure during Docker build */
    /* This ensures only the 'app' user with X509 certificate authentication exists */
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "-- Remove root accounts (created by --initialize-insecure)\n"
        "-- Only 'app' user with X509 certificate authentication is allowed\n"
        "DROP USER IF EXISTS 'root'@'localhost';\n"
        "DROP USER IF EXISTS 'root'@'%%';\n"
        "DROP USER IF EXISTS 'root'@'127.0.0.1';\n"
        "DROP USER IF EXISTS 'root'@'::1';\n\n");
    
    /* Note: Password authentication plugins (mysql_native_password, caching_sha2_password, sha256_password)
     * are built-in to mysqld and cannot be uninstalled. Security is enforced at account level:
     * - Only 'app' user exists with REQUIRE X509 (certificate authentication required)
     * - All root accounts are removed
     * - require_secure_transport=ON in mysqld.cnf ensures TLS is mandatory */
    
    offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
        "FLUSH PRIVILEGES;\n\n");
    
    /* Note: Group Replication plugin is loaded via plugin_load_add in mysql-gr.cnf */
    /* No need for INSTALL PLUGIN here - the plugin is already loaded at startup */
    
    /* Note: Recovery channel SSL settings are configured via group_replication_recovery_ssl_*
     * options in mysql-gr.cnf. We do NOT use CHANGE REPLICATION SOURCE ... FOR CHANNEL
     * 'group_replication_recovery' because MySQL 8 does not allow direct modification
     * of this GR-managed channel (error 3139).
     * 
     * Instead, we specify recovery user via START GROUP_REPLICATION USER='app'.
     * The 'app' user has REQUIRE X509 (certificate authentication), so no password is needed.
     * Mutual TLS is enforced by:
     * - Server side: 'app' user with REQUIRE X509
     * - Client side: group_replication_recovery_ssl_verify_server_cert=ON in cnf
     * - RA-TLS: libratls-quote-verify.so verifies SGX quotes in certificates */
    
    /* Start Group Replication using delayed EVENTs
     * 
     * Problem: Executing START GROUP_REPLICATION directly in init-file causes error
     * MY-011706 "maximum number of retries exceeded when waiting for the internal
     * server session state to be operating" because GR's internal session mechanism
     * is not ready during init-file execution.
     * 
     * Solution: Use MySQL EVENT scheduler to delay GR startup until the server is
     * fully operational. Each EVENT has a single-statement body because init-file
     * does not support DELIMITER, so we cannot use BEGIN...END blocks.
     * 
     * For bootstrap: 3 separate EVENTs with staggered execution times:
     *   +10s: SET GLOBAL group_replication_bootstrap_group=ON
     *   +12s: START GROUP_REPLICATION USER='app'
     *   +14s: SET GLOBAL group_replication_bootstrap_group=OFF
     * 
     * For join: 1 EVENT at +10s to start GR
     * 
     * ON COMPLETION NOT PRESERVE ensures EVENTs are automatically dropped after execution.
     * If GR is already running on restart, the START command will fail but mysqld continues. */
    if (is_bootstrap) {
        offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
            "-- Bootstrap the group (first node) using delayed EVENTs\n"
            "-- EVENTs are used because GR internal session is not ready during init-file execution\n"
            "-- Each EVENT has single-statement body (init-file doesn't support DELIMITER)\n"
            "-- Events are created in mysql schema (fully qualified names avoid 'No database selected' error)\n"
            "-- DEFINER='app'@'%%' ensures events run with proper privileges (app user created above)\n\n"
            "-- Drop any existing events from previous failed starts\n"
            "DROP EVENT IF EXISTS mysql.gr_bootstrap_on;\n"
            "DROP EVENT IF EXISTS mysql.gr_start;\n"
            "DROP EVENT IF EXISTS mysql.gr_bootstrap_off;\n\n"
            "-- EVENT 1: Enable bootstrap mode (+10 seconds)\n"
            "CREATE DEFINER='app'@'%%' EVENT mysql.gr_bootstrap_on\n"
            "  ON SCHEDULE AT CURRENT_TIMESTAMP + INTERVAL 10 SECOND\n"
            "  ON COMPLETION NOT PRESERVE\n"
            "  DO SET GLOBAL group_replication_bootstrap_group=ON;\n\n"
            "-- EVENT 2: Start Group Replication (+12 seconds)\n"
            "CREATE DEFINER='app'@'%%' EVENT mysql.gr_start\n"
            "  ON SCHEDULE AT CURRENT_TIMESTAMP + INTERVAL 12 SECOND\n"
            "  ON COMPLETION NOT PRESERVE\n"
            "  DO START GROUP_REPLICATION USER='app';\n\n"
            "-- EVENT 3: Disable bootstrap mode (+14 seconds)\n"
            "CREATE DEFINER='app'@'%%' EVENT mysql.gr_bootstrap_off\n"
            "  ON SCHEDULE AT CURRENT_TIMESTAMP + INTERVAL 14 SECOND\n"
            "  ON COMPLETION NOT PRESERVE\n"
            "  DO SET GLOBAL group_replication_bootstrap_group=OFF;\n");
    } else {
        offset += snprintf(sql_content + offset, sizeof(sql_content) - offset,
            "-- Join existing group using delayed EVENT\n"
            "-- EVENT is used because GR internal session is not ready during init-file execution\n"
            "-- Event is created in mysql schema (fully qualified name avoids 'No database selected' error)\n"
            "-- DEFINER='app'@'%%' ensures event runs with proper privileges (app user created above)\n\n"
            "-- Drop any existing event from previous failed starts\n"
            "DROP EVENT IF EXISTS mysql.gr_start;\n\n"
            "-- Start Group Replication (+10 seconds after server is ready)\n"
            "CREATE DEFINER='app'@'%%' EVENT mysql.gr_start\n"
            "  ON SCHEDULE AT CURRENT_TIMESTAMP + INTERVAL 10 SECOND\n"
            "  ON COMPLETION NOT PRESERVE\n"
            "  DO START GROUP_REPLICATION USER='app';\n");
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
    const char *whitelist_config;      /* RA_TLS_WHITELIST_CONFIG */
    const char *cert_path;             /* RA_TLS_CERT_PATH */
    const char *key_path;              /* RA_TLS_KEY_PATH */
    const char *data_dir;              /* MYSQL_DATA_DIR */
    
    /* RA-TLS configuration (from manifest, can be overridden) */
    const char *ra_tls_cert_algorithm;           /* RA_TLS_CERT_ALGORITHM */
    const char *ratls_enable_verify;             /* RA_TLS_ENABLE_VERIFY */
    const char *ratls_require_peer_cert;         /* RA_TLS_REQUIRE_PEER_CERT */
    const char *ra_tls_allow_outdated_tcb;       /* RA_TLS_ALLOW_OUTDATED_TCB_INSECURE */
    const char *ra_tls_allow_hw_config_needed;   /* RA_TLS_ALLOW_HW_CONFIG_NEEDED */
    const char *ra_tls_allow_sw_hardening_needed;/* RA_TLS_ALLOW_SW_HARDENING_NEEDED */
    
    /* Group Replication options */
    const char *gr_group_name;         /* --gr-group-name */
    const char *gr_seeds;              /* --gr-seeds */
    const char *gr_local_address;      /* --gr-local-address */
    int gr_port;                       /* --gr-port / GR_PORT: XCom communication port */
    int gr_port_specified;             /* 1 if user explicitly specified GR port */
    int gr_bootstrap;                  /* --gr-bootstrap */
    int gr_debug;                      /* --gr-debug: enable verbose GR logging */
    
    /* MySQL port option (for host network mode with multiple instances) */
    int mysql_port;                    /* --mysql-port / MYSQL_PORT: MySQL service port */
    int mysql_port_specified;          /* 1 if user explicitly specified MySQL port */
    
    /* Testing options */
    int dry_run;                       /* --dry-run: run all logic but skip execve() */
    const char *test_lan_ip;           /* --test-lan-ip: override LAN IP for testing */
    const char *test_output_dir;       /* --test-output-dir: override output directory for testing */
    
    /* GCS debug trace path option */
    const char *gcs_debug_trace_path;  /* --gcs-debug-trace-path: set GCS_DEBUG_TRACE_PATH env var for GR plugin */
    
    /* Additional MySQL args to pass through */
    int mysql_argc;
    char **mysql_argv;
};

/* Parse command line arguments - args take priority over environment variables */
static void parse_args(int argc, char *argv[], struct launcher_config *config) {
    /* Initialize all options to NULL/0 first */
    config->contract_address = NULL;
    config->rpc_url = NULL;
    config->whitelist_config = NULL;
    config->cert_path = NULL;
    config->key_path = NULL;
    config->data_dir = NULL;
    
    config->ra_tls_cert_algorithm = NULL;
    config->ratls_enable_verify = NULL;
    config->ratls_require_peer_cert = NULL;
    config->ra_tls_allow_outdated_tcb = NULL;
    config->ra_tls_allow_hw_config_needed = NULL;
    config->ra_tls_allow_sw_hardening_needed = NULL;
    
    config->gr_group_name = NULL;
    config->gr_seeds = NULL;
    config->gr_local_address = NULL;
    config->gr_bootstrap = 0;
    config->gr_debug = 0;
    config->gr_port = GR_DEFAULT_PORT;
    config->gr_port_specified = 0;
    config->mysql_port = 0;
    config->mysql_port_specified = 0;
    
    config->dry_run = 0;
    config->test_lan_ip = NULL;
    config->test_output_dir = NULL;
    config->gcs_debug_trace_path = NULL;
    
    /* Allocate array for MySQL passthrough args */
    config->mysql_argv = malloc(argc * sizeof(char *));
    config->mysql_argc = 0;
    
    /* Track which options were set via command-line (for warning when env var overrides) */
    int cli_gr_port = 0, cli_mysql_port = 0;
    int cli_gr_bootstrap = 0, cli_gr_debug = 0, cli_dry_run = 0;
    
    /* STEP 1: Parse command-line arguments first
     * Supports both --arg=value and --arg value formats for options that take values */
    
    /* Helper macro to parse an option that takes a value
     * Supports both --option=value and --option value formats */
    #define PARSE_OPTION(opt_name, opt_len, target) \
        if (strncmp(argv[i], opt_name "=", (opt_len) + 1) == 0) { \
            target = argv[i] + (opt_len) + 1; \
        } else if (strcmp(argv[i], opt_name) == 0) { \
            if (i + 1 < argc && argv[i + 1][0] != '-') { \
                target = argv[++i]; \
            } else { \
                fprintf(stderr, "[Launcher] Warning: %s requires a value\n", opt_name); \
            } \
        }
    
    for (int i = 1; i < argc; i++) {
        if (0) {
            /* Placeholder for else-if chain */
        }
        /* Contract and RPC options */
        else PARSE_OPTION("--contract-address", 18, config->contract_address)
        else PARSE_OPTION("--rpc-url", 9, config->rpc_url)
        /* NOTE: --whitelist-config is NOT allowed via command-line for security */
        /* It can only be set via environment variables in the manifest */
        else PARSE_OPTION("--cert-path", 11, config->cert_path)
        /* NOTE: --key-path and --data-dir are NOT allowed via command-line to prevent data leakage */
        /* They can only be set via environment variables in the manifest */
        /* RA-TLS options */
        else PARSE_OPTION("--ra-tls-cert-algorithm", 23, config->ra_tls_cert_algorithm)
        else PARSE_OPTION("--ratls-enable-verify", 21, config->ratls_enable_verify)
        else PARSE_OPTION("--ratls-require-peer-cert", 25, config->ratls_require_peer_cert)
        else PARSE_OPTION("--ra-tls-allow-outdated-tcb", 27, config->ra_tls_allow_outdated_tcb)
        else PARSE_OPTION("--ra-tls-allow-hw-config-needed", 31, config->ra_tls_allow_hw_config_needed)
        else PARSE_OPTION("--ra-tls-allow-sw-hardening-needed", 34, config->ra_tls_allow_sw_hardening_needed)
        /* Group Replication options */
        else PARSE_OPTION("--gr-group-name", 15, config->gr_group_name)
        else PARSE_OPTION("--gr-seeds", 10, config->gr_seeds)
        else PARSE_OPTION("--gr-local-address", 18, config->gr_local_address)
        else if (strcmp(argv[i], "--gr-bootstrap") == 0) {
            config->gr_bootstrap = 1;
            cli_gr_bootstrap = 1;
        } else if (strncmp(argv[i], "--gr-bootstrap=", 15) == 0) {
            const char *val = argv[i] + 15;
            config->gr_bootstrap = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
            cli_gr_bootstrap = 1;
        } else if (strcmp(argv[i], "--gr-debug") == 0) {
            config->gr_debug = 1;
            cli_gr_debug = 1;
        } else if (strncmp(argv[i], "--gr-debug=", 11) == 0) {
            const char *val = argv[i] + 11;
            config->gr_debug = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
            cli_gr_debug = 1;
        }
        /* Port options (for host network mode with multiple instances) */
        else if (strncmp(argv[i], "--gr-port=", 10) == 0) {
            config->gr_port = atoi(argv[i] + 10);
            if (config->gr_port <= 0 || config->gr_port > 65535) {
                fprintf(stderr, "[Launcher] Error: Invalid --gr-port value '%s'\n", argv[i] + 10);
                exit(1);
            }
            config->gr_port_specified = 1;
            cli_gr_port = 1;
        } else if (strcmp(argv[i], "--gr-port") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->gr_port = atoi(argv[++i]);
                if (config->gr_port <= 0 || config->gr_port > 65535) {
                    fprintf(stderr, "[Launcher] Error: Invalid --gr-port value '%s'\n", argv[i]);
                    exit(1);
                }
                config->gr_port_specified = 1;
                cli_gr_port = 1;
            } else {
                fprintf(stderr, "[Launcher] Error: --gr-port requires a value\n");
                exit(1);
            }
        } else if (strncmp(argv[i], "--mysql-port=", 13) == 0) {
            config->mysql_port = atoi(argv[i] + 13);
            if (config->mysql_port <= 0 || config->mysql_port > 65535) {
                fprintf(stderr, "[Launcher] Error: Invalid --mysql-port value '%s'\n", argv[i] + 13);
                exit(1);
            }
            config->mysql_port_specified = 1;
            cli_mysql_port = 1;
        } else if (strcmp(argv[i], "--mysql-port") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->mysql_port = atoi(argv[++i]);
                if (config->mysql_port <= 0 || config->mysql_port > 65535) {
                    fprintf(stderr, "[Launcher] Error: Invalid --mysql-port value '%s'\n", argv[i]);
                    exit(1);
                }
                config->mysql_port_specified = 1;
                cli_mysql_port = 1;
            } else {
                fprintf(stderr, "[Launcher] Error: --mysql-port requires a value\n");
                exit(1);
            }
        }
        /* Testing options */
        else if (strcmp(argv[i], "--dry-run") == 0) {
            config->dry_run = 1;
            cli_dry_run = 1;
        } else if (strncmp(argv[i], "--dry-run=", 10) == 0) {
            const char *val = argv[i] + 10;
            config->dry_run = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
            cli_dry_run = 1;
        }
        else PARSE_OPTION("--test-lan-ip", 13, config->test_lan_ip)
        else PARSE_OPTION("--test-output-dir", 17, config->test_output_dir)
        /* GCS debug trace path option */
        else PARSE_OPTION("--gcs-debug-trace-path", 22, config->gcs_debug_trace_path)
        /* Help */
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            /* Pass through to MySQL */
            config->mysql_argv[config->mysql_argc++] = argv[i];
        }
    }
    
    #undef PARSE_OPTION
    
    /* STEP 2: Apply environment variables - they take PRIORITY over command-line args
     * Print warning when env var overrides a CLI arg */
    
    /* Helper macro to apply env var with warning if it overrides CLI arg */
    #define APPLY_ENV_VAR(env_name, target, cli_was_set) do { \
        const char *env_val = getenv(env_name); \
        if (env_val && strlen(env_val) > 0) { \
            if ((cli_was_set) && target) { \
                fprintf(stderr, "[Launcher] Warning: Environment variable %s overrides command-line argument (env=%s, cli=%s)\n", \
                        env_name, env_val, (const char*)target); \
            } \
            target = env_val; \
        } \
    } while(0)
    
    #define APPLY_ENV_VAR_INT(env_name, target, target_specified, cli_was_set, default_val) do { \
        const char *env_val = getenv(env_name); \
        if (env_val && strlen(env_val) > 0) { \
            int new_val = atoi(env_val); \
            if (new_val > 0 && new_val <= 65535) { \
                if (cli_was_set) { \
                    fprintf(stderr, "[Launcher] Warning: Environment variable %s overrides command-line argument (env=%d, cli=%d)\n", \
                            env_name, new_val, target); \
                } \
                target = new_val; \
                target_specified = 1; \
            } \
        } \
    } while(0)
    
    #define APPLY_ENV_VAR_BOOL(env_name, target, cli_was_set) do { \
        const char *env_val = getenv(env_name); \
        if (env_val && strlen(env_val) > 0) { \
            int new_val = (strcmp(env_val, "1") == 0 || strcasecmp(env_val, "true") == 0); \
            if (cli_was_set && target != new_val) { \
                fprintf(stderr, "[Launcher] Warning: Environment variable %s overrides command-line argument (env=%s, cli=%d)\n", \
                        env_name, env_val, target); \
            } \
            target = new_val; \
        } \
    } while(0)
    
    /* Contract and RPC options */
    APPLY_ENV_VAR("CONTRACT_ADDRESS", config->contract_address, config->contract_address != NULL);
    APPLY_ENV_VAR("RPC_URL", config->rpc_url, config->rpc_url != NULL);
    
    /* Whitelist config - only from env var (security) */
    config->whitelist_config = getenv("RA_TLS_WHITELIST_CONFIG");
    
    /* Path options */
    APPLY_ENV_VAR("RA_TLS_CERT_PATH", config->cert_path, config->cert_path != NULL);
    config->key_path = getenv("RA_TLS_KEY_PATH");  /* Only from env var (security) */
    config->data_dir = getenv("MYSQL_DATA_DIR");   /* Only from env var (security) */
    
    /* RA-TLS options */
    APPLY_ENV_VAR("RA_TLS_CERT_ALGORITHM", config->ra_tls_cert_algorithm, config->ra_tls_cert_algorithm != NULL);
    APPLY_ENV_VAR("RA_TLS_ENABLE_VERIFY", config->ratls_enable_verify, config->ratls_enable_verify != NULL);
    APPLY_ENV_VAR("RA_TLS_REQUIRE_PEER_CERT", config->ratls_require_peer_cert, config->ratls_require_peer_cert != NULL);
    APPLY_ENV_VAR("RA_TLS_ALLOW_OUTDATED_TCB_INSECURE", config->ra_tls_allow_outdated_tcb, config->ra_tls_allow_outdated_tcb != NULL);
    APPLY_ENV_VAR("RA_TLS_ALLOW_HW_CONFIG_NEEDED", config->ra_tls_allow_hw_config_needed, config->ra_tls_allow_hw_config_needed != NULL);
    APPLY_ENV_VAR("RA_TLS_ALLOW_SW_HARDENING_NEEDED", config->ra_tls_allow_sw_hardening_needed, config->ra_tls_allow_sw_hardening_needed != NULL);
    
    /* Group Replication options */
    APPLY_ENV_VAR("MYSQL_GR_GROUP_NAME", config->gr_group_name, config->gr_group_name != NULL);
    APPLY_ENV_VAR("GR_SEEDS", config->gr_seeds, config->gr_seeds != NULL);
    APPLY_ENV_VAR("GR_LOCAL_ADDRESS", config->gr_local_address, config->gr_local_address != NULL);
    APPLY_ENV_VAR_BOOL("GR_BOOTSTRAP", config->gr_bootstrap, cli_gr_bootstrap);
    APPLY_ENV_VAR_BOOL("GR_DEBUG", config->gr_debug, cli_gr_debug);
    
    /* Port options */
    APPLY_ENV_VAR_INT("GR_PORT", config->gr_port, config->gr_port_specified, cli_gr_port, GR_DEFAULT_PORT);
    APPLY_ENV_VAR_INT("MYSQL_PORT", config->mysql_port, config->mysql_port_specified, cli_mysql_port, 0);
    
    /* Testing options */
    APPLY_ENV_VAR_BOOL("DRY_RUN", config->dry_run, cli_dry_run);
    APPLY_ENV_VAR("TEST_LAN_IP", config->test_lan_ip, config->test_lan_ip != NULL);
    APPLY_ENV_VAR("TEST_OUTPUT_DIR", config->test_output_dir, config->test_output_dir != NULL);
    
    /* GCS debug trace path option */
    APPLY_ENV_VAR("GCS_DEBUG_TRACE_PATH", config->gcs_debug_trace_path, config->gcs_debug_trace_path != NULL);
    
    #undef APPLY_ENV_VAR
    #undef APPLY_ENV_VAR_INT
    #undef APPLY_ENV_VAR_BOOL
    
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
    printf("\n");
    printf("  NOTE: RA_TLS_WHITELIST_CONFIG can ONLY be set via manifest environment variable\n");
    printf("        (not command-line) for security. If both contract and env var are set,\n");
    printf("        their whitelists are merged with column-based deduplication.\n\n");
    
    printf("PATH OPTIONS:\n");
    printf("  --cert-path=PATH          Path for RA-TLS certificate\n");
    printf("                            (env: RA_TLS_CERT_PATH, default: %s)\n", DEFAULT_CERT_PATH);
    printf("\n");
    printf("  NOTE: The following paths can ONLY be set via manifest environment variables\n");
    printf("        (not command-line) to prevent data leakage:\n");
    printf("        - RA_TLS_KEY_PATH: RA-TLS private key path (default: %s)\n", DEFAULT_KEY_PATH);
    printf("        - MYSQL_DATA_DIR: MySQL data directory (default: %s)\n\n", DEFAULT_DATA_DIR);
    
    printf("RA-TLS CONFIGURATION OPTIONS:\n");
    printf("  --ra-tls-cert-algorithm=ALG\n");
    printf("                            Certificate algorithm (e.g., secp256r1, secp256k1)\n");
    printf("                            (env: RA_TLS_CERT_ALGORITHM)\n");
    printf("  --ratls-enable-verify=0|1\n");
    printf("                            Enable RA-TLS verification (default: 1)\n");
    printf("                            (env: RA_TLS_ENABLE_VERIFY)\n");
    printf("  --ratls-require-peer-cert=0|1\n");
    printf("                            Require peer certificate for mutual TLS (default: 1)\n");
    printf("                            (env: RA_TLS_REQUIRE_PEER_CERT)\n");
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
    printf("                            (port defaults to --gr-port value if not specified)\n");
    printf("                            (env: GR_SEEDS)\n");
    printf("                            Note: Local LAN IP and public IP are automatically added\n");
    printf("  --gr-local-address=IP     Override local IP address for GR communication\n");
    printf("                            (env: GR_LOCAL_ADDRESS)\n");
    printf("                            (default: auto-detect LAN IP, port is set by --gr-port)\n");
    printf("  --gr-port=PORT            XCom communication port for Group Replication (default: %d)\n", GR_DEFAULT_PORT);
    printf("                            (env: GR_PORT)\n");
    printf("                            Use different ports for multiple instances on same host\n");
    printf("  --gr-bootstrap            Bootstrap a new replication group (first node only)\n");
    printf("                            (env: GR_BOOTSTRAP=1|true)\n");
    printf("                            Without this flag, node will try to join existing group\n");
    printf("  --gr-debug                Enable verbose GR logging for debugging and troubleshooting\n");
    printf("                            (env: GR_DEBUG=1|true)\n");
    printf("                            Logs XCom communication details to MySQL error log\n\n");
    
    printf("PORT OPTIONS (for host network mode with multiple instances):\n");
    printf("  --mysql-port=PORT         MySQL service port (default: 3306)\n");
    printf("                            (env: MYSQL_PORT)\n");
    printf("                            Use different ports for multiple instances on same host\n");
    printf("  NOTE: Port availability is checked at startup:\n");
    printf("        - If you specify a port and it's occupied: launcher exits with error\n");
    printf("        - If using default port and it's occupied: auto-increments to find available port\n\n");
    
    printf("TESTING OPTIONS:\n");
    printf("  --dry-run                 Run all logic but skip execve() to mysqld\n");
    printf("                            (env: DRY_RUN=1|true)\n");
    printf("                            Useful for testing configuration generation\n");
    printf("  --test-lan-ip=IP          Override LAN IP detection (for testing)\n");
    printf("                            (env: TEST_LAN_IP)\n");
    printf("  --test-output-dir=DIR     Override output directory for config files (for testing)\n");
    printf("                            (env: TEST_OUTPUT_DIR)\n\n");
    
    printf("DEBUG OPTIONS:\n");
    printf("  --gcs-debug-trace-path=DIR\n");
    printf("                            Set GCS_DEBUG_TRACE output directory for GR plugin\n");
    printf("                            (env: GCS_DEBUG_TRACE_PATH, takes priority over CLI)\n");
    printf("                            Default: MySQL data directory (encrypted partition)\n");
    printf("                            Use this to write debug traces to a readable location\n\n");
    
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
    printf("  Environment variables take PRIORITY over command-line arguments.\n");
    printf("  When an env var overrides a CLI arg, a warning is printed.\n");
}

/* Validate and normalize configuration - handle mutual exclusions and dependencies */
static int validate_config(struct launcher_config *config) {
    int has_errors = 0;
    
    printf("\n[Launcher] Validating configuration...\n");
    
    /* === Whitelist / Contract / RPC configuration === */
    /* Rule: If both contract and env var whitelist are set, they will be merged with column-based deduplication */
    /* Note: --whitelist-config is NOT allowed via command-line, only via manifest env var */
    if (config->rpc_url && strlen(config->rpc_url) > 0) {
        /* If rpc-url is set but contract-address is missing, warn */
        if (!config->contract_address || strlen(config->contract_address) == 0) {
            printf("[Launcher] Warning: --rpc-url specified but --contract-address is missing\n");
            printf("[Launcher]          Cannot read whitelist from contract without contract address\n");
        } else {
            printf("[Launcher] Contract whitelist configured (will merge with env var if set)\n");
        }
    }
    
    /* If contract-address is set but rpc-url is missing, warn and fall back to env whitelist */
    if (config->contract_address && strlen(config->contract_address) > 0) {
        if (!config->rpc_url || strlen(config->rpc_url) == 0) {
            printf("[Launcher] Warning: --contract-address specified but --rpc-url is missing\n");
            printf("[Launcher]          Using environment whitelist only (if set)\n");
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
    
    /* Extract RA_TLS_WHITELIST_CONFIG field */
    cJSON *whitelist_config = cJSON_GetObjectItem(json, "RA_TLS_WHITELIST_CONFIG");
    if (whitelist_config && whitelist_config->valuestring) {
        whitelist = strdup(whitelist_config->valuestring);
        printf("[Launcher] Found RA_TLS_WHITELIST_CONFIG in SGX config\n");
    } else {
        fprintf(stderr, "[Launcher] RA_TLS_WHITELIST_CONFIG field not found in SGX config\n");
    }
    
    cJSON_Delete(json);
    free(sgx_config);
    
    return whitelist;
}

/* ============================================
 * Whitelist Merge Helper Functions
 * ============================================ */

/* Base64 decoding table */
static const unsigned char base64_decode_table[256] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63,
     52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,  0,255,255,
    255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
    255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
     41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255
};

/* Base64 encoding table */
static const char base64_encode_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Decode Base64 string, returns decoded length or -1 on error */
static int base64_decode(const char *input, unsigned char *output, size_t output_size) {
    size_t input_len = strlen(input);
    size_t output_len = 0;
    unsigned int buf = 0;
    int bits = 0;
    
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        
        unsigned char val = base64_decode_table[c];
        if (val == 255) {
            fprintf(stderr, "[Launcher] Invalid Base64 character: %c\n", c);
            return -1;
        }
        
        buf = (buf << 6) | val;
        bits += 6;
        
        if (bits >= 8) {
            bits -= 8;
            if (output_len >= output_size) return -1;
            output[output_len++] = (buf >> bits) & 0xFF;
        }
    }
    
    return (int)output_len;
}

/* Encode data to Base64 string, returns allocated string or NULL on error */
static char *base64_encode(const unsigned char *input, size_t input_len) {
    size_t output_len = ((input_len + 2) / 3) * 4 + 1;
    char *output = malloc(output_len);
    if (!output) return NULL;
    
    size_t i, j;
    for (i = 0, j = 0; i < input_len; ) {
        uint32_t octet_a = i < input_len ? input[i++] : 0;
        uint32_t octet_b = i < input_len ? input[i++] : 0;
        uint32_t octet_c = i < input_len ? input[i++] : 0;
        
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        
        output[j++] = base64_encode_table[(triple >> 18) & 0x3F];
        output[j++] = base64_encode_table[(triple >> 12) & 0x3F];
        output[j++] = (i > input_len + 1) ? '=' : base64_encode_table[(triple >> 6) & 0x3F];
        output[j++] = (i > input_len) ? '=' : base64_encode_table[triple & 0x3F];
    }
    output[j] = '\0';
    
    return output;
}

/* Whitelist CSV structure: 5 lines, each with comma-separated values */
#define WHITELIST_NUM_LINES 5
#define MAX_VALUES_PER_LINE 256
#define MAX_VALUE_LEN 128

struct whitelist_line {
    char values[MAX_VALUES_PER_LINE][MAX_VALUE_LEN];
    int count;
};

struct whitelist_csv {
    struct whitelist_line lines[WHITELIST_NUM_LINES];
};

/* Parse CSV line into values array */
static void parse_csv_line(const char *line, struct whitelist_line *wl_line) {
    wl_line->count = 0;
    if (!line || strlen(line) == 0) {
        /* Empty line: treat as single "0" value */
        strcpy(wl_line->values[0], "0");
        wl_line->count = 1;
        return;
    }
    
    /* Check if line is just "0" */
    if (strcmp(line, "0") == 0) {
        strcpy(wl_line->values[0], "0");
        wl_line->count = 1;
        return;
    }
    
    const char *start = line;
    const char *end;
    
    while (*start && wl_line->count < MAX_VALUES_PER_LINE) {
        /* Skip leading whitespace */
        while (*start == ' ' || *start == '\t') start++;
        
        /* Find end of value (comma or end of string) */
        end = strchr(start, ',');
        if (!end) end = start + strlen(start);
        
        /* Copy value, trimming trailing whitespace */
        size_t len = end - start;
        while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t')) len--;
        
        if (len > 0 && len < MAX_VALUE_LEN) {
            strncpy(wl_line->values[wl_line->count], start, len);
            wl_line->values[wl_line->count][len] = '\0';
            wl_line->count++;
        }
        
        if (*end == ',') {
            start = end + 1;
        } else {
            break;
        }
    }
    
    /* If no values parsed, treat as "0" */
    if (wl_line->count == 0) {
        strcpy(wl_line->values[0], "0");
        wl_line->count = 1;
    }
}

/* Parse Base64-encoded CSV whitelist */
static int parse_whitelist(const char *base64_str, struct whitelist_csv *csv) {
    if (!base64_str || strlen(base64_str) == 0) {
        /* Initialize empty whitelist */
        for (int i = 0; i < WHITELIST_NUM_LINES; i++) {
            csv->lines[i].count = 0;
        }
        return 0;
    }
    
    /* Decode Base64 */
    size_t decoded_size = strlen(base64_str);
    unsigned char *decoded = malloc(decoded_size + 1);
    if (!decoded) return -1;
    
    int decoded_len = base64_decode(base64_str, decoded, decoded_size);
    if (decoded_len < 0) {
        free(decoded);
        return -1;
    }
    decoded[decoded_len] = '\0';
    
    /* Parse lines */
    char *str = (char *)decoded;
    char *line;
    int line_idx = 0;
    
    while ((line = strsep(&str, "\n")) != NULL && line_idx < WHITELIST_NUM_LINES) {
        /* Remove carriage return if present */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\r') {
            line[len-1] = '\0';
        }
        parse_csv_line(line, &csv->lines[line_idx]);
        line_idx++;
    }
    
    /* Fill remaining lines with empty */
    while (line_idx < WHITELIST_NUM_LINES) {
        csv->lines[line_idx].count = 0;
        line_idx++;
    }
    
    free(decoded);
    return 0;
}

/* Check if a rule (same index across all lines) already exists in the whitelist */
static int rule_exists(struct whitelist_csv *csv, const char *values[WHITELIST_NUM_LINES]) {
    /* Find the maximum count across all lines to determine number of rules */
    int max_count = 0;
    for (int i = 0; i < WHITELIST_NUM_LINES; i++) {
        if (csv->lines[i].count > max_count) {
            max_count = csv->lines[i].count;
        }
    }
    
    /* Check each existing rule */
    for (int rule_idx = 0; rule_idx < max_count; rule_idx++) {
        int match = 1;
        for (int line_idx = 0; line_idx < WHITELIST_NUM_LINES; line_idx++) {
            const char *existing_val = (rule_idx < csv->lines[line_idx].count) 
                ? csv->lines[line_idx].values[rule_idx] : "0";
            const char *new_val = values[line_idx] ? values[line_idx] : "0";
            
            if (strcmp(existing_val, new_val) != 0) {
                match = 0;
                break;
            }
        }
        if (match) return 1;  /* Rule exists */
    }
    
    return 0;  /* Rule does not exist */
}

/* Merge two whitelists with rule-based deduplication */
static int merge_whitelists(struct whitelist_csv *dest, struct whitelist_csv *src) {
    /* Find the maximum count in source to determine number of rules to add */
    int src_max_count = 0;
    for (int i = 0; i < WHITELIST_NUM_LINES; i++) {
        if (src->lines[i].count > src_max_count) {
            src_max_count = src->lines[i].count;
        }
    }
    
    /* For each rule in source, check if it exists in dest, if not add it */
    for (int rule_idx = 0; rule_idx < src_max_count; rule_idx++) {
        /* Extract rule values from source */
        const char *rule_values[WHITELIST_NUM_LINES];
        for (int line_idx = 0; line_idx < WHITELIST_NUM_LINES; line_idx++) {
            rule_values[line_idx] = (rule_idx < src->lines[line_idx].count)
                ? src->lines[line_idx].values[rule_idx] : "0";
        }
        
        /* Check if rule already exists in dest */
        if (!rule_exists(dest, rule_values)) {
            /* Add rule to dest */
            for (int line_idx = 0; line_idx < WHITELIST_NUM_LINES; line_idx++) {
                int dest_idx = dest->lines[line_idx].count;
                if (dest_idx < MAX_VALUES_PER_LINE) {
                    strncpy(dest->lines[line_idx].values[dest_idx], 
                            rule_values[line_idx], MAX_VALUE_LEN - 1);
                    dest->lines[line_idx].values[dest_idx][MAX_VALUE_LEN - 1] = '\0';
                    dest->lines[line_idx].count++;
                }
            }
        }
    }
    
    return 0;
}

/* Serialize whitelist back to Base64-encoded CSV */
static char *serialize_whitelist(struct whitelist_csv *csv) {
    /* Calculate required buffer size */
    size_t buf_size = 0;
    for (int line_idx = 0; line_idx < WHITELIST_NUM_LINES; line_idx++) {
        for (int val_idx = 0; val_idx < csv->lines[line_idx].count; val_idx++) {
            buf_size += strlen(csv->lines[line_idx].values[val_idx]) + 1;  /* +1 for comma */
        }
        buf_size += 1;  /* +1 for newline */
    }
    buf_size += 1;  /* +1 for null terminator */
    
    char *csv_str = malloc(buf_size);
    if (!csv_str) return NULL;
    
    size_t offset = 0;
    for (int line_idx = 0; line_idx < WHITELIST_NUM_LINES; line_idx++) {
        for (int val_idx = 0; val_idx < csv->lines[line_idx].count; val_idx++) {
            if (val_idx > 0) {
                csv_str[offset++] = ',';
            }
            size_t val_len = strlen(csv->lines[line_idx].values[val_idx]);
            memcpy(csv_str + offset, csv->lines[line_idx].values[val_idx], val_len);
            offset += val_len;
        }
        csv_str[offset++] = '\n';
    }
    csv_str[offset] = '\0';
    
    /* Encode to Base64 */
    char *base64_str = base64_encode((unsigned char *)csv_str, offset);
    free(csv_str);
    
    return base64_str;
}

/* Merge whitelist from contract with whitelist from environment variable */
static char *merge_whitelist_configs(const char *env_whitelist, const char *contract_whitelist) {
    struct whitelist_csv env_csv = {0};
    struct whitelist_csv contract_csv = {0};
    
    printf("[Launcher] Merging whitelists...\n");
    
    /* Parse environment whitelist */
    if (env_whitelist && strlen(env_whitelist) > 0) {
        if (parse_whitelist(env_whitelist, &env_csv) != 0) {
            fprintf(stderr, "[Launcher] Warning: Failed to parse environment whitelist\n");
        } else {
            printf("[Launcher]   Environment whitelist: %d rules\n", 
                   env_csv.lines[0].count > 0 ? env_csv.lines[0].count : 0);
        }
    }
    
    /* Parse contract whitelist */
    if (contract_whitelist && strlen(contract_whitelist) > 0) {
        if (parse_whitelist(contract_whitelist, &contract_csv) != 0) {
            fprintf(stderr, "[Launcher] Warning: Failed to parse contract whitelist\n");
        } else {
            printf("[Launcher]   Contract whitelist: %d rules\n",
                   contract_csv.lines[0].count > 0 ? contract_csv.lines[0].count : 0);
        }
    }
    
    /* Merge contract whitelist into env whitelist (env is base, contract is added) */
    if (merge_whitelists(&env_csv, &contract_csv) != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to merge whitelists\n");
        return NULL;
    }
    
    printf("[Launcher]   Merged whitelist: %d rules (after deduplication)\n",
           env_csv.lines[0].count > 0 ? env_csv.lines[0].count : 0);
    
    /* Serialize merged whitelist */
    return serialize_whitelist(&env_csv);
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
    for (int i = 0; RA_TLS_LIB_PATHS[i] != NULL; i++) {
        if (file_exists(RA_TLS_LIB_PATHS[i])) {
            return RA_TLS_LIB_PATHS[i];
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
    
    /* Check port availability and handle conflicts
     * - If user explicitly specified a port and it's occupied: error and exit
     * - If port was not specified (using default) and it's occupied: auto-increment until available
     */
    printf("[Launcher] Checking port availability...\n");
    
    /* Check MySQL port (default 3306 if mysql_port is 0) */
    int mysql_port_to_check = (config.mysql_port > 0) ? config.mysql_port : 3306;
    int mysql_port_available = is_port_available(mysql_port_to_check);
    
    if (mysql_port_available == 0) {
        /* Port is occupied */
        if (config.mysql_port_specified) {
            /* User explicitly specified this port - error */
            fprintf(stderr, "[Launcher] ERROR: MySQL port %d is already in use\n", mysql_port_to_check);
            fprintf(stderr, "[Launcher] Please specify a different port with --mysql-port or MYSQL_PORT\n");
            curl_global_cleanup();
            return 1;
        } else {
            /* Port was not specified - auto-increment to find available port */
            printf("[Launcher] MySQL port %d is occupied, searching for available port...\n", mysql_port_to_check);
            int new_port = find_available_port(mysql_port_to_check + 1);
            if (new_port < 0) {
                fprintf(stderr, "[Launcher] ERROR: Could not find available MySQL port (tried %d-65535)\n", mysql_port_to_check);
                curl_global_cleanup();
                return 1;
            }
            config.mysql_port = new_port;
            printf("[Launcher] Auto-selected MySQL port: %d\n", config.mysql_port);
        }
    } else if (mysql_port_available == 1) {
        /* Port is available */
        if (config.mysql_port == 0) {
            /* Using default port 3306, set it explicitly now */
            config.mysql_port = 3306;
        }
        printf("[Launcher] MySQL port %d is available\n", config.mysql_port);
    } else {
        /* Error checking port - proceed with caution */
        fprintf(stderr, "[Launcher] Warning: Could not verify MySQL port %d availability\n", mysql_port_to_check);
        if (config.mysql_port == 0) {
            config.mysql_port = 3306;
        }
    }
    
    /* Check GR XCom port */
    int gr_port_available = is_port_available(config.gr_port);
    
    if (gr_port_available == 0) {
        /* Port is occupied */
        if (config.gr_port_specified) {
            /* User explicitly specified this port - error */
            fprintf(stderr, "[Launcher] ERROR: GR XCom port %d is already in use\n", config.gr_port);
            fprintf(stderr, "[Launcher] Please specify a different port with --gr-port or GR_PORT\n");
            curl_global_cleanup();
            return 1;
        } else {
            /* Port was not specified - auto-increment to find available port */
            printf("[Launcher] GR XCom port %d is occupied, searching for available port...\n", config.gr_port);
            int new_port = find_available_port(config.gr_port + 1);
            if (new_port < 0) {
                fprintf(stderr, "[Launcher] ERROR: Could not find available GR XCom port (tried %d-65535)\n", config.gr_port);
                curl_global_cleanup();
                return 1;
            }
            config.gr_port = new_port;
            printf("[Launcher] Auto-selected GR XCom port: %d\n", config.gr_port);
        }
    } else if (gr_port_available == 1) {
        printf("[Launcher] GR XCom port %d is available\n", config.gr_port);
    } else {
        /* Error checking port - proceed with caution */
        fprintf(stderr, "[Launcher] Warning: Could not verify GR XCom port %d availability\n", config.gr_port);
    }
    
    printf("[Launcher] Final ports - MySQL: %d, GR XCom: %d\n\n", config.mysql_port, config.gr_port);
    
    /* Set RA-TLS configuration from parsed config (args override env vars) */
    printf("[Launcher] Setting up RA-TLS configuration...\n");
    
    /* Apply RA-TLS settings from config (command-line args take priority) */
    if (config.ra_tls_cert_algorithm && strlen(config.ra_tls_cert_algorithm) > 0) {
        set_env("RA_TLS_CERT_ALGORITHM", config.ra_tls_cert_algorithm, 1);
    }
    if (config.ratls_enable_verify && strlen(config.ratls_enable_verify) > 0) {
        set_env("RA_TLS_ENABLE_VERIFY", config.ratls_enable_verify, 1);
    } else {
        set_env("RA_TLS_ENABLE_VERIFY", "1", 1);  /* Default: enable verification */
    }
    if (config.ratls_require_peer_cert && strlen(config.ratls_require_peer_cert) > 0) {
        set_env("RA_TLS_REQUIRE_PEER_CERT", config.ratls_require_peer_cert, 1);
    } else {
        set_env("RA_TLS_REQUIRE_PEER_CERT", "1", 1);  /* Default: require peer cert */
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
    set_env("RA_TLS_CERT_PATH", config.cert_path, 1);
    set_env("RA_TLS_KEY_PATH", config.key_path, 1);
    
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
    
    /* Create MySQL runtime directory for socket and pid file */
    printf("[Launcher] Creating MySQL runtime directory: /var/run/mysqld\n");
    if (mkdir_p("/var/run/mysqld") != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create MySQL runtime directory: %s\n", strerror(errno));
    }
    
    /* Create MySQL secure file operations directory (for LOAD DATA INFILE, etc.) */
    printf("[Launcher] Creating MySQL secure files directory: /var/lib/mysql-files\n");
    if (mkdir_p("/var/lib/mysql-files") != 0) {
        fprintf(stderr, "[Launcher] Warning: Failed to create MySQL secure files directory: %s\n", strerror(errno));
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
    /* Note: RA_TLS_WHITELIST_CONFIG can ONLY be set via manifest environment variable (not command-line) */
    /* If both contract and env var are set, they are merged with rule-based deduplication */
    printf("\n[Launcher] Whitelist Configuration:\n");
    
    /* Get environment whitelist (from manifest) */
    const char *env_whitelist = config.whitelist_config;  /* Already loaded from getenv in parse_args */
    char *contract_whitelist = NULL;
    char *merged_whitelist = NULL;
    
    if (env_whitelist && strlen(env_whitelist) > 0) {
        printf("[Launcher] Environment whitelist is set (from manifest)\n");
    } else {
        printf("[Launcher] No environment whitelist set\n");
    }
    
    /* Try to read whitelist from contract if configured */
    if (config.contract_address && strlen(config.contract_address) > 0 &&
        config.rpc_url && strlen(config.rpc_url) > 0) {
        printf("[Launcher] Contract address specified: %s\n", config.contract_address);
        printf("[Launcher] RPC URL specified: %s\n", config.rpc_url);
        
        contract_whitelist = read_whitelist_from_contract(config.contract_address, config.rpc_url);
        
        if (contract_whitelist) {
            printf("[Launcher] Successfully read whitelist from contract\n");
        } else {
            printf("[Launcher] Could not read valid whitelist from contract\n");
        }
    } else if (config.contract_address && strlen(config.contract_address) > 0) {
        printf("[Launcher] Contract address specified but RPC_URL not set, cannot read from contract\n");
    } else {
        printf("[Launcher] No CONTRACT_ADDRESS specified\n");
    }
    
    /* Merge whitelists if both are available, otherwise use whichever is set */
    if (env_whitelist && strlen(env_whitelist) > 0 && contract_whitelist && strlen(contract_whitelist) > 0) {
        /* Both whitelists available - merge them */
        merged_whitelist = merge_whitelist_configs(env_whitelist, contract_whitelist);
        if (merged_whitelist) {
            set_env("RA_TLS_WHITELIST_CONFIG", merged_whitelist, 1);
            free(merged_whitelist);
        } else {
            fprintf(stderr, "[Launcher] Warning: Failed to merge whitelists, using environment whitelist only\n");
            /* env_whitelist is already set in environment from manifest */
        }
    } else if (contract_whitelist && strlen(contract_whitelist) > 0) {
        /* Only contract whitelist available */
        set_env("RA_TLS_WHITELIST_CONFIG", contract_whitelist, 1);
    } else if (env_whitelist && strlen(env_whitelist) > 0) {
        /* Only environment whitelist available - already set from manifest */
        printf("[Launcher] Using environment whitelist only\n");
    }
    
    /* Clean up contract whitelist if allocated */
    if (contract_whitelist) {
        free(contract_whitelist);
    }
    
    /* Display final whitelist status */
    const char *final_whitelist = getenv("RA_TLS_WHITELIST_CONFIG");
    if (final_whitelist && strlen(final_whitelist) > 0) {
        printf("[Launcher] RA_TLS_WHITELIST_CONFIG is set\n");
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
    char seeds_list[MAX_SEEDS_LEN] = {0};
    uint32_t server_id = 0;
    
    /* Handle Group Replication setup if enabled */
    if (gr_enabled) {
        printf("\n[Launcher] Group Replication Configuration:\n");
        printf("[Launcher] Group name: %s\n", gr_group_name);
        printf("[Launcher] Bootstrap mode: %s\n", config.gr_bootstrap ? "YES" : "NO");
        
        /* In non-bootstrap (join) mode, --gr-seeds is required to specify which node to connect to */
        if (!config.gr_bootstrap && (!config.gr_seeds || strlen(config.gr_seeds) == 0)) {
            fprintf(stderr, "[Launcher] ERROR: --gr-seeds is required in join mode (without --gr-bootstrap)\n");
            fprintf(stderr, "[Launcher] You must specify at least one seed node to join the group.\n");
            fprintf(stderr, "[Launcher] Example: --gr-seeds=192.168.1.100:33061\n");
            return 1;
        }
        
        /* Get LAN IP - use test override if provided */
        if (config.test_lan_ip && strlen(config.test_lan_ip) > 0) {
            strncpy(lan_ip, config.test_lan_ip, sizeof(lan_ip) - 1);
            printf("[Launcher] Using test LAN IP: %s\n", lan_ip);
        } else if (get_lan_ip(lan_ip, sizeof(lan_ip)) != 0) {
            fprintf(stderr, "[Launcher] Warning: Could not detect LAN IP\n");
        } else {
            printf("[Launcher] Detected LAN IP: %s\n", lan_ip);
        }
        
        /* Get or create stable server ID */
        server_id = get_or_create_server_id(lan_ip, config.gr_port);
        printf("[Launcher] Server ID: %u\n", server_id);
        
        /* Build seeds list from user-specified seeds only.
         * We no longer auto-add the local node's IP to seeds to avoid self-connection issues.
         * Note: --gr-local-address only accepts IP (no port), port is controlled by --gr-port */
        
        /* Build seeds list: only user-specified seeds, no auto-generated IPs */
        build_seeds_list(seeds_list, sizeof(seeds_list), config.gr_seeds, config.gr_port);
        printf("[Launcher] Seeds list: %s\n", seeds_list);
        printf("[Launcher] GR XCom port: %d\n", config.gr_port);
        
        /* Determine local address for GR
         * Each node MUST have a unique local address for Group Replication to work.
         * Using 127.0.0.1 for all nodes causes "Old incarnation found" errors.
         * Priority: --gr-local-address > LAN IP
         * 
         * IMPORTANT: group_replication_local_address MUST be in host:port format.
         * The port is always taken from --gr-port (which has already been validated for availability).
         * --gr-local-address only specifies the IP address.
         */
        char gr_local_address[MAX_IP_LEN + 16] = {0};
        
        if (config.gr_local_address && strlen(config.gr_local_address) > 0) {
            /* Use the specified IP with the configured GR port */
            snprintf(gr_local_address, sizeof(gr_local_address), "%s:%d", config.gr_local_address, config.gr_port);
        } else if (strlen(lan_ip) > 0) {
            /* Default to LAN IP for unique local address across nodes */
            snprintf(gr_local_address, sizeof(gr_local_address), "%s:%d", lan_ip, config.gr_port);
        } else {
            fprintf(stderr, "[Launcher] ERROR: Could not determine local IP address for Group Replication\n");
            fprintf(stderr, "[Launcher] Please specify --gr-local-address explicitly\n");
            return 1;
        }
        printf("[Launcher] GR local address: %s\n", gr_local_address);
        
        /* Set GR_LOCAL_IP environment variable for the gr_getifaddrs() replacement in MySQL GR plugin.
         * This allows MySQL Group Replication to work in Gramine/SGX environments where
         * the real getifaddrs() fails because it uses netlink sockets.
         * The replacement function returns fake interface data based on these IPs.
         * 
         * GR_LOCAL_IP supports comma-separated list of IPs (e.g., "10.0.0.1,192.168.1.100,203.0.113.50")
         * to support LAN and public IP addresses for cross-datacenter replication.
         * Order: specified IP (if any), LAN IP, public IP - all deduplicated
         * Note: 127.0.0.1 is NOT included to avoid "Old incarnation found" errors in GR.
         */
        char gr_local_ip_list[MAX_IP_LEN * 4 + 8] = {0};  /* Room for four IPs + commas + null */
        int ip_count = 0;
        
        /* Helper function-like macro to add IP if not already in list */
        #define ADD_IP_IF_UNIQUE(ip_var) do { \
            if (strlen(ip_var) > 0) { \
                int found = 0; \
                if (strlen(gr_local_ip_list) > 0) { \
                    char *search_pos = gr_local_ip_list; \
                    while ((search_pos = strstr(search_pos, ip_var)) != NULL) { \
                        char before = (search_pos == gr_local_ip_list) ? ',' : *(search_pos - 1); \
                        char after = *(search_pos + strlen(ip_var)); \
                        if ((before == ',' || search_pos == gr_local_ip_list) && (after == ',' || after == '\0')) { \
                            found = 1; break; \
                        } \
                        search_pos++; \
                    } \
                } \
                if (!found) { \
                    size_t current_len = strlen(gr_local_ip_list); \
                    if (current_len == 0) { \
                        strncpy(gr_local_ip_list, ip_var, sizeof(gr_local_ip_list) - 1); \
                    } else if (current_len + 1 + strlen(ip_var) < sizeof(gr_local_ip_list)) { \
                        strcat(gr_local_ip_list, ","); \
                        strcat(gr_local_ip_list, ip_var); \
                    } \
                    ip_count++; \
                } \
            } \
        } while(0)
        
        /* First, add specified IP (from --gr-local-address) if provided */
        if (config.gr_local_address && strlen(config.gr_local_address) > 0) {
            ADD_IP_IF_UNIQUE(config.gr_local_address);
        }
        
        /* Add auto-detected LAN IP */
        if (strlen(lan_ip) > 0) {
            ADD_IP_IF_UNIQUE(lan_ip);
        }
        
        #undef ADD_IP_IF_UNIQUE
        
        if (strlen(gr_local_ip_list) > 0) {
            set_env("GR_LOCAL_IP", gr_local_ip_list, 1);
            printf("[Launcher] Set GR_LOCAL_IP=%s for gr_getifaddrs() (%d IP(s))\n", gr_local_ip_list, ip_count);
        } else {
            fprintf(stderr, "[Launcher] Warning: Could not determine IP for GR_LOCAL_IP environment variable\n");
        }
        
        /* Create GR config file */
        strncpy(gr_config_path, GR_CONFIG_FILE, sizeof(gr_config_path) - 1);
        if (create_gr_config(GR_CONFIG_FILE, server_id,
                            gr_group_name, gr_local_address, seeds_list,
                            config.gr_bootstrap, cert_path, key_path,
                            config.gr_debug) != 0) {
            fprintf(stderr, "[Launcher] ERROR: Failed to create GR config file\n");
            return 1;
        }
        
        /* Prepare --defaults-extra-file argument */
        snprintf(defaults_extra_file_arg, sizeof(defaults_extra_file_arg),
                 "--defaults-extra-file=%s", GR_CONFIG_FILE);
        
        /* Note: GCS_DEBUG_TRACE will be written to ${datadir}/GCS_DEBUG_TRACE by default (encrypted partition).
         * Use --gcs-debug-trace-path or GCS_DEBUG_TRACE_PATH env var to redirect to a readable location.
         * The GR plugin patch reads this env var and writes debug traces to the specified directory. */
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
                               config.gr_bootstrap) == 0) {
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
    if (config.mysql_port > 0) extra_args++;  /* --port */
    
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
    char mysql_port_arg[32] = {0};
    
    snprintf(ssl_cert_arg, sizeof(ssl_cert_arg), "--ssl-cert=%s", cert_path);
    snprintf(ssl_key_arg, sizeof(ssl_key_arg), "--ssl-key=%s", key_path);
    snprintf(datadir_arg, sizeof(datadir_arg), "--datadir=%s", data_dir);
    if (config.mysql_port > 0) {
        snprintf(mysql_port_arg, sizeof(mysql_port_arg), "--port=%d", config.mysql_port);
    }
    
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
    
    /* Add --port if custom MySQL port is specified */
    if (config.mysql_port > 0 && mysql_port_arg[0] != '\0') {
        new_argv[idx++] = mysql_port_arg;
    }
    
    new_argv[idx] = NULL;
    
    /* Set GCS_DEBUG_TRACE_PATH if configured */
    /* This allows the GR plugin to write debug traces to a readable location */
    if (config.gcs_debug_trace_path && strlen(config.gcs_debug_trace_path) > 0) {
        printf("[Launcher] Setting GCS_DEBUG_TRACE_PATH=%s\n", config.gcs_debug_trace_path);
        set_env("GCS_DEBUG_TRACE_PATH", config.gcs_debug_trace_path, 1);
    }
    
    /* Set LD_LIBRARY_PATH to use custom OpenSSL library for mysqld */
    /* This ensures mysqld uses our compiled OpenSSL instead of system OpenSSL */
    /* The custom OpenSSL is installed at /opt/openssl-install/lib64 */
    const char *openssl_lib_path = "/opt/openssl-install/lib64";
    const char *current_ld_path = getenv("LD_LIBRARY_PATH");
    if (current_ld_path && strlen(current_ld_path) > 0) {
        /* Prepend custom OpenSSL path to existing LD_LIBRARY_PATH */
        char new_ld_path[4096];
        snprintf(new_ld_path, sizeof(new_ld_path), "%s:%s", openssl_lib_path, current_ld_path);
        set_env("LD_LIBRARY_PATH", new_ld_path, 1);
    } else {
        set_env("LD_LIBRARY_PATH", openssl_lib_path, 1);
    }
    
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
    if (config.mysql_port > 0) {
        printf("[Launcher]   MySQL port: %d\n", config.mysql_port);
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
