# Gramine Web3 钱包 Docker 镜像

这个仓库专门用于构建和发布基于 Gramine 项目的 Web3 钱包 Docker 镜像。镜像会自动构建并推送到 GitHub Container Registry (GHCR)。

## 功能特性

- ✅ 基于 [mccoysc/gramine](https://github.com/mccoysc/gramine) 构建
- ✅ 支持 Intel SGX 可信执行环境
- ✅ 预装 Node.js 和常用 Web3 开发工具
- ✅ 自动监控 Gramine 仓库更新并触发重新构建
- ✅ 自动构建并推送镜像到 GitHub Container Registry
- ✅ 支持多种标签策略（latest, 版本号, SHA 等）

## 架构说明

### 自动化工作流

1. **构建和推送镜像** (`.github/workflows/build-and-push.yml`)
   - 触发条件：
     - 推送到 main/master 分支
     - 创建版本标签（v*）
     - 手动触发
   - 自动操作：
     - 构建 Docker 镜像
     - 推送到 GitHub Container Registry
     - 生成多个标签（latest, 版本号, SHA 等）

2. **监控 Gramine 仓库** (`.github/workflows/monitor-gramine.yml`)
   - 触发条件：
     - 每 6 小时自动检查一次
     - 手动触发
   - 自动操作：
     - 检查 mccoysc/gramine 仓库是否有更新
     - 如果有更新，自动触发镜像重新构建
     - 更新 `.gramine-version` 文件记录版本

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
│       ├── build-and-push.yml      # 构建和推送镜像的工作流
│       └── monitor-gramine.yml     # 监控 Gramine 仓库的工作流
├── docker/                         # Docker 相关配置
├── scripts/
│   └── entrypoint.sh              # 容器启动脚本
├── Dockerfile                      # Docker 镜像定义
├── docker-compose.yml             # Docker Compose 配置
├── .gramine-version               # 记录当前使用的 Gramine 版本
└── README.md                      # 本文档
```

## 环境变量

| 变量名 | 说明 | 默认值 |
|--------|------|--------|
| `GRAMINE_SGX_MODE` | 是否启用 SGX 模式 | `1` |
| `GRAMINE_DIRECT_MODE` | 是否启用直接模式 | `0` |
| `NODE_ENV` | Node.js 环境 | `production` |

## 预装工具

镜像中预装了以下工具：

- **Gramine LibOS**: 从 mccoysc/gramine 构建
- **Node.js**: v20.x LTS
- **Web3 工具**:
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

如果需要强制重新构建镜像（即使 Gramine 仓库没有更新）：

1. 进入仓库的 Actions 页面
2. 选择 "Monitor Gramine Repository" 工作流
3. 点击 "Run workflow"
4. 勾选 "Force rebuild" 选项
5. 运行工作流

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

1. 查看 Actions 日志
2. 检查 Dockerfile 语法
3. 确认依赖项可访问

### 镜像拉取失败

如果无法拉取镜像：

1. 确认已登录 GitHub Container Registry：
   ```bash
   echo $GITHUB_TOKEN | docker login ghcr.io -u USERNAME --password-stdin
   ```
2. 确认镜像仓库权限设置正确

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
- Email: hbzgzr@gmail.com

---

**注意**: 本镜像仅用于开发和测试目的。生产环境使用前请进行充分的安全审计。
