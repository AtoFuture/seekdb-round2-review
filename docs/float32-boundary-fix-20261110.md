# float32 边界误拒修复（2026-11-10）

## 缺陷

`src/types/value.cpp` 的 `make_vector` 用 `std::abs(value) > std::numeric_limits<float>::max()`
提前拒绝输入。但 IEEE binary32 的最近偶数舍入溢出边界是 FLT_MAX 之上半个 ULP，即
`2^128 - 2^103`；十进制字面量 `3.40282347e38`、`3.4028235e38` 转换到 binary32 仍然是
**有限**的 FLT_MAX（位模式 `0x7f7fffff`，负数为 `0xff7fffff`）。题面 `user-provided-task.txt:40`
要求拒绝 NaN/Infinity/维度错及**转换溢出**，公开 float32 样例也明确允许输入舍入，不要求输入
精确等于某个 float32。因此这些输入本应被接受。

同样被误拒的还有把它当作查询向量使用的情况：`L2_DISTANCE(v,'[3.4028235e38]')`。

复现（JSONL）：

```sql
CREATE TABLE t(id BIGINT PRIMARY KEY, v VECTOR(1));
INSERT INTO t VALUES(1,'[3.40282347e38]');   -- 期望成功，实际 TYPE
INSERT INTO t VALUES(2,'[3.4028235e38]');    -- 期望成功，实际 TYPE
INSERT INTO t VALUES(3,'[-3.4028235e38]');   -- 期望成功，实际 TYPE
SELECT id, L2_DISTANCE(v,'[3.4028235e38]') AS d FROM t ORDER BY d LIMIT 1;  -- 期望成功，实际 TYPE
INSERT INTO t VALUES(5,'[3.4028236e38]');    -- 期望 TYPE（真溢出），修复前后一致
```

独立真值用 Python `struct` 的 IEEE float32 转换核对，不依赖被测实现。

## 修复

- `make_vector`：按 `2^128-2^103` 判定真实溢出；`(FLT_MAX, threshold)` 区间显式映射到带符号
  FLT_MAX，避免对超出 float 有限范围的 double 直接 cast；转换后再检查有限性。
- `src/sql/binder.cpp`：SQL 数字字面量解析。`std::from_chars` 对**下溢**也返回
  `result_out_of_range`，低于 binary64 范围但可表示为 binary32 有符号零的值（如 `1e-46`）
  本应被接受；改用私有不可变 C locale 的 `strtod_l` 处理该分支，不触碰调用方全局 locale。
- 真正溢出（`>= 2^128-2^103`）、非有限值、维度错误仍然拒绝。

## 验证

在 `d55b183` 之上（本提交未改动该提交的任何文件）：

- 官方题包 `protocol_check/run.py --image`：握手与 20 个公开用例全部通过
- 仓库内部 ctest：27/27 通过（含新增 `core_float32_literals`）
- 上述反例：四条边界输入全部接受并可用于距离查询，`3.4028236e38` 仍被拒绝
- 距离并列顺序专项与差分对照（升序/降序、ANN、有无索引、更新后、删除后等 12 个场景）输出不变
- 崩溃恢复 + 持久向量索引一致性检查通过
