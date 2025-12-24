# MySQL 客户端 RA-TLS 支持

本示例提供了一个支持透明 RA-TLS（远程证明 TLS）的 MySQL 客户端镜像，用于基于 SGX 的双向认证。它设计用于连接 `mysql-ratls` 服务器示例。

## 自包含 Dockerfile

本示例使用**自包含 Dockerfile** - 不需要外部文件。所有源代码（C 启动器、Node.js 客户端脚本、Gramine manifest 模板、构建脚本）都在 Docker 构建期间从仓库下载。只需运行 `docker build` 即可创建镜像。

## 功能特性

- **双向 RA-TLS 认证**：客户端和服务器都必须在 SGX enclave 中运行
- **仅证书认证**：MySQL 使用 X.509 证书进行认证（无密码）
- **SGX Quote 证书**：证书在启动时从 SGX quote 生成
- **以太坊兼容**：使用 secp256r1 曲线生成证书
- **智能合约白名单**：可选的以太坊智能合约白名单配置
- **预编译 Manifest**：Gramine manifest 在 Docker 构建期间预编译和签名
- **交互模式**：支持交互式 SQL 查询

## 工作原理

1. 容器以 `gramine-web3-wallet-docker` 基础镜像启动，提供：
   - Intel SGX 运行时（aesmd 服务）
   - DCAP 证明的 PCCS
   - RA-TLS 库（`libratls-quote-verify.so`）
   - Node.js 运行时

2. C 启动器程序在 SGX enclave 内运行：
   - 从智能合约读取白名单配置（如果设置了 CONTRACT_ADDRESS）
   - 设置 RA-TLS 环境变量
   - 设置 LD_PRELOAD 用于 RA-TLS 注入
   - 使用 `execve()` 替换自身为 Node.js（避免 enclave 内创建子进程的开销）

3. Node.js 运行 MySQL 客户端脚本，通过 LD_PRELOAD 注入 RA-TLS：
   - MySQL 连接的透明 TLS 拦截
   - 自动 SGX quote 生成和验证
   - 与 MySQL 服务器的证书认证

4. 连接服务器时：
   - TLS 握手包含 SGX 证明（RA-TLS）
   - 双方验证对方的 SGX quote
   - 如果配置了白名单，检查服务器测量值
   - MySQL 使用客户端的 X.509 证书进行认证

## 构建

```bash
docker build -t mysql-client-ratls .
```

## 运行

### 基本用法（连接 MySQL 服务器）

```bash
docker run -it \
  --name mysql-client-ratls \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e MYSQL_HOST=mysql-server-hostname \
  -e MYSQL_USER=app \
  -e MYSQL_INTERACTIVE=1 \
  mysql-client-ratls
```

### 执行单个查询

```bash
docker run --rm \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e MYSQL_HOST=mysql-server-hostname \
  -e MYSQL_USER=app \
  -e MYSQL_DATABASE=mydb \
  -e MYSQL_QUERY="SELECT * FROM users LIMIT 10" \
  mysql-client-ratls
```

### 使用智能合约白名单

```bash
docker run -it \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e CONTRACT_ADDRESS=0x1234567890abcdef... \
  -e RPC_URL=https://eth-mainnet.example.com \
  -e MYSQL_HOST=mysql-server-hostname \
  -e MYSQL_USER=app \
  -e MYSQL_INTERACTIVE=1 \
  mysql-client-ratls
```

### 连接 mysql-ratls 服务器（Docker 网络）

```bash
# 创建网络
docker network create sgx-network

# 启动 mysql-ratls 服务器
docker run -d \
  --name mysql-server \
  --network sgx-network \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  mysql-ratls

# 使用客户端连接
docker run -it \
  --name mysql-client \
  --network sgx-network \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e MYSQL_HOST=mysql-server \
  -e MYSQL_USER=app \
  -e MYSQL_INTERACTIVE=1 \
  mysql-client-ratls
```

## 环境变量

| 变量 | 必需 | 描述 |
|------|------|------|
| `PCCS_API_KEY` | 是 | Intel PCCS API 密钥用于 DCAP 证明 |
| `MYSQL_HOST` | 否 | MySQL 服务器主机名（默认：`localhost`） |
| `MYSQL_PORT` | 否 | MySQL 服务器端口（默认：`3306`） |
| `MYSQL_USER` | 否 | MySQL 用户名（默认：`app`） |
| `MYSQL_DATABASE` | 否 | 要连接的 MySQL 数据库 |
| `MYSQL_QUERY` | 否 | 要执行的 SQL 查询（执行后退出） |
| `MYSQL_INTERACTIVE` | 否 | 设置为 `1` 启用交互模式 |
| `CONTRACT_ADDRESS` | 否 | 读取白名单的以太坊合约地址 |
| `RPC_URL` | 否* | 以太坊 RPC 端点（*如果设置了 CONTRACT_ADDRESS 则必需） |

## RA-TLS 证书配置

以下 RA-TLS 环境变量由 C 启动器配置（遵循 [mccoysc/gramine tools/sgx/ra-tls/CERTIFICATE_CONFIGURATION.md](https://github.com/mccoysc/gramine/blob/main/tools/sgx/ra-tls/CERTIFICATE_CONFIGURATION.md)）：

| 变量 | 值 | 描述 |
|------|-----|------|
| `RA_TLS_CERT_ALGORITHM` | `secp256r1` | 证书生成的椭圆曲线 |
| `RA_TLS_ENABLE_VERIFY` | `1` | 在 TLS 握手期间启用 RA-TLS 验证 |
| `RA_TLS_REQUIRE_PEER_CERT` | `1` | 双向 TLS 认证需要对等证书 |
| `RA_TLS_CERT_PATH` | `/var/lib/mysql-client-ssl/client-cert.pem` | 存储生成证书的路径（普通目录） |
| `RA_TLS_KEY_PATH` | `/app/wallet/mysql-client-keys/client-key.pem` | 存储私钥的路径（加密分区） |

这些值在 manifest 和启动器中硬编码以确保安全（不从主机环境透传）。私钥存储在加密分区（`/app/wallet/`）中以防止数据泄露。

## 智能合约集成

启动器可以从实现以下接口的智能合约读取白名单配置：

```solidity
function getSGXConfig() external view returns (string memory);
```

返回的字符串应该是包含 `RA_TLS_WHITELIST_CONFIG` 字段的 JSON 对象：

```json
{
  "RA_TLS_WHITELIST_CONFIG": "BASE64_ENCODED_CSV_WHITELIST",
  "other_config": "..."
}
```

## 白名单格式

`RA_TLS_WHITELIST_CONFIG` 是 Base64 编码的 CSV，包含 5 行：

1. **MRENCLAVE**：允许的 enclave 测量值的逗号分隔十六进制值
2. **MRSIGNER**：允许的签名者测量值的逗号分隔十六进制值
3. **ISV_PROD_ID**：逗号分隔的产品 ID
4. **ISV_SVN**：逗号分隔的安全版本号
5. **PLATFORM_INSTANCE_ID**：逗号分隔的平台实例 ID

空行或 "0" 值作为通配符（允许任何值）。

## 服务器要求

此客户端连接的 MySQL 服务器必须：

1. 在支持 RA-TLS 的 SGX enclave 中运行（例如 `mysql-ratls` 示例）
2. 通过 LD_PRELOAD 使用 `libratls-quote-verify.so`
3. 在 TLS 握手期间提供有效的 RA-TLS 证书
4. 配置为仅证书认证（REQUIRE X509）

## 目录结构

应用程序使用具有不同安全级别的结构化目录布局：

```
/app/
├── code/                    # 代码目录（挂载在 /app/code）
│   ├── mysql-client.js      # 主客户端脚本（trusted_files - 完整性检查）
│   ├── package.json         # NPM 配置（trusted_files - 完整性检查）
│   ├── node_modules/        # NPM 依赖（trusted_files - 完整性检查）
│   └── data/                # 运行时数据的加密分区
│       └── (运行时文件)      # 代码生成的敏感数据（加密）
└── wallet/                  # 密钥的加密分区
    └── mysql-client-keys/   # 客户端私钥（加密）
```

### 文件保护级别

| 路径 | 保护方式 | 描述 |
|------|----------|------|
| `/app/code/mysql-client.js` | trusted_files | enclave 加载时完整性检查，不可修改 |
| `/app/code/package.json` | trusted_files | enclave 加载时完整性检查 |
| `/app/code/node_modules/` | trusted_files | 所有依赖完整性检查 |
| `/app/code/data/` | encrypted | 敏感运行时数据的加密分区 |
| `/app/wallet/` | encrypted | 私钥的加密分区 |
| `/var/lib/mysql-client-ssl/` | allowed_files | 证书的可写目录 |

### 加密分区

加密分区使用 `_sgx_mrenclave` 密钥进行 SGX 密封，这意味着：
- 数据使用从 enclave 测量值派生的密钥加密
- 只有相同的 enclave 代码才能解密数据
- 数据在容器重启后持久化（如果挂载了卷）
- 为敏感运行时数据提供机密性保护

## 安全考虑

- **无密码认证**：此客户端配置为仅证书认证
- **双向证明**：客户端和服务器相互验证 SGX quote
- **证书重新生成**：每次容器启动时重新生成证书
- **私钥保护**：私钥存储在加密分区（`/app/wallet/`）
- **代码完整性**：应用程序代码通过 trusted_files 保护（加载时完整性检查）
- **运行时数据保护**：敏感运行时数据存储在加密分区（`/app/code/data/`）
- **白名单强制**：配置后，只能连接匹配白名单的服务器

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│                    SGX Enclave                               │
│  ┌─────────────────┐      ┌─────────────────────────────┐   │
│  │  C 启动器       │      │  Node.js + mysql-client.js  │   │
│  │                 │      │                             │   │
│  │  1. 读取配置    │      │  3. 连接 MySQL              │   │
│  │  2. 设置 LD_PRELOAD ──► │  4. RA-TLS 握手            │   │
│  │     execve()    │      │  5. 执行查询                │   │
│  └─────────────────┘      └─────────────────────────────┘   │
│                                      │                       │
│                           ┌──────────▼──────────┐           │
│                           │ libratls-quote-     │           │
│                           │ verify.so           │           │
│                           │ (LD_PRELOAD)        │           │
│                           └─────────────────────┘           │
└─────────────────────────────────────────────────────────────┘
                               │
                               │ TLS + SGX Quote
                               ▼
                    ┌─────────────────────┐
                    │  MySQL 服务器       │
                    │  (mysql-ratls)      │
                    │  在 SGX Enclave 中  │
                    └─────────────────────┘
```

## 故障排除

### RA-TLS 库未找到

如果看到 "libratls-quote-verify.so not found"，请确保基础镜像正确构建了 Gramine 和 RA-TLS 支持。

### 连接被拒绝

- 验证 MySQL 服务器正在运行且可访问
- 检查 `MYSQL_HOST` 是否正确设置
- 确保两个容器在同一个 Docker 网络上

### 证书验证失败

- 客户端和服务器都必须在 SGX enclave 中运行
- 验证 PCCS 已正确配置有效的 API 密钥
- 检查白名单配置（如果使用）是否包含服务器的测量值

### SGX 设备不可用

如果 SGX 设备不可用：
- 确保主机在 BIOS 中启用了 SGX
- 使用 `--device=/dev/sgx_enclave --device=/dev/sgx_provision` 运行容器
- 检查主机上是否安装了 SGX 驱动程序

## 许可证

本示例与 gramine-web3-wallet-docker 项目使用相同的许可证。
