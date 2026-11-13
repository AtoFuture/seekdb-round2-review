# Isolated durable append-log experiment

This executable is independent of MiniVectorDB storage, SQL, and its CMake targets. It evaluates one successful fsync per transaction against a two-fsync reference with identical self-contained records. It is not a production WAL or a proposed change to production synchronization.

## Format and recovery

All integers are little endian. Each frame consists of a 32-byte header, at most 1 MiB of payload, and a 24-byte commit footer.

Header: magic/version `MVLOG001` (8 bytes), sequence (8), payload length (4), operation count (4), reserved zero (4), CRC32 over preceding 28 bytes (4). Every operation has unsigned 64-bit key, one-byte tag (0 = put, 1 = delete), 32-bit value length and value bytes. Delete values must be empty; repeated operations apply in recorded order. Footer: `COMMIT01` (8), matching sequence (8), CRC32 over header and payload (4), CRC32 over preceding 20 footer bytes (4). CRC32 is the IEEE reflected polynomial 0xedb88320, implemented here without another database/engine.

Replay holds an exclusive nonblocking flock, walks the entire committed prefix from sequence 1, validates header CRC before trusting lengths, enforces the payload bound, verifies footer/frame CRC and validates every operation before applying it. It truncates and fsyncs only an incomplete final frame. A complete corrupt header, committed frame, unsupported operation or nonconsecutive sequence fails closed. An incomplete header is also a discarded tail. A file with corrupted contents that happens to look like an incomplete tail cannot be distinguished from a torn tail; checksums are corruption detection, not protection against deliberately recomputed malicious records. Existing state is rebuilt entirely from the file; no external index is needed. Transaction allocations are bounded, while total recovered map size grows with live state.

One-sync mode writes header, payload and footer in one write loop, then fsyncs. Two-sync mode writes header/payload and fsyncs, then writes the identical footer and fsyncs. append returns (ACK) only after final fsync and state update succeed. Any exception after IO begins poisons the instance; retry requires close/reopen. Preflight oversized/invalid input rejection leaves it usable. An unacknowledged but complete record can appear after reopen: uncertain completion is not guaranteed rollback. A process crash alone cannot simulate power loss.

Creation/open setup syncs the file and its parent directory before measured transactions. Reopening an existing complete file also performs these setup syncs; they are excluded from transaction counters. Recovery tail truncation syncs are likewise excluded from those counters. The containing directory should already reside on durable storage; recursively created ancestors are not separately fsynced.

## Tests

`append_log --selftest DIRECTORY` owns its fixture filenames within DIRECTORY. Use a dedicated scratch directory. Both modes exercise:

- exact reopen after repeated-key puts, delete and empty value;
- every byte truncation of a second transaction, followed by append to the recovered prefix;
- every byte single-bit corruption in two complete transactions;
- CRC-valid invalid header lengths/count/reserved/sequence, unknown operations, delete-with-value, cross-frame length, unconsumed payload and footer sequence;
- 1 MiB exact payload boundary and over-limit rejection;
- exclusive writer lock;
- injected write failures after 0, 10, 40 and 70 bytes, poisoned retry, reopening and durable retry;
- injected fsync failure at each synchronization point, no successful ACK, poisoned retry and documented uncertain recovery;
- child SIGKILL before append, after write before fsync, and after ACK.

Fault controls are explicit fields used only by selftests: write_budget returns an injected EIO after a byte budget; sync_call throws an injected EIO **before** the selected fsync syscall; pause_fd signals a pipe after a write loop then waits for SIGKILL. These hooks test control flow and process recovery. They do not emulate storage-device reorder, torn sectors, real kernel fsync failure, or power failure. Real process termination is used for kill tests.

## Build and controlled measurement

Compile C++20 with `g++ -std=c++20 -O2 -Wall -Wextra -Werror experiments/append_log.cpp -o /D_BACKED_BUILD/append_log`. No new dependencies.

`append_log --bench FRESH_DIRECTORY [transactions=20] [rounds=3] [rows=100] [payload=800]`

Each round alternates the mode order, beginning two-sync then one-sync. Output is JSONL. The input transactions and independent expected map are prepared before timing. Write time includes framing/CRC, writes, fsync and in-memory application; it excludes file open/creation, workload preparation and exact-state comparison. Replay time measures reading/validation/state reconstruction only; reopen time additionally includes opening/locking and setup synchronization. Both live and replay state must equal the independently constructed full key/value map.

Each JSON row reports transaction count, rows/transaction, value bytes, file/write bytes, write syscall count, transaction fsync count, write seconds, replay seconds, total reopen seconds and exact checks. Setup/recovery fsyncs are deliberately not counted as transaction fsyncs. No warm/cold cache claim: immediate replay is normally warm. Both modes have exactly the same framing and bytes. One-sync also coalesces the footer write; interpret this as a combined write/sync strategy comparison, not a pure syscall-isolated fsync experiment.

Use fresh directories for repeated invocations. The program refuses existing benchmark files. Default data per mode is 20 transactions × 100 rows × 800 value bytes, or 100 transactions when explicitly selected. Workload preparation is bounded to an estimated 128 MiB plus allocator overhead. Parent controls serialization of measurements and supplies environment labels/CPU-memory limits externally. Native Linux storage must be inside the D-backed MiniVectorDB WSL disk; Docker must use the same source/binary with 2 CPUs and 512 MiB and D-backed data.

No SQL throughput equivalence, compounded speedup, group commit, concurrency scaling, index maintenance, checkpoint, garbage collection, replication, production durability certification or migration claim follows from this experiment.
