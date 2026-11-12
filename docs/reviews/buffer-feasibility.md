# Independent buffer feasibility review

Read-only reviewer returned GO before storage implementation. Large diagnostic pread151.788ms plus CRC235.568ms =58.39% of663.372ms, distance3.38%; headroom, not predicted speedup.

append increments next_page only after a complete write. Failed partial pages remain inaccessible and unadmitted, while successful page IDs are never reused during rollback/savepoint restoration. Reopen skips trailing partial pages. Cache each Store separately, never dirty/root pages, and admit only after read/magic/CRC/type/structure validation. Enforce physical address and expected type on each hit, raw packed reference legality and slot<count at extraction. Keep owned row strings before nested/mutating callbacks, and full SQL row validation on every query.

Fixed four-way sets are feasible. Validate conflict misses, workloads exceeding capacity and first query cost; retain scan memo as independent control. WAL/index redesign needs a separate recovery architecture, SIMD currently has little measured headroom, and VA skipping conflicts with validation. Reviewer performed no tests or writes.
