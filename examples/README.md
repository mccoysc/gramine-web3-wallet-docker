# 示例文件

本目录包含使用 Gramine Web3 钱包 Docker 镜像的示例文件。

## 目录结构

- `mysql-ratls/` - MySQL RA-TLS 服务端示例
- `mysql-client-ratls/` - MySQL RA-TLS 客户端示例

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
