#include "shadow_resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::uint64_t next_random(std::uint64_t& state) noexcept {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

} // namespace

int main() {
    using zevryon::core::ResourceClass;
    using zevryon::core::ShadowMismatchKind;
    using zevryon::core::ShadowResourceLedger;
    using zevryon::core::ShadowSnapshotField;
    using zevryon::core::resource_class_count;

    constexpr std::uint64_t verification_interval = 257U;
    constexpr std::size_t operation_count = 250'000U;
    ShadowResourceLedger ledger(verification_interval);

    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        ledger.set_hard_limit(
            static_cast<ResourceClass>(index),
            65'536U + index * 1'024U);
    }

    std::uint64_t performed_operations =
        static_cast<std::uint64_t>(resource_class_count);
    std::uint64_t random_state = 0x6A09E667F3BCC909ULL;
    for (std::size_t operation = 0U; operation < operation_count; ++operation) {
        const std::uint64_t random = next_random(random_state);
        const auto resource_class = static_cast<ResourceClass>(
            static_cast<std::size_t>(random % resource_class_count));
        const std::size_t bytes =
            1U + static_cast<std::size_t>((random >> 8U) % 4'096U);

        switch ((random >> 24U) % 8U) {
        case 0U:
        case 1U:
        case 2U:
            static_cast<void>(ledger.try_reserve(resource_class, bytes));
            break;
        case 3U:
            ledger.release(resource_class, bytes);
            break;
        case 4U:
            ledger.record_cache_hit(resource_class);
            break;
        case 5U:
            ledger.record_cache_miss(resource_class);
            break;
        case 6U:
            ledger.record_physical_read(resource_class, random);
            break;
        default:
            ledger.record_physical_write(resource_class, random >> 1U);
            ledger.record_eviction(resource_class);
            ++performed_operations;
            break;
        }
        ++performed_operations;
    }

    if (!require(ledger.verify_now(), "final full verification succeeds") ||
        !require(ledger.healthy(), "matching C++ and Rust ledgers remain healthy") ||
        !require(
            ledger.diagnostics().operations == performed_operations,
            "every mutating operation is counted") ||
        !require(
            ledger.diagnostics().verifications >=
                performed_operations / verification_interval,
            "periodic verification cadence is enforced") ||
        !require(
            ledger.diagnostics().mismatches == 0U,
            "no mismatch is recorded for the deterministic stress stream")) {
        return 1;
    }

#if defined(ZEVRYON_SHADOW_LEDGER_TEST_HOOKS)
    ShadowResourceLedger divergent(0U);
    divergent.set_hard_limit(ResourceClass::SourceWindow, 128U);
    if (!require(
            divergent.inject_shadow_reservation_for_testing(
                ResourceClass::SourceWindow,
                1U),
            "test hook mutates only the Rust shadow") ||
        !require(!divergent.verify_now(), "full verification detects divergence") ||
        !require(!divergent.healthy(), "divergence latches unhealthy state") ||
        !require(
            divergent.diagnostics().first_mismatch ==
                ShadowMismatchKind::SnapshotField,
            "first divergence is classified as a snapshot-field mismatch") ||
        !require(
            divergent.diagnostics().first_field ==
                ShadowSnapshotField::CurrentBytes,
            "first divergent field is current bytes") ||
        !require(
            divergent.diagnostics().expected == 0U &&
                divergent.diagnostics().actual == 1U,
            "diagnostics retain exact primary and shadow values")) {
        return 1;
    }
#endif

    std::cout << "Shadow resource ledger tests passed\n";
    return 0;
}
