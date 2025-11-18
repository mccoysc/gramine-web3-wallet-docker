# 示例文件

本目录包含使用 Gramine Web3 钱包 Docker 镜像的示例文件。

## 文件说明

### web3-app.manifest.template

这是一个 Gramine manifest 模板文件，用于配置 Web3 应用程序在 SGX 环境中的运行参数。

**主要配置项：**
- 文件系统挂载（包括加密存储）
- SGX 参数（enclave 大小、线程数等）
- 可信文件列表
- 环境变量

**使用方法：**

```bash
# 1. 进入容器
docker exec -it gramine-web3-wallet bash

# 2. 处理 manifest 模板
gramine-manifest \
  -Dentrypoint=/usr/bin/node \
  -Dlog_level=error \
  web3-app.manifest.template \
  web3-app.manifest

# 3. 签名 manifest（生成 SGX 签名）
gramine-sgx-sign \
  --manifest web3-app.manifest \
  --output web3-app.manifest.sgx

# 4. 运行应用
gramine-sgx node your-app.js
```

### simple-web3-app.js

这是一个简单的 Node.js Web3 应用程序示例，演示如何：
- 连接到 Ethereum 节点
- 查询网络信息
- 获取账户余额

**运行方法：**

```bash
# 1. 安装依赖
npm install web3

# 2. 设置 RPC URL（可选）
export ETH_RPC_URL=http://ganache:8545

# 3. 在 Gramine 中运行
gramine-sgx node simple-web3-app.js

# 或者直接运行（不使用 SGX）
node simple-web3-app.js
```

## 完整示例：使用 Docker Compose 运行

1. 启动服务（包括 Ganache 测试网络）：
```bash
docker-compose up -d
```

2. 进入 Gramine 容器：
```bash
docker-compose exec gramine-web3-wallet bash
```

3. 复制示例文件到容器：
```bash
cp /examples/simple-web3-app.js /app/
cd /app
```

4. 安装依赖：
```bash
npm install web3
```

5. 运行示例应用：
```bash
node simple-web3-app.js
```

## 高级用法

### 使用加密存储

Gramine 支持加密文件系统，可以保护敏感的钱包数据：

```bash
# 在 manifest 中配置加密挂载
{ type = "encrypted", path = "/app/wallet", uri = "file:/app/wallet", key_name = "wallet_key" }

# 运行时，Gramine 会自动处理加密/解密
```

### 远程认证

启用 SGX 远程认证以验证应用程序的完整性：

```bash
# 在 manifest 中启用
sgx.remote_attestation = "dcap"

# 使用 RA-TLS 进行安全通信
# 参考 Gramine 文档：https://gramine.readthedocs.io/en/latest/attestation.html
```

## 故障排除

### 问题：无法连接到 Ethereum 节点

**解决方案：**
- 确认 Ganache 容器正在运行：`docker-compose ps`
- 检查网络连接：`ping ganache`
- 验证 RPC URL 配置正确

### 问题：SGX 设备未找到

**解决方案：**
- 使用 `gramine-direct` 而不是 `gramine-sgx` 进行测试
- 确认主机支持 SGX 并已安装驱动
- 检查 Docker 设备映射配置

## 更多资源

- [Gramine 官方文档](https://gramine.readthedocs.io/)
- [Web3.js 文档](https://web3js.readthedocs.io/)
- [Ethers.js 文档](https://docs.ethers.io/)
- [Hardhat 文档](https://hardhat.org/docs)
