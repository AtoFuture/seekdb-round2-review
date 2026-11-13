# Profile and D-drive storage-path comparison

Baseline07e16cf8e185495a01234f26ddc48b5579b4fc67. User explicitly approved instrumentation, Windows bind vs Linux filesystem in D-backed MiniVectorDB WSL, then evidence-based cache/batch-read choice. No initial branch, global settings, push or formal submission changes.

Implement disabled-by-default thread-local leaf timing/counters, phase snapshots; compare on/off overhead in same binary. CPU and wall API times exclude request construction and independent validation. Write snapshots may contain other loop work and are interpreted as diagnostics, not additive CPU attribution. No per-page unbounded address trace.

Verify HKCU MiniVectorDB BasePath D:\Codex\MiniVectorDB\wsl before native Linux /var/lib/minivectordb-profile; Windows root /mnt/d/Codex/MiniVectorDB/data/path-profile. Both reside physically on D. Same image, binary, seeded2000x128/40queries,2CPU512MiB and identical warm-up. No global cache dropping. Three alternating rounds per path x profiling state,12samples. Results/reopen identical; include filesystem type and hashes. Real elapsed values profile-off, breakdown profile-on; disclose overhead and clock boundaries.

Then choose next engineering experiment from measured page-type counts, read time, checksum/decode/distance and filesystem sensitivity. High syscall time alone does not prove cache hits. Do not infer official score or cold-cache/physicalIO results.