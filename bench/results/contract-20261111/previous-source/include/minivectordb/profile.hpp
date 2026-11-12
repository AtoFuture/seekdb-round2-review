#pragma once
#include <cerrno>
#include <chrono>
#include <cstdint>
namespace minivectordb::profile {
struct Metric {
  std::uint64_t calls{}, bytes{}, nanoseconds{};
  bool operator==(const Metric&) const = default;
};
struct Snapshot {
  Metric pread,pwrite,fsync,page_checksum,row_validation,row_materialization,row_decode,vector_distance;
  Metric page_read_branch,page_read_overflow,page_read_packed,packed_cache_hit;
  Metric table_scan,scan_consume,predicate_evaluation,vector_scratch_copy;
  bool operator==(const Snapshot&) const = default;
};
// Per-thread diagnostics. Enable/reset only between synchronous operations.
// Reset preserves the enable flag; disabling preserves the current snapshot.
// Timings are elapsed (not CPU) nanoseconds, with instrumentation overhead.
// table_scan and scan_consume are inclusive scan_select timings: table_scan covers
// lookup/traversal plus every row callback; scan_consume covers each callback,
// including validation, materialization, predicates and its visitor. Winner
// refetch is outside both. Never add these to each other or to leaf timings.
// Other timed stages are leaves. predicate_evaluation starts after its Cell is
// materialized and counts evaluated predicates, including a rejecting predicate.
// page_read_* count typed read requests (including failures), not extra I/O.
// row_decode combines eager validation/materialization; lazy stages are separate.
// vector_scratch_copy times candidate copies without creating Cells; bytes count
// successfully copied float payloads. It is separate from row_materialization.
// Materialization counts uncached cells. Syscall counts include failed attempts;
// syscall bytes count only successfully transferred bytes (including short I/O).
namespace detail {
inline thread_local bool active=false;
inline thread_local Snapshot counters{};
}
inline bool enabled() noexcept { return detail::active; }
inline void set_enabled(bool value) noexcept { detail::active=value; }
inline void reset() noexcept { detail::counters={}; }
inline Snapshot snapshot() noexcept { return detail::counters; }
namespace detail {
class Scope {
  Metric* metric_;
  std::chrono::steady_clock::time_point start_{};
public:
  explicit Scope(Metric Snapshot::* member) noexcept : metric_(active?&(counters.*member):nullptr) {
    if(metric_) {
      const int saved=errno;
      start_=std::chrono::steady_clock::now();
      ++metric_->calls;
      errno=saved;
    }
  }
  ~Scope() {
    if(metric_) {
      const int saved=errno;
      metric_->nanoseconds+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start_).count());
      errno=saved;
    }
  }
  void bytes(std::uint64_t count) noexcept { if(metric_) metric_->bytes+=count; }
  Scope(const Scope&)=delete;
  Scope& operator=(const Scope&)=delete;
};
inline void count(Metric Snapshot::* member) noexcept { if(active) ++(counters.*member).calls; }
}
}
