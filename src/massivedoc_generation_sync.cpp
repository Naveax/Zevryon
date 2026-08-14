#include "massivedoc_generation_sync.hpp"

namespace zevryon::massivedoc {

std::recursive_mutex& generation_transaction_mutex() noexcept {
    static std::recursive_mutex mutex;
    return mutex;
}

} // namespace zevryon::massivedoc
