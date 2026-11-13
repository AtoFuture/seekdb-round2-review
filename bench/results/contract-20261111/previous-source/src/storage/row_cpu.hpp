#pragma once

namespace minivectordb::detail {
// Private, thread-local benchmark controls. Both paths fully validate each row.
// Not exposed through SQL or the runtime protocol.
enum class VectorValidationMode { scalar, batched };
inline thread_local VectorValidationMode vector_validation_mode=VectorValidationMode::batched;
inline thread_local bool row_metadata_heap=false;
}
