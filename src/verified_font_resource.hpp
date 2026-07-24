#pragma once

#include "font_resource_integrity.hpp"
#include "font_resource_sfnt.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class VerifiedFontResourceErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    OutputBudgetExceeded,
    AllocationFailed,
    ParseFailed,
    IntegrityFailed
};

struct VerifiedFontResourceError {
    VerifiedFontResourceErrorKind kind{VerifiedFontResourceErrorKind::None};
    std::size_t byte_offset{0};
    std::uint32_t table_tag{0};
    SfntParseErrorKind parse_error{SfntParseErrorKind::None};
    SfntIntegrityErrorKind integrity_error{SfntIntegrityErrorKind::None};
    std::string message;
};

struct VerifiedFontResourceStats {
    std::uint64_t resource_id{0};
    std::uint64_t source_bytes{0};
    std::uint64_t retained_bytes{0};
    SfntParseStats parse;
    SfntIntegrityStats integrity;
};

class VerifiedFontResource final {
public:
    VerifiedFontResource(const VerifiedFontResource&) = delete;
    VerifiedFontResource& operator=(const VerifiedFontResource&) = delete;

    std::uint64_t resource_id() const noexcept;
    std::span<const std::byte> bytes() const noexcept;
    const SfntResourceView& view() const noexcept;
    const SfntParseStats& parse_stats() const noexcept;
    const SfntIntegrityStats& integrity_stats() const noexcept;

    core::ResourceSnapshot resource_snapshot() const noexcept;
    bool accounting_clean() const noexcept;
    bool within_hard_limit() const noexcept;

private:
    friend bool build_verified_font_resource(
        std::uint64_t,
        std::span<const std::byte>,
        std::uint32_t,
        std::size_t,
        std::shared_ptr<const VerifiedFontResource>*,
        VerifiedFontResourceStats*,
        VerifiedFontResourceError*) noexcept;

    VerifiedFontResource(
        std::uint64_t resource_id,
        std::size_t hard_limit) noexcept;

    core::ResourceLedger ledger_;
    core::LedgerMemoryResource byte_resource_;
    std::pmr::vector<std::byte> bytes_;
    SfntResourceView view_;
    SfntParseStats parse_stats_;
    SfntIntegrityStats integrity_stats_;
    std::uint64_t resource_id_{0};
};

const char* verified_font_resource_error_kind_name(
    VerifiedFontResourceErrorKind kind) noexcept;

// Copies caller-owned font bytes once into an immutable, ledger-bounded
// generation, then validates and stores one selected face view over the retained
// bytes. Failure publishes no resource. Resource IDs must be non-zero and are
// caller-defined generation identities, not content hashes.
bool build_verified_font_resource(
    std::uint64_t resource_id,
    std::span<const std::byte> source,
    std::uint32_t face_index,
    std::size_t hard_limit,
    std::shared_ptr<const VerifiedFontResource>* output,
    VerifiedFontResourceStats* stats,
    VerifiedFontResourceError* error) noexcept;

} // namespace zevryon::text
