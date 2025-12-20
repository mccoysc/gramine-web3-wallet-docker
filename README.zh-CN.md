# Gramine Web3 钱包 Docker 镜像

[English](README.md) | **中文文档**

这个仓库专门用于构建和发布基于 Gramine 项目的 Web3 钱包 Docker 镜像。镜像会自动构建并推送到 GitHub Container Registry (GHCR)。

## 功能特性

- ✅ 基于 [mccoysc/gramine](https://github.com/mccoysc/gramine) 构建，启用 DCAP 模式
- ✅ 支持 Intel SGX 可信执行环境
- ✅ 预装 Node.js (最新 LTS ≤v22.x，当前 v22.21.1)、OpenSSL (最新稳定版，当前 3.6.0) 和常用 Web3 开发工具
- ✅ 智能构建系统，通过 GitHub Releases 缓存避免不必要的重新编译
- ✅ 自动检测 Gramine、OpenSSL 和 Node.js 的最新版本
- ✅ 多任务工作流架构，高效利用资源
- ✅ 自动检测 Dockerfile 变化并触发镜像重新构建
- ✅ 自动构建并推送镜像到 GitHub Container Registry
- ✅ 支持多种标签策略（latest, 版本号, SHA 等）

## 架构说明

### 自动化工作流

本仓库使用智能多任务工作流系统，通过 GitHub Releases 缓存优化构建时间和资源使用。

#### 主工作流：构建和推送 (`.github/workflows/build-and-push.yml`)

该工作流实现了复杂的构建流水线，包含以下任务：

**1. 版本检测 (`check-versions`)**
   - 自动检测最新版本：
     - **Gramine**: mccoysc/gramine 仓库的最新提交 SHA
     - **OpenSSL**: 最新稳定版本（语义版本解析，排除预发布版本）
     - **Node.js**: 与 GCC 11 兼容的最新 LTS 版本（v22.x，排除需要 GCC 13+ 的 v24+）
   - 检查 GitHub Releases 中是否存在预编译二进制文件
   - 检测 Dockerfile 和相关文件的变化（Dockerfile、scripts/、config/、工作流文件）
   - 根据以下条件判断哪些组件需要重新构建：
     - 版本变化
     - GitHub Releases 中缺少预编译二进制文件
     - Dockerfile 修改
   - 触发条件：push、pull_request、workflow_dispatch、schedule（每 6 小时）

**2. 组件编译任务**

每个组件在独立的 GitHub Actions 环境中构建，避免资源耗尽：

- **`build-gramine`**: 编译支持 DCAP 的 Gramine
  - 仅在 Gramine 版本变化且 Releases 中不存在预编译版本时运行
  - 上传编译后的二进制文件到 GitHub Releases（标签：`prebuilt-gramine-{SHA8}`）
  - 文件大小：约 10MB

- **`build-openssl`**: 从源码编译 OpenSSL
  - 仅在 OpenSSL 版本变化且 Releases 中不存在预编译版本时运行
  - 上传编译后的二进制文件到 GitHub Releases（标签：`prebuilt-openssl-{VERSION}`）
  - 文件大小：约 8MB
  - 与 `build-gramine` 并行运行

- **`build-node`**: 编译使用自定义 OpenSSL 的 Node.js
  - 仅在 Node.js 版本变化且 Releases 中不存在预编译版本时运行
  - 依赖 `build-openssl`（使用预编译的 OpenSSL 进行链接）
  - 上传编译后的二进制文件到 GitHub Releases（标签：`prebuilt-{NODE_VERSION}`）
  - 文件大小：约 45MB

**3. Docker 镜像构建 (`build-image`)**
   - 触发条件：
     - Dockerfile 或相关文件变化，或
     - 任何依赖项（Gramine/OpenSSL/Node.js）被重新构建
   - 从 GitHub Releases 下载预编译二进制文件
   - 如果预编译文件不可用，则回退到源码编译
   - 推送到 GitHub Container Registry，生成多个标签
   - 仅在所有依赖任务完成（成功或跳过）后运行

**4. 版本更新 (`update-versions`)**
   - 更新 prebuilt/ 目录中的 VERSION 文件
   - 提交变更到仓库
   - 仅在推送到 main/master 时运行（PR 中不运行）

#### 旧版工作流：监控 Gramine (`.github/workflows/monitor-gramine.yml`)

**注意**: 该工作流已被弃用，功能已集成到 `build-and-push.yml` 的版本检测中。

   - 触发条件：
     - 每 6 小时自动检查一次
     - 手动触发
   - 自动操作：
     - 检查 mccoysc/gramine 仓库是否有更新
     - 如果有更新，自动触发镜像重新构建
     - 更新 `.gramine-version` 文件记录版本

### 关键优化

1. **GitHub Releases 缓存**: 预编译二进制文件存储在 GitHub Releases 中，避免跨工作流运行的不必要重新编译
2. **智能跳过逻辑**: 仅在版本变化且预编译文件不存在时才重新构建组件
3. **独立环境**: 每个编译任务在独立的 GitHub Actions 运行器中运行，防止资源耗尽
4. **并行执行**: OpenSSL 和 Gramine 并行编译；Node.js 等待 OpenSSL 完成
5. **Dockerfile 变化检测**: 即使依赖项未变化，Dockerfile 变化也会自动触发镜像重新构建
6. **GCC 兼容性**: Node.js 版本限制为 v22.x，以兼容 Ubuntu 22.04 的 GCC 11

## 使用方法

### 拉取镜像

```bash
# 拉取最新版本
docker pull ghcr.io/mccoysc/gramine-web3-wallet-docker:latest

# 拉取特定版本
docker pull ghcr.io/mccoysc/gramine-web3-wallet-docker:v1.0.0
```

### 运行容器

#### 使用 Docker 命令

```bash
# 基本运行
docker run -it ghcr.io/mccoysc/gramine-web3-wallet-docker:latest

# 启用 SGX 支持（需要 SGX 硬件）
docker run -it \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -v $(pwd)/wallet:/app/wallet \
  -v $(pwd)/keys:/app/keys \
  -p 8545:8545 \
  ghcr.io/mccoysc/gramine-web3-wallet-docker:latest
```

#### 使用 Docker Compose

```bash
# 启动服务
docker-compose up -d

# 查看日志
docker-compose logs -f

# 进入容器
docker-compose exec gramine-web3-wallet bash

# 停止服务
docker-compose down
```

### 本地构建

```bash
# 克隆仓库
git clone https://github.com/mccoysc/gramine-web3-wallet-docker.git
cd gramine-web3-wallet-docker

# 构建镜像
docker build -t gramine-web3-wallet:local .

# 运行本地构建的镜像
docker run -it gramine-web3-wallet:local
```

## 目录结构

```
gramine-web3-wallet-docker/
├── .github/
│   └── workflows/
│       ├── build-and-push.yml      # 主构建工作流，带智能缓存
│       └── monitor-gramine.yml     # 旧版监控工作流（已弃用）
├── config/
│   └── pccs-default.json          # PCCS 默认配置
├── scripts/
│   └── entrypoint.sh              # 容器启动脚本
├── prebuilt/                       # 预编译二进制文件元数据（由工作流创建）
│   ├── gramine/
│   │   └── VERSION                # 当前 Gramine SHA（由 update-versions 任务维护）
│   ├── openssl/
│   │   └── VERSION                # 当前 OpenSSL 版本（由 update-versions 任务维护）
│   └── nodejs/
│       └── VERSION                # 当前 Node.js 版本（由 update-versions 任务维护）
├── Dockerfile                      # Docker 镜像定义
├── docker-compose.yml             # Docker Compose 配置
├── .gramine-version               # 记录当前使用的 Gramine 版本（旧版）
└── README.md                      # 本文档
```

## 环境变量

### 运行时环境变量

| 变量名 | 说明 | 默认值 | 是否必需 |
|--------|------|--------|----------|
| `GRAMINE_SGX_MODE` | 是否启用 SGX 模式 | `1` | 否 |
| `GRAMINE_DIRECT_MODE` | 是否启用直接模式 | `0` | 否 |
| `NODE_ENV` | Node.js 环境 | `production` | 否 |
| `PCCS_API_KEY` | 用于 DCAP 认证的 Intel PCCS API 密钥 | 空 | DCAP 推荐 |

### RA-TLS 配置

本 Docker 镜像包含用于远程认证的 Gramine RA-TLS 库。要在应用程序中启用 RA-TLS，请使用 Gramine manifest 处理提供的自动注入功能。

**推荐：通过 GRAMINE_LD_PRELOAD 自动注入**

Gramine manifest 处理器支持自动 RA-TLS 库注入。在运行 `gramine-manifest` 之前设置 `GRAMINE_LD_PRELOAD` 环境变量：

```bash
# 查找库路径（常见位置）
# /usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so
# /usr/local/lib/libratls-quote-verify.so
# 或使用：ldconfig -p | grep libratls-quote-verify.so

export GRAMINE_LD_PRELOAD="file:/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so"
gramine-manifest my-app.manifest.template my-app.manifest
```

这会自动：
- 将库添加到 `sgx.trusted_files`
- 将 `loader.env.LD_PRELOAD` 设置为库路径
- 设置 `loader.env.RA_TLS_ENABLE_VERIFY=1`
- 创建必要的 `fs.mounts` 条目

**手动配置**

或者，在 manifest 模板中手动配置 LD_PRELOAD：

```toml
sgx.remote_attestation = "dcap"

loader.env.LD_PRELOAD = "/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so"
loader.env.RA_TLS_ENABLE_VERIFY = "1"

sgx.trusted_files = [
    "file:/usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so",
]
```

**RA-TLS 环境变量**

有关 RA-TLS 环境变量的完整文档（包括 `RA_TLS_ENABLE_VERIFY`、`RA_TLS_REQUIRE_PEER_CERT`、`RA_TLS_MRSIGNER`、`RA_TLS_MRENCLAVE` 等），请参阅 [Gramine RA-TLS 文档](https://github.com/mccoysc/gramine#ra-tls-quick-start)。

**历史遗留说明**：本仓库以前包含用于自动注入的 `ratls_inject.py` 和 `patch-gramine-manifest.sh` 脚本。这些脚本现已禁用（在 Dockerfile 中注释掉），并将在未来版本中删除。环境变量 `DISABLE_RATLS_PRELOAD` 和 `RA_TLS_PRELOAD_PATH` 已弃用且不再使用。请改用 `GRAMINE_LD_PRELOAD`。

### API 密钥配置

#### PCCS API 密钥

PCCS (Provisioning Certificate Caching Service) 用于 SGX DCAP 远程认证。如果需要从 Intel 获取最新的 SGX 证书，需要配置 API 密钥。

**获取 API 密钥**：
1. 访问 [Intel PCS Portal](https://api.portal.trustedservices.intel.com/)
2. 注册并创建订阅
3. 获取 API 密钥（Primary Key 或 Secondary Key）

**配置方法**：

使用 Docker 命令行：
```bash
docker run -it \
  -e PCCS_API_KEY="your-api-key-here" \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  ghcr.io/mccoysc/gramine-web3-wallet-docker:latest
```

使用 Docker Compose，在 `docker-compose.yml` 中添加：
```yaml
services:
  gramine-web3-wallet:
    image: ghcr.io/mccoysc/gramine-web3-wallet-docker:latest
    environment:
      - PCCS_API_KEY=${PCCS_API_KEY}
    # ... 其他配置
```

然后创建 `.env` 文件：
```bash
PCCS_API_KEY=your-api-key-here
```

**注意事项**：
- API 密钥应保密，不要提交到代码仓库
- 如果不配置 API 密钥，PCCS 仍会启动，但无法从 Intel 获取最新证书
- 对于开发和测试环境，可以不配置 API 密钥

### SGX 服务配置

Docker 镜像包含三个用于 DCAP 远程认证的关键 SGX 服务：

#### 1. aesmd (架构飞地服务管理器)

**用途**：管理认证所需的 SGX 架构飞地（Quoting Enclave、Provisioning Enclave 等）。

**安装详情**：
- 软件包：`sgx-aesm-service`（不是 `libsgx-aesm-service`）
- 插件：`libsgx-aesm-launch-plugin`、`libsgx-aesm-pce-plugin`、`libsgx-aesm-epid-plugin`、`libsgx-aesm-quote-ex-plugin`、`libsgx-aesm-ecdsa-plugin`
- 二进制文件位置：`/opt/intel/sgx-aesm-service/aesm/aesm_service`
- Socket：`/var/run/aesmd/aesm.socket`

**启动方式**：entrypoint 脚本在后台自动启动 aesmd，无需 systemd：
```bash
LD_LIBRARY_PATH=/opt/intel/sgx-aesm-service/aesm /opt/intel/sgx-aesm-service/aesm/aesm_service &
```

**验证**：检查 aesmd 是否运行：
```bash
# 在容器内
test -S /var/run/aesmd/aesm.socket && echo "aesmd 正在运行" || echo "aesmd 未运行"
```

#### 2. PCCS (配置证书缓存服务)

**用途**：缓存来自 Intel PCS（配置证书服务）的 SGX 配置证书，以减少网络延迟并提供离线认证支持。

**安装详情**：
- 软件包：`sgx-dcap-pccs`
- 安装路径：`/opt/intel/sgx-dcap-pccs`
- 配置文件：`/opt/intel/sgx-dcap-pccs/config/default.json`
- 端口：HTTP 8080、HTTPS 8081

**启动方式**：当设置 `PCCS_API_KEY` 时，entrypoint 脚本会条件性启动：
```bash
# 仅在提供 PCCS_API_KEY 环境变量时启动
if [ -n "$PCCS_API_KEY" ]; then
    cd /opt/intel/sgx-dcap-pccs
    node pccs_server.js &
fi
```

**配置**：entrypoint 脚本会自动将 `PCCS_API_KEY` 环境变量中的 API 密钥注入到 PCCS 配置文件中。

#### 3. QPL (Quote Provider Library) 配置

**用途**：配置 Gramine 应用程序如何获取认证材料（证书、CRL、TCB 信息）。

**配置文件**：`/etc/sgx_default_qcnl.conf`

**行为**：
- **当设置 PCCS_API_KEY 时**：QPL 配置为使用本地 PCCS 实例：
  ```json
  {
    "pccs_url": "https://127.0.0.1:8081/sgx/certification/v4/",
    "use_secure_cert": false,
    "collateral_service": "https://api.trustedservices.intel.com/sgx/certification/v4/",
    "retry_times": 6,
    "retry_delay": 10
  }
  ```
  这允许 aesmd 从本地 PCCS 获取认证材料，而 PCCS 则使用 API 密钥从 Intel PCS 获取。

- **当未设置 PCCS_API_KEY 时**：QPL 配置为直接使用 Intel PCS：
  ```json
  {
    "pccs_url": "https://api.trustedservices.intel.com/sgx/certification/v4/",
    "use_secure_cert": true,
    "collateral_service": "https://api.trustedservices.intel.com/sgx/certification/v4/"
  }
  ```
  这需要在认证期间直接访问 Intel 服务器的网络连接。

**注意**：aesmd 服务不直接使用 PCCS_API_KEY。相反，aesmd 使用 QPL 连接到 PCCS，而 PCCS 使用 API 密钥从 Intel PCS 获取证书。

## 预装工具

镜像中预装了以下工具：

- **Gramine LibOS**: 从 mccoysc/gramine 构建，启用 DCAP 模式
  - 自动检测并编译最新版本
  - 安装路径：`/opt/gramine-install`
- **OpenSSL**: 最新稳定版（当前 3.6.0）
  - 预编译：从源码编译，带优化
  - 回退：使用 Ubuntu 22.04 系统 OpenSSL
  - 安装路径：`/opt/openssl-install`（预编译路径）
  - 库路径：`/opt/openssl-install/lib64`（x86_64）
- **Node.js**: 最新 LTS，限制为 v22.x 以兼容 GCC 11（当前 v22.21.1）
  - 预编译：使用自定义 OpenSSL 3.6.0 编译
  - 回退：从 NodeSource 仓库安装（v22.x）
  - 兼容 Ubuntu 22.04 GCC 11
  - 安装路径：`/opt/node-install`（预编译路径）
  - 注意：v24+ LTS 需要 GCC 13+，因此被兼容性限制排除
- **Python**: 3.10.x
- **SGX 服务**:
  - aesmd (SGX Architectural Enclave Service Manager)
  - PCCS (Provisioning Certificate Caching Service)
  - QPL (Quote Provider Library)
- **Web3 工具**（通过 npm 全局安装，由 `INSTALL_WEB3_TOOLS=true` 构建参数控制）:
  - web3.js
  - ethers.js
  - Hardhat
  - Truffle
  - Ganache

## 开发指南

### 修改 Dockerfile

1. 编辑 `Dockerfile` 文件
2. 提交更改到仓库
3. GitHub Actions 会自动构建新镜像

### 手动触发构建

1. 进入仓库的 Actions 页面
2. 选择 "Build and Push Docker Image" 工作流
3. 点击 "Run workflow"
4. 选择分支并运行

### 强制重新构建

如果需要强制重新构建所有组件（即使版本没有变化）：

1. 进入仓库的 Actions 页面
2. 选择 "Build and Push Docker Image" 工作流
3. 点击 "Run workflow"
4. 勾选 "Force rebuild all components" 选项
5. 运行工作流

这将从源码重新编译 Gramine、OpenSSL 和 Node.js，并重新构建 Docker 镜像，无论 GitHub Releases 中是否存在预编译二进制文件。

## 镜像标签策略

GitHub Actions 会自动生成以下标签：

- `latest`: 最新的 main/master 分支构建
- `v1.0.0`: 版本标签（如果推送了 git tag）
- `v1.0`: 主版本号和次版本号
- `v1`: 主版本号
- `main-abc1234`: 分支名-提交 SHA
- `pr-123`: Pull Request 编号

## 安全注意事项

1. **SGX 设备访问**: 容器需要访问 `/dev/sgx_enclave` 和 `/dev/sgx_provision` 设备
2. **密钥管理**: 建议将私钥存储在加密卷中，不要提交到仓库
3. **网络安全**: 生产环境中请配置适当的防火墙规则
4. **权限控制**: 使用最小权限原则运行容器

## 故障排除

### SGX 设备未找到

如果看到 "SGX device not found" 警告：

1. 确认主机支持 Intel SGX
2. 确认 SGX 驱动已安装
3. 确认 Docker 运行时有权限访问 SGX 设备

### 构建失败

如果 GitHub Actions 构建失败：

1. 查看 Actions 日志，找出具体失败的任务
2. 检查 Dockerfile 语法
3. 确认依赖项可访问
4. 检查 GitHub Releases 下载是否失败（网络问题）
5. 对于编译失败，检查 GCC 版本兼容性

常见问题：
- **OpenSSL 下载失败**: 检查 Release `prebuilt-openssl-{VERSION}` 是否存在，资源名称为 `openssl-install-openssl-{VERSION}.tar.gz`（例如：`prebuilt-openssl-3.6.0` 和 `openssl-install-openssl-3.6.0.tar.gz`）
- **Node.js 编译失败**: 确认 OpenSSL 已成功构建并可用
- **Gramine 编译失败**: 检查 mccoysc/gramine 仓库是否可访问
- **VERSION 文件缺失**: 这些文件由 update-versions 任务在首次成功运行 main 分支后创建

### 镜像拉取失败

如果无法拉取镜像：

1. 确认已登录 GitHub Container Registry：
   ```bash
   echo $GITHUB_TOKEN | docker login ghcr.io -u USERNAME --password-stdin
   ```
2. 确认镜像仓库权限设置正确

### OpenSSL 库未找到

如果在容器中看到 "OpenSSL version not found" 错误：

1. 确认 `LD_LIBRARY_PATH` 包含 `/opt/openssl-install/lib64`
2. 检查 OpenSSL 是否正确解压：`ls -la /opt/openssl-install`
3. 手动测试 OpenSSL：`LD_LIBRARY_PATH=/opt/openssl-install/lib64 /opt/openssl-install/bin/openssl version`

## 贡献指南

欢迎提交 Issue 和 Pull Request！

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/amazing-feature`)
3. 提交更改 (`git commit -m 'Add some amazing feature'`)
4. 推送到分支 (`git push origin feature/amazing-feature`)
5. 创建 Pull Request

## 许可证

本项目采用 LGPL-3.0 许可证，与 Gramine 项目保持一致。

## 相关链接

- [Gramine 官方文档](https://gramine.readthedocs.io/)
- [mccoysc/gramine 仓库](https://github.com/mccoysc/gramine)
- [Intel SGX 文档](https://www.intel.com/content/www/us/en/developer/tools/software-guard-extensions/overview.html)
- [GitHub Container Registry 文档](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry)

## 联系方式

- GitHub: [@mccoysc](https://github.com/mccoysc)

---

**注意**: 本镜像仅用于开发和测试目的。生产环境使用前请进行充分的安全审计。
