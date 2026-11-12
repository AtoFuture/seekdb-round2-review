# MiniVectorDB 开发环境

工作根目录 D:/Codex/MiniVectorDB，独立仓库 repo/，分支 2026-competition-round2，初始官方提交 c842a368c3394748d0499386c18f6b465242e141。origin 指向 juanzhi-gcb/seekdb，upstream 指向 oceanbase/seekdb。初赛分支和已有 GPU 优化工作区没有修改。

## 已验证

- 专用 WSL 发行版 MiniVectorDB，注册系统盘目录 D:/Codex/MiniVectorDB/wsl。
- Alpine 3.22.8，Linux x86_64，由官方 3.22.2 minirootfs 导入后更新。
- 初始根文件系统 SHA256：412454ed98025ad9cd910d13f3d448b184e81502baa83180e89f98d0f13674be。
- Docker Engine 28.3.3，overlay2，cgroup v2，本地 Unix socket，无 TCP 监听。
- Docker data-root 为 /var/lib/minivectordb-docker，位于 D 盘虚拟磁盘内。
- 独立 Docker 客户端配置 /var/lib/minivectordb-docker-client；Linux PATH 排除旧 Rancher 凭据助手。
- GCC 14.2.0，CMake 3.31.7，Ninja。
- 容器 /data 映射 /mnt/d/Codex/MiniVectorDB/data，已写入 environment-probe.txt 并从 Windows D 盘读回。
- 基础镜像 alpine:3.22.8，摘要 sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f。
- WSL 全局 memory=24GB、swap=8GB 保留原设置；用户明确选择不修改全局交换文件路径。不能宣称宿主所有交换/系统日志也在 D 盘。
- 本地 Docker 报告 No swap limit support，本地资源测试不等同正式评测环境。

源码、编译产物、依赖下载、镜像层、项目测试数据和项目日志均位于 D 盘。Codex/Windows 自身设置和日志不由项目脚本控制。

## 原 Docker Desktop 故障

原 Desktop 数据盘通过 C:/Users/gcb20/AppData/Local/Docker/wsl 目录链接指向 D:/Docker/wsl，未迁移、清空或重置。

Desktop 4.73.0 启动在 dockerInference 通信套接字失败。诊断时保留原运行时目录 C:/Users/gcb20/AppData/Local/Docker/run.before-minivectordb-20261109，将 run 链接到 D:/Codex/MiniVectorDB/work/docker-runtime；重试仍报地址占用，故停止本次启动的 Desktop 进程并改用独立引擎。原镜像、容器数据没有删除。

本项目不使用原 Desktop/Rancher 上下文，不通过恢复出厂设置修复。

## 操作

在 PowerShell 中调用 D:\Codex\MiniVectorDB\repo\scripts\dev.ps1，使用 -Action 参数：
- Status：检查 Docker、编译器和 CMake。
- Build：在 D 盘 build/core 中编译。
- Test：运行核心、SQL 和命令行测试，数据位于 D 盘 data/core-tests。
- Image：构建开发验证镜像。
- RuntimeImage：构建带本地 SQL 开发入口的运行镜像。
- RuntimeTest：在只读根目录、无网络的运行镜像中检查跨容器重启及 EOF 回滚。
- ContainerTest：在无网络、2 CPU、512 MiB 内存容器内测试。
- StartDocker：重启机器后启动专用 Docker；之后用 Status 验证就绪。仅在引擎未运行时使用。

脚本不安装/重置系统，不改全局 WSL 配置。Dockerfile 提供 test 和 runtime 两个目标；runtime 使用显式 dev-tsv 本地协议，尚未适配官方协议。基础镜像和直接依赖已固定；传递依赖仍依赖 apk 仓库状态，最终交付须补可复现归档或冻结清单。

来源：[Alpine Docker 文档](https://wiki.alpinelinux.org/wiki/Docker)、[Microsoft WSL 配置](https://learn.microsoft.com/en-us/windows/wsl/wsl-config)。实测记录位于项目 work/environment/。
