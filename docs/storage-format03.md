# Immutable disk formats 02, 03 and 04

## Current format04

New SQL databases now default to format04 (`compressed_paths=true`,
`packed_branches=true`). The low-level Store default remains format02. Opening
an existing format02/03 file preserves its version, even if the new options are
enabled. Current binaries read all three versions. Older binaries reject
format04; do not use them to open newly created format04 files. There is no
automatic migration or reclamation.

Format04 retains 4096-byte pages, checksums, two alternating commit roots and
the existing value/overflow encodings. Root/page magic is `MVDBRT04`/`MVDBPG04`.
It changes only radix-node storage and traversal:

- A retained branch stores its terminal depth in bytes12..15, and the canonical
  namespace/ordered-key prefix in bytes2080..2095. Prefix bytes from that depth
  onward must be zero. Intermediate unary branches are omitted; a leaf at depth15
  is always retained. Point lookups and ordinary scans check the prefix and jump
  over omitted levels. Mutation and paired traversal can expand a virtual unary
  page in memory without persisting it.
- Sparse branch records share type3 packed pages. A record contains version1
  (one byte), depth (one byte), a16-byte prefix, a32-byte child bitmap, then one
  little-endian64-bit nonzero child reference for each set bit. Its length must
  equal50 plus8 times the bitmap population; empty/unknown/truncated records are
  rejected. The largest record is2098 bytes. The containing packed page supplies
  the checksum and validates all slot bounds before a record is decoded.
- Bit62 identifies a packed branch reference: `bit62 | (page_id << 9) | slot`.
  Roots and non-leaf branches may use this tag; value references keep bit63.
  Tagged values are forbidden as internal links, and tagged branches are forbidden
  as leaf values. Slots508..511 and out-of-extent pages are invalid.
- A child must precede its parent physically. Two branch records may share a
  page only when the child slot precedes the parent slot. Self/forward edges are
  rejected. This preserves an acyclic immutable graph despite same-page packing.

Each mutation call uses one4096-byte node staging page and a bounded2098-byte
encoding buffer, in addition to the existing recursion and row state. Values
are fully written before node packing starts. Nodes are emitted bottom-up; a
full node page is written before moving to the next page. The final node page
must finish its complete write loop before the working root is assigned.
Failures leave the previous working root intact; partial pages never enter a
read cache. Savepoints and callbacks retain immutable old roots.

The default SQL commit policy remains `ProcessCrash`: all reachable data and
node writes complete, then the root is written and the whole file is fsynced
before acknowledgement. `Ordered` retains its extra data-before-root fsync.
Neither format04 feature removes a synchronization barrier from its selected
policy. Tests cover both policies, partial writes, injected sync errors,
SIGKILL at publication stages, and both root parities.

## Historical format03 details

Format03 extends the existing immutable page tree; page size remains 4096 bytes. New SQL databases use the selected default in Options; Store's low-level default remains format02. Existing format02 files always remain format02 even when packing is requested. Existing format03 files can be opened with packing disabled and continue to read old packed values. No migration or reclamation is implemented.

## Encoding and validation

Root and page magic are MVDBRT03 and MVDBPG03. An independently valid root must have recognized magic, correct checksum, valid extent and root bounds. Two valid roots with differing versions are rejected; an invalid candidate may be ignored during fallback. Old format02 binaries reject format03 at startup (their generic error says corrupt file).

Type3 pages hold a slot count at byte12, zero reserved bytes16..23, the existing checksum field at24, and an 8-byte little-endian offset/length directory beginning at32. Payload is packed canonically from the end of the page backwards; directory/payload bounds, adjacency, count and nonoverlap are checked. At most508 slots fit. Empty values are supported.

Only format03 leaf references at depth15 may contain bit63. Such references encode `(1ULL<<63) | (page_id<<9) | slot`; slots508..511 are invalid. Physical addresses must be within committed/working extent and precede the referencing parent. Internal links, roots and overflow-next pointers cannot carry tags. Ordinary overflow values keep their previous representation inside format03.

## Write and recovery rules

A bounded mutation batch packs fitting values, flushes all data pages, then writes index parents. The working root changes only after the complete merge succeeds. Failed partial pages are not cached. A page with only one value falls back to an ordinary value record; old format03 packed singleton pages remain readable. Oversized values use ordinary overflow chains. This avoids extra packed-page management when density cannot improve.

Commit synchronization follows the selected policy described above: Ordered has two barriers, while the SQL ProcessCrash default writes all data and the root before one final file fsync and acknowledgement. Statement savepoints, transaction rollback and restart selection remain common to packed and ordinary values. Appending new versions does not reclaim historical pages.

## Scan cache

Each scan owns a lazy fixed-size direct-mapped cache of validated immutable packed pages, destroyed at scan completion or exception. SQL currently defaults to a4MiB cross-query data buffer and disables this scan memo; the low-level Store scan budget remains256KiB. See[data-buffer.md](data-buffer.md). When explicitly configured with1MiB, Entries include a4096-byte page and8-byte identifier (255 entries,1046520 bytes for1MiB on the tested ABI). Fixed object/allocation overhead, the current returned row, index recursion and nested scans are additional. This is not a process memory cap.

A cache hit still validates the requested physical address and slot. The row is copied into an owned string before a callback can mutate the store or start another scan. No mutable/incomplete page enters this cache. Zero budget disables reuse. Index cache and packed scan cache are separate.

Full SQL row validation remains in force: filtered and unprojected fields are checked for valid UTF-8, finite float32, dimensions and boundaries. Page-skipping vector summaries are therefore not silently introduced by this format change.
