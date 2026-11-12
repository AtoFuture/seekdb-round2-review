# 2026-11-10 官方协议复核

来源为 oceanbase/seekdb 的固定提交 f4d4e26653af823a7601668802f80e374ad6643c：
[STDIO_PROTOCOL.md](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/STDIO_PROTOCOL.md)、
[公开检查程序](https://github.com/oceanbase/seekdb/tree/f4d4e26653af823a7601668802f80e374ad6643c/protocol_check)。
新增文档明确描述当前判题器使用 JSONL 双向通信，取代此前题面中的模糊纯 SQL 表述。

上游新增的 8 个文件以及此前 42 个公开样例文件均按 Git blob SHA1 校验。原文件保存在 D:/Codex/MiniVectorDB/work，未修改检查逻辑。公开样例仍为 v4、20 个用例、95 条 SQL。组织者说明 Python 程序由 AI 编写且未细致运行，因此不能只信脚本的 PASS 标签。

## 已复现问题

上传版 873e139 的实际 Docker 镜像 ac93c9b166ed664c75cc1f091a94b375025d6f19f1ba42781cd40ffdf1c50a67，用新官方驱动仅测握手，3 秒诊断超时，结果 JSON response timeout。此处缩短超时只是为了复现当前程序从不发 ready，不修改正式120秒启动限制。公开 SQL 的旧诊断另已复现 CREATE VECTOR INDEX 失败。这两项是明确缺口；平台没有提供足够日志证明哪项是其唯一失败原因。

## 驱动复核与执行约束

- Python -O 或 PYTHONOPTIMIZE 会移除结果比较的 assert，造成假通过。启动器要求 sys.flags.optimize==0，并先故意喂错结果确认比较器拒绝。
- shutdown 后阻塞读取剩余 stdout 缺少超时；继承 stdout 的子进程可能拖住 --command 测试。运行在 Docker 并使用外部总时限。
- Python JSON 解析对 1e999 不会拒绝溢出，虽然检查到的浮点结果仍由比较器拒绝；数据库自行严格拒绝非有限输出。
- 无序浮点行使用贪心匹配，部分接近容差的集合可能假失败。当前公开文件没有这一组合；必要时用独立匹配核对，不能修改正确数据库迎合错误判定。
- 原脚本默认 Docker 命名卷。D 盘启动器仅给 Session 传入 data_root，将 /data 绑定到 D 盘临时目录，并检查实际挂载、2GiB内存及只读根文件系统。记录实际 swap 限制，协议和比较代码保持原样。
- 本机 Docker 明确报告 No swap limit support；驱动虽传 --memory-swap=2g，实际 inspect 为 -1。保留用户现有 WSL 全局配置；因此本地结果只能证明功能，不能证明正式禁用 swap 的资源准入。前两次修复版握手诊断被启动器的 swap 断言拦下，非数据库错误；记录差异后同一镜像握手通过。
- 使用专用 Linux WSL 和 Python3.12；原程序的管道 selector/killpg 不适合本机 Windows Python3.7。
- config 中 error_codes 为空，因此只要求非空错误码，不能据此宣称固定错误码映射验证通过。
- 公开驱动只正常重启，不包含强杀；另外用独立模型和强杀恢复测试验证事务与索引。

验证结果单独记录于 admission-validation.md；公开检查不等于隐藏正式准入，也不产生性能或 Agent 排名分。
