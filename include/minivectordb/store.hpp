#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <stdexcept>
namespace minivectordb {
// Ordered persists data before publishing the root. ProcessCrash targets a
// process-only crash with the kernel/filesystem alive; it still fsyncs the whole
// file before acknowledging commit, but does not order an unacknowledged root
// against its data across power loss or a kernel/device failure.
enum class CommitSyncPolicy { Ordered, ProcessCrash };
struct CacheStats {
  std::uint64_t hits=0, misses=0, evictions=0;
  std::size_t capacity_bytes=0, resident_pages=0;
};
// Single-writer, single-thread API. Immutable pages are append-only; no compaction yet.
// All mutations require begin(). Each failed put/erase leaves the prior working root intact.
// Cached pages assume exclusive cooperative ownership: external writes to an open file
// are unsupported. Close and reopen to validate externally modified pages.
// Savepoints belong to one Store and one active transaction.
class Store {
public:
  class KeySetMismatch : public std::runtime_error {
  public:
    KeySetMismatch() : std::runtime_error("corrupt MiniVectorDB file: paired key sets differ") {}
  };
  struct Mutation {
    std::int64_t key;
    std::optional<std::string> value; // nullopt erases the key; missing keys are a no-op.
  };
  static constexpr std::size_t max_batch_mutations = 128;
  static constexpr std::size_t max_batch_bytes = 512 * 1024;
  class Savepoint {
    friend class Store;
    std::uint64_t root_ = 0, epoch_ = 0, owner_ = 0;
  };
  // Cache budget covers the allocated entry array, including page and entry metadata.
  // Fixed Store bookkeeping and allocator overhead are excluded; zero disables caching.
  // Packing selects format03; compressed_paths selects format04 for new files.
  // Existing formats are never migrated. Format04 optionally packs sparse
  // branch nodes as well as omitting unary paths; both encodings remain readable.
  // Each scan lazily allocates its own bounded validated packed-page cache.
  explicit Store(const std::filesystem::path& path, std::size_t index_cache_bytes=0,
                 bool packed_values=false, std::size_t packed_scan_cache_bytes=262144,
                 std::size_t data_cache_bytes=0,
                 CommitSyncPolicy commit_sync_policy=CommitSyncPolicy::Ordered,
                 bool compressed_paths=false, bool packed_branches=true);
  // Separate lazy four-way cache for validated packed and overflow pages. The budget
  // includes the entire set array (pages, IDs, ranks, padding); fixed object and
  // allocator overhead are excluded. A budget smaller than one set disables it.
  CacheStats data_cache_stats() const noexcept;
  void reset_data_cache_stats() noexcept;
  CacheStats cache_stats() const noexcept;
  // Reset event counters, retaining the cache and its capacity/residency.
  void reset_cache_stats() noexcept;
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  void begin();
  std::optional<std::string> get(std::int64_t key) const;
  void put(std::int64_t key, std::string_view value);
  bool erase(std::int64_t key);
  std::optional<std::string> get_in(std::uint64_t space, std::int64_t key) const;
  void put_in(std::uint64_t space, std::int64_t key, std::string_view value);
  bool erase_in(std::uint64_t space, std::int64_t key);
  // Atomic working-root update; duplicate keys and oversized batches fail before writing.
  // Payload is bounded by max_batch_bytes, except one individually oversized value.
  void apply_batch_in(std::uint64_t space, std::span<const Mutation> mutations);
  void scan_in(std::uint64_t space, const std::function<void(std::int64_t, std::string_view)>& visitor) const;
  // Co-visits equal key sets in signed order from one immutable root snapshot.
  // Both namespaces are resolved before callbacks; callbacks may mutate/rollback.
  // Views last only during the callback. One shared bounded scan cache plus
  // O(tree height) pages and the two current owned values are retained.
  // Throws KeySetMismatch on unequal key sets; earlier visits are not undone.
  void scan_equal_in(std::uint64_t left_space, std::uint64_t right_space,
                     const std::function<void(std::int64_t, std::string_view, std::string_view)>& visitor) const;
  void commit();
  void rollback();
  Savepoint savepoint() const;
  void restore(const Savepoint&);
  // Visits in signed key order. Uses an immutable root snapshot; callback may mutate the active transaction.
  void scan(const std::function<void(std::int64_t, std::string_view)>& visitor) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
