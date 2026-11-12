#pragma once

namespace minivectordb::detail {
// Private diagnostic control for same-binary comparisons. Neither mode skips
// Unicode or scalar-length validation, and callers on other threads are isolated.
enum class TextValidationMode { scalar, ascii_blocks };
inline thread_local TextValidationMode text_validation_mode=TextValidationMode::ascii_blocks;
}
