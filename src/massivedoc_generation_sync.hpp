#pragma once

#include <mutex>

namespace zevryon::massivedoc {

std::recursive_mutex& generation_transaction_mutex() noexcept;

} // namespace zevryon::massivedoc
