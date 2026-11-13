#pragma once
#include "minivectordb/store.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace minivectordb {
using Cell = std::variant<std::int64_t, std::string, std::vector<float>, double>;
using Row = std::vector<Cell>;

enum class ErrorCode { Syntax, Schema, Type, Constraint, Transaction, Corruption };
class Error : public std::runtime_error {
  ErrorCode code_;
public:
  Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
  ErrorCode code() const noexcept { return code_; }
};

struct Options {
  std::filesystem::path sort_directory;
  // Working-set target, excluding the current row/key and allocation/stream overhead.
  std::size_t memory_budget_bytes = 8 * 1024 * 1024;
  bool selective_decode = true;
  bool batch_writes = true;
  // Format04 omits unary radix pages; existing format02/03 files stay unchanged.
  bool compressed_paths = true;
  bool packed_branches = true; // Format04 sparse nodes share immutable pages.
  bool compact_node_refs = true; // Format06 canonical compact node addresses.
  bool compact_values = true; // Format05 bounded lossless packed payloads.
  bool discard_file_cache = false; // Optional advisory release after owned copies/fsync.
  std::size_t index_cache_bytes = 1024 * 1024; // Bounded validated INDEX pages; zero disables.
  bool packed_values = true; // Format03 for new files; existing format is preserved.
  std::size_t packed_scan_cache_bytes = 0; // Optional per-scan packed memo; data buffer is the default.
  std::size_t data_cache_bytes = 524288; // Bounded cross-query data pages; zero disables.
  // Bounded paired traversal; false retains the reference index/get path.
  // True also detects missing index entries that the reference scan cannot see.
  bool paired_index_scan = true;
  // Shared validated-snapshot budget: numeric columns cap at 64 MiB, raw rows
  // at 8 MiB. Complete probation scans size numeric arrays to actual rows.
  // Zero disables both; allocator and bounded preparation scratch are separate.
  std::size_t scan_snapshot_bytes = 64 * 1024 * 1024;
  // Allow unindexed exact TopK scans to use the shared snapshot.
  bool table_scan_snapshot = true;
  // Retain the arena across invalidation; every rebuild still validates all rows.
  bool reuse_scan_snapshot_buffer = true;
  // Reuse one candidate-vector buffer per TopK query without materializing Cells.
  bool reuse_vector_scratch = true;
  // Reuse completely validated numeric columns instead of parsing raw rows.
  // Shares the snapshot budget; unsupported predicates retain the raw path.
  bool typed_scan_cache = true;
  // Reuse validation proofs of a complete owned raw snapshot. False retains
  // full per-query row validation for controlled comparisons.
  bool validated_snapshot_replay = true;
  // Compile required field traversal once for zero/one-predicate raw replays.
  bool projected_snapshot_replay = true;
  // Internal SQL candidates have decoder/cache finiteness proofs. Keep the
  // checked public vector API unchanged; false is the measured reference.
  bool validated_vector_kernel = true;
  // Interleave independent exact distances, with at most 64 KiB query scratch.
  // False retains the scalar validated kernel for measured comparisons.
  bool batched_vector_kernel = true;
  // AVX2 executes only after CPU capability detection; portable batching remains
  // the fallback. Independent candidate lanes keep scalar double result bits.
  bool simd_vector_kernel = true;
  // Optional wider exact lanes on CPUs/OSes with AVX-512F support.
  bool wide_simd_vector_kernel = true;
  // Experimental exact rejection by a conservatively bounded FP32 estimate.
  // Remaining candidates always use the original double distance kernel.
  bool bounded_fp32_screen = true;
  bool certified_block_scan = true; // Skip redundant predicates only with an exact interval proof.
  // Transpose completed numeric snapshots once, within the existing arena.
  // AVX2 queries can then read candidate blocks without per-query repacking.
  bool transposed_scan_cache = true;
  // Admit one validated VARCHAR predicate column alongside numeric vectors,
  // within the same snapshot budget; unsupported shapes use raw snapshots.
  bool text_scan_cache = true;
  // Cluster a completed snapshot by a reusable ordinary column before SIMD
  // transposition, making selective filters occupy dense candidate blocks.
  bool clustered_scan_cache = true;
  // Restrict cached scans to bounds on the column actually clustered. False
  // retains complete cached scans for controlled comparisons.
  bool clustered_range_scan = true;
  // New flat vector indexes keep validated table/key references instead of a
  // second persistent vector payload. Existing version1 indexes retain copies.
  bool referenced_vector_index = true;
  // Competition recovery model: process SIGKILL, with final fsync before ACK.
  // Ordered remains available for data-before-root persistence ordering.
  CommitSyncPolicy commit_sync_policy = CommitSyncPolicy::ProcessCrash;
  bool bounded_int8_screen = true; // Auxiliary conservative bound; exact survivors unchanged.
  bool byteplane_values = true; // Format07 lossless generic byte-plane packing.
  std::size_t projection_cache_bytes=8*1024*1024; // Shared snapshot budget.
  bool journal_store=false; // New-file backend; existing formats autodetected.
};
struct QuerySink {
  std::function<void(const std::vector<std::string>&)> columns;
  std::function<void(const Row&)> row;
};
enum class SearchMode { Exact, Ann };
struct ExecutionInfo {
  std::uint64_t snapshot_rows_examined = 0;
  std::uint64_t validated_snapshot_rows_examined = 0;
  // Arena capacity retained after TopK execution; other paths do not collect it.
  std::size_t snapshot_bytes = 0;
  std::uint64_t typed_rows_examined = 0;
  std::uint64_t certified_blocks_examined = 0;
  std::size_t typed_cache_bytes = 0;
  std::size_t projection_cache_bytes = 0;
  std::uint64_t vector_index_entries_examined = 0;
  // SELECT reports emitted rows; DML reports matching rows (including unchanged UPDATEs).
  std::uint64_t affected_rows = 0;
  std::uint64_t rows_examined = 0, distances_computed = 0, spill_runs = 0;
  // Decoded Cells only: includes winner refetch and DML scans, excludes output copies.
  std::uint64_t vector_values_materialized = 0, cells_materialized = 0;
};

// Single-threaded, non-reentrant. Callback references last only during the callback.
// Sink exceptions propagate; partial streamed SELECT output is possible on an error.
// Uncommitted work is rolled back on destruction. I/O exceptions propagate unchanged.
class Database {
public:
  // The parent directory of the database file must already exist.
  explicit Database(const std::filesystem::path&, Options = {});
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  ExecutionInfo execute(std::string_view sql, const QuerySink& sink = {}, SearchMode mode = SearchMode::Exact);
  bool in_transaction() const noexcept;
  CacheStats data_cache_stats() const noexcept;
  void reset_data_cache_stats() noexcept;
  CacheStats cache_stats() const noexcept;
  void reset_cache_stats() noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
