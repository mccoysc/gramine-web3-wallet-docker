# MySQL 8 RA-TLS 支持

[English](README.md)

本示例提供了一个支持透明 RA-TLS（远程证明 TLS）的 MySQL 8 服务器镜像，用于基于 SGX 的双向认证。

## 自包含 Dockerfile

本示例使用**自包含 Dockerfile** - 无需外部文件。所有源代码（C 启动器、Gramine manifest 模板、构建脚本）都使用 heredoc 语法直接嵌入到 Dockerfile 中。只需运行 `docker build` 即可创建镜像。

## 功能特性

- **双向 RA-TLS 认证**：客户端和服务器都必须在 SGX enclave 中运行
- **仅证书认证**：MySQL 使用 X.509 证书进行认证（无密码）
- **基于 SGX Quote 的证书**：证书在启动时从 SGX quote 生成
- **标准 TLS 证书**：使用 secp256r1 曲线生成证书（基础设施广泛支持）
- **智能合约白名单**：可选的从以太坊智能合约读取白名单配置
- **预编译 Manifest**：Gramine manifest 在 Docker 构建期间预编译和签名
- **加密数据存储**：MySQL 数据和私钥存储在加密分区（日志为便于调试不加密）
- **Group Replication 支持**：多节点互为主从模式，支持自动 IP 检测
- **默认启用 GR 模式**：Group Replication 默认启用，无需任何参数
- **自动生成 Group Name**：首次运行时自动生成 UUID 并持久化以供重用

## 工作原理

1. 容器使用 `gramine-web3-wallet-docker` 基础镜像启动，该镜像提供：
   - Intel SGX 运行时（aesmd 服务）
   - 用于 DCAP 证明的 PCCS
   - RA-TLS 库（`libratls-quote-verify.so`）

2. C 启动器程序在 SGX enclave 内运行：
   - 从智能合约读取白名单配置（如果设置了 CONTRACT_ADDRESS）
   - 设置 RA-TLS 环境变量（secp256r1 曲线，启用验证）
   - 在 execve() 前设置 `LD_PRELOAD` 注入 RA-TLS 库
   - 生成或加载 Group Replication 配置
   - 使用 `execve()` 替换自身为 mysqld（避免在 enclave 中创建子进程的开销）

3. RA-TLS 通过启动器设置的 `LD_PRELOAD` 注入（不是 manifest）：
   - 启动器在 execve() 前动态设置 LD_PRELOAD
   - 这确保只有 mysqld 获得 RA-TLS 库，启动器本身不会
   - 透明拦截 MySQL 连接的 TLS
   - 自动生成和验证 SGX quote

4. 当客户端连接时：
   - TLS 握手包含 SGX 证明（RA-TLS）
   - 双方验证对方的 SGX quote
   - 如果配置了白名单，检查客户端测量值
   - MySQL 使用客户端的 X.509 证书进行认证

## 构建

```bash
docker build -t mysql-ratls .
```

## 运行

### 基本用法（默认启用 GR）

```bash
# 直接运行 - GR 默认启用，自动生成 group name
docker run -d \
  --name mysql-ratls \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -p 3306:3306 \
  -p 33061:33061 \
  mysql-ratls
```

启动器将：
1. 首次运行时自动生成 UUID 作为 group name
2. 将 UUID 持久化到加密分区以供重用
3. 将明文副本写入 `/var/lib/mysql/gr_group_name.txt` 供运维查看

### 使用智能合约白名单

```bash
docker run -d \
  --name mysql-ratls \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e CONTRACT_ADDRESS=0x1234567890abcdef... \
  -e RPC_URL=https://eth-mainnet.example.com \
  -p 3306:3306 \
  -p 33061:33061 \
  mysql-ratls
```

### 使用手动白名单

**注意**：`RATLS_WHITELIST_CONFIG` 只能通过 Gramine manifest 环境变量设置，不能通过 `docker run -e` 设置。这是因为 manifest 是签名和可信的，而命令行参数不是。

要使用手动白名单，必须在构建前修改 manifest 模板：

```toml
# 在 mysql-ratls.manifest.template 中添加：
loader.env.RATLS_WHITELIST_CONFIG = "BASE64_ENCODED_CSV_HERE"
```

白名单格式是 Base64 编码的 CSV，共 5 行：
```
# 第 1 行：MRENCLAVE 值（逗号分隔的十六进制）
# 第 2 行：MRSIGNER 值（逗号分隔的十六进制）
# 第 3 行：ISV_PROD_ID 值
# 第 4 行：ISV_SVN 值
# 第 5 行：PLATFORM_INSTANCE_ID 值
```

如果同时配置了合约白名单（通过 RPC URL）和 manifest 白名单，它们会按规则去重后合并。

## 环境变量

| 变量 | 必需 | 描述 |
|------|------|------|
| `PCCS_API_KEY` | 是 | 用于 DCAP 证明的 Intel PCCS API 密钥 |
| `CONTRACT_ADDRESS` | 否 | 用于读取白名单的以太坊合约地址 |
| `RPC_URL` | 否* | 以太坊 RPC 端点（*如果设置了 CONTRACT_ADDRESS 则必需） |
| `RATLS_WHITELIST_CONFIG` | 否 | 手动白名单（Base64 编码的 CSV）。只能通过 manifest 环境变量设置，不能通过命令行设置。如果同时设置了合约白名单和环境变量白名单，它们会按规则去重后合并。 |
| `MYSQL_GR_GROUP_NAME` | 否 | Group Replication group name（UUID）。未设置时自动生成。 |
| `RATLS_CERT_PATH` | 否 | RA-TLS 证书路径（默认：`/var/lib/mysql-ssl/server-cert.pem`） |
| `RATLS_KEY_PATH` | 否 | RA-TLS 私钥路径，必须在加密分区（默认：`/app/wallet/mysql-keys/server-key.pem`） |

## 智能合约集成

启动器脚本从实现以下接口的智能合约读取白名单配置：

```solidity
function getSGXConfig() external view returns (string memory);
```

返回的字符串应该是包含 `RATLS_WHITELIST_CONFIG` 字段的 JSON 对象：

```json
{
  "RATLS_WHITELIST_CONFIG": "BASE64_ENCODED_CSV_WHITELIST",
  "other_config": "..."
}
```

## 白名单格式

`RATLS_WHITELIST_CONFIG` 是 Base64 编码的 CSV，正好 5 行：

1. **MRENCLAVE**：允许的 enclave 测量值（逗号分隔的十六进制）
2. **MRSIGNER**：允许的签名者测量值（逗号分隔的十六进制）
3. **ISV_PROD_ID**：产品 ID
4. **ISV_SVN**：安全版本号
5. **PLATFORM_INSTANCE_ID**：平台实例 ID

空行或 "0" 值作为通配符（允许任何值）。

## 客户端要求

连接到此 MySQL 服务器的客户端必须：

1. 在支持 RA-TLS 的 SGX enclave 中运行
2. 通过 LD_PRELOAD 使用 `libratls-quote-verify.so`
3. 在 TLS 握手期间提供有效的 RA-TLS 证书
4. 匹配白名单配置（如果启用）

## 安全注意事项

- **无密码认证**：此镜像配置为仅证书认证
- **双向证明**：客户端和服务器相互验证对方的 SGX quote
- **证书重新生成**：每次容器启动时重新生成证书
- **白名单强制执行**：配置后，只有匹配白名单的客户端才能连接

## 故障排除

### 找不到 RA-TLS 库

如果看到 "libratls-quote-verify.so not found"，请确保基础镜像正确构建了 Gramine 和 RA-TLS 支持。

### 合约读取失败

如果从合约读取白名单失败：
- 验证 `CONTRACT_ADDRESS` 是否正确
- 验证 `RPC_URL` 是否可访问
- 检查合约是否实现了 `getSGXConfig()`
- 确保返回的 JSON 包含 `RATLS_WHITELIST_CONFIG` 字段

### SGX 设备不可用

如果 SGX 设备不可用：
- 确保主机在 BIOS 中启用了 SGX
- 使用 `--device=/dev/sgx_enclave --device=/dev/sgx_provision` 运行容器
- 检查主机是否安装了 SGX 驱动程序

## Group Replication

本镜像支持 MySQL 8 Group Replication，用于多节点部署的互为主从模式。

### 默认行为（默认启用 GR）

**Group Replication 默认启用** - 无需任何参数：

```bash
# 第一个节点 - 自动生成并持久化 group name
docker run -d --name mysql-gr-node1 \
  --device=/dev/sgx_enclave --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -p 3306:3306 -p 33061:33061 \
  mysql-ratls --gr-bootstrap
```

Group name：
- 首次运行时**自动生成** UUID v4
- **持久化**到 `/app/wallet/.mysql_gr_group_name`（加密分区）
- **明文副本**写入 `/var/lib/mysql/gr_group_name.txt`（供运维查看）

### Group Name 优先级链

Group name 按以下顺序解析：
1. `--gr-group-name` 命令行参数（最高优先级）
2. `MYSQL_GR_GROUP_NAME` 环境变量
3. 持久化文件 `/app/wallet/.mysql_gr_group_name`
4. 自动生成新 UUID（仅首次运行）

### Group Replication 工作原理

1. **自动 IP 检测**：启动器自动检测：
   - 本地局域网 IP 地址（通过 UDP socket 技巧）
   - 公网 IP 地址（通过 https://ifconfig.me/ip）

2. **种子节点配置**：种子列表由以下组成：
   - 自身 IP（局域网 + 公网，按 ip:port 对去重）
   - 通过 `--gr-seeds` 参数指定的额外种子

3. **用于复制的 RA-TLS**：`app` 用户用于复制，使用 X509 证书认证（与客户端连接相同的 RA-TLS 证书）。

### 启动 Group Replication 集群

#### 引导第一个节点

```bash
docker run -d \
  --name mysql-gr-node1 \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -p 3306:3306 \
  -p 33061:33061 \
  mysql-ratls \
  --gr-bootstrap
```

启动后，获取自动生成的 group name：
```bash
docker exec mysql-gr-node1 cat /var/lib/mysql/gr_group_name.txt
```

#### 加入其他节点

```bash
# 使用第一个节点的 group name
docker run -d \
  --name mysql-gr-node2 \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e MYSQL_GR_GROUP_NAME=<uuid-from-node1> \
  -p 3307:3306 \
  -p 33062:33061 \
  mysql-ratls \
  --gr-seeds=192.168.1.100:33061
```

### Group Replication 参数

| 参数 | 描述 |
|------|------|
| `--gr-group-name=UUID` | Group Replication group name（UUID 格式）。未指定时自动生成。 |
| `--gr-bootstrap` | 引导新的复制组（仅第一个节点）。 |
| `--gr-seeds=SEEDS` | 逗号分隔的种子节点列表（格式：`host1:port,host2:port`）。未指定端口时默认为 33061。 |
| `--gr-local-address=ADDR` | 覆盖 GR 通信的本地地址（默认：自动检测局域网 IP:33061）。 |

### 验证 GR 配置

MySQL 启动后，验证配置：

```sql
-- 检查 GR 插件是否已加载
SELECT plugin_name, plugin_status FROM information_schema.plugins 
WHERE plugin_name = 'group_replication';

-- 查看所有 GR 变量
SHOW VARIABLES LIKE 'group_replication%';

-- 检查关键设置
SHOW VARIABLES WHERE Variable_name IN (
  'gtid_mode',
  'enforce_gtid_consistency',
  'log_bin',
  'binlog_format',
  'server_id'
);

-- 检查 GR 成员状态
SELECT * FROM performance_schema.replication_group_members;
```

### 运行时配置更改

部分 GR 设置可以在运行时修改（需要先停止 GR）：

```sql
-- 停止 GR
STOP GROUP_REPLICATION;

-- 修改种子
SET GLOBAL group_replication_group_seeds = '192.168.1.100:33061,192.168.1.101:33061';

-- 修改本地地址
SET GLOBAL group_replication_local_address = '192.168.1.100:33061';

-- 修改 group name（加入不同的组！）
SET GLOBAL group_replication_group_name = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee';

-- 重新启动 GR
START GROUP_REPLICATION;
```

**需要重启 MySQL 的设置：**
- `gtid_mode`
- `enforce_gtid_consistency`
- `server_id`
- `plugin_load_add`

### Group Replication 注意事项

- **端口 33061**：用于 Group Replication XCom 通信。必须在节点之间暴露和可访问。
- **UUID 格式**：`--gr-group-name` 必须是有效的 UUID（例如 `aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee`）。
- **种子去重**：所有种子（自身 IP + 额外种子）按 ip:port 对去重。
- **证书认证**：复制使用与客户端连接相同的 RA-TLS 证书（需要 X509）。
- **多主模式**：所有节点都可以接受写入（互为主从模式）。
- **明文 Group Name**：`/var/lib/mysql/gr_group_name.txt` 包含 group name 供运维查看。

## 命令行参数

所有环境变量也可以通过命令行参数指定。参数优先于环境变量。

### 通用选项

| 参数 | 环境变量 | 描述 |
|------|----------|------|
| `--help`, `-h` | - | 显示帮助信息 |
| `--dry-run` | - | 测试模式：生成所有配置但不启动 mysqld |
| `--contract-address=ADDR` | `CONTRACT_ADDRESS` | 用于白名单的智能合约地址 |
| `--rpc-url=URL` | `RPC_URL` | 以太坊 JSON-RPC 端点 URL |

**注意**：`RATLS_WHITELIST_CONFIG` 只能通过 manifest 环境变量设置（不能通过命令行），这是出于安全考虑。如果同时设置了合约白名单和环境变量白名单，它们会按规则去重后合并（5 行中相同索引的值组成一条规则）。

### 路径选项

| 参数 | 环境变量 | 默认值 | 描述 |
|------|----------|--------|------|
| `--cert-path=PATH` | `RATLS_CERT_PATH` | `/var/lib/mysql-ssl/server-cert.pem` | RA-TLS 证书路径 |

**注意**：以下路径**只能**通过 manifest 环境变量设置（不能通过命令行），以防止数据泄漏：
- `RATLS_KEY_PATH`：RA-TLS 私钥路径（默认：`/app/wallet/mysql-keys/server-key.pem`）
- `MYSQL_DATA_DIR`：MySQL 数据目录（默认：`/app/wallet/mysql-data`）

### RA-TLS 配置选项

| 参数 | 环境变量 | 默认值 | 描述 |
|------|----------|--------|------|
| `--ra-tls-cert-algorithm=ALG` | `RA_TLS_CERT_ALGORITHM` | - | 证书算法（例如 secp256r1, secp256k1） |
| `--ratls-enable-verify=0\|1` | `RATLS_ENABLE_VERIFY` | `1` | 启用 RA-TLS 验证 |
| `--ratls-require-peer-cert=0\|1` | `RATLS_REQUIRE_PEER_CERT` | `1` | 要求对等证书以进行双向 TLS |
| `--ra-tls-allow-outdated-tcb=0\|1` | `RA_TLS_ALLOW_OUTDATED_TCB_INSECURE` | 来自 manifest | 允许过时的 TCB（不安全） |
| `--ra-tls-allow-hw-config-needed=0\|1` | `RA_TLS_ALLOW_HW_CONFIG_NEEDED` | 来自 manifest | 允许需要硬件配置状态 |
| `--ra-tls-allow-sw-hardening-needed=0\|1` | `RA_TLS_ALLOW_SW_HARDENING_NEEDED` | 来自 manifest | 允许需要软件加固状态 |

### 配置验证

启动器验证配置并处理依赖关系：

- `RATLS_WHITELIST_CONFIG` 只能通过 manifest 环境变量设置（不能通过命令行），这是出于安全考虑
- 如果同时设置了合约白名单和环境变量白名单，它们会按规则去重后合并
- Group Replication 默认启用；未指定 `--gr-group-name` 时自动生成
- 当缺少依赖配置时会打印警告（例如设置了 `--contract-address` 但没有 `--rpc-url`）

## 文件位置

| 路径 | 描述 | 加密 |
|------|------|------|
| `/app/wallet/mysql-data` | MySQL 数据目录 | 是 |
| `/app/wallet/mysql-keys/server-key.pem` | RA-TLS 私钥 | 是 |
| `/app/wallet/.mysql_gr_group_name` | 持久化的 GR group name | 是 |
| `/app/wallet/.mysql_server_id` | 持久化的 server ID | 是 |
| `/var/lib/mysql-ssl/server-cert.pem` | RA-TLS 证书 | 否 |
| `/var/lib/mysql/mysql-gr.cnf` | GR 配置文件 | 否 |
| `/var/lib/mysql/gr_group_name.txt` | 明文 group name（运维用） | 否 |
| `/var/log/mysql/error.log` | MySQL 错误日志 | 否 |

## 许可证

本示例与 gramine-web3-wallet-docker 项目使用相同的许可证。
