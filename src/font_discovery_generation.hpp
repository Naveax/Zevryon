#pragma once

#include "font_catalog.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::text {

struct FontDiscoveryFace {
    std::string_view platform_identity;
    std::string_view family_name;
    std::uint16_t weight{400};
    std::uint8_t width{5};
    FontSlant slant{FontSlant::Upright};
    ScriptId preferred_script{ScriptId::Zyyy};
    std::uint16_t flags{0};
    std::span<const FontCoverageRange> coverage;
};

struct FontDiscoveryRecord {
    std::uint32_t identity_offset{0};
    std::uint32_t identity_length{0};
    std::uint32_t family_index{0};
    FontFaceId face_id{kInvalidFontFaceId};

    bool operator==(const FontDiscoveryRecord&) const noexcept = default;
};

static_assert(
    sizeof(FontDiscoveryRecord) == 16U,
    "font discovery records must remain compact");

struct FontFamilyRecord {
    std::uint32_t name_offset{0};
    std::uint32_t name_length{0};
    std::uint32_t family_key{0};

    bool operator==(const FontFamilyRecord&) const noexcept = default;
};

static_assert(
    sizeof(FontFamilyRecord) == 12U,
    "font family records must remain compact");

struct FontGenerationFingerprint {
    std::uint64_t high{0};
    std::uint64_t low{0};

    bool operator==(const FontGenerationFingerprint&) const noexcept = default;
};

enum class FontDiscoveryErrorKind : std::uint8_t {
    None = 0,
    InvalidGeneration,
    InvalidIdentity,
    InvalidFamily,
    DuplicateIdentity,
    IndexOverflow,
    SnapshotBudgetExceeded,
    CatalogBuildFailed
};

struct FontDiscoveryError {
    FontDiscoveryErrorKind kind{FontDiscoveryErrorKind::None};
    std::size_t face_index{0};
    FontCatalogErrorKind catalog_error{FontCatalogErrorKind::None};
    std::string message;
};

struct FontDiscoveryStats {
    std::uint64_t input_faces{0};
    std::uint64_t output_faces{0};
    std::uint64_t unique_families{0};
    std::uint64_t identity_bytes{0};
    std::uint64_t family_bytes{0};
    std::uint64_t snapshot_bytes{0};
    FontCatalogStats catalog;
};

class FontCatalogGeneration final {
public:
    FontCatalogGeneration(
        std::uint64_t generation_id,
        std::size_t discovery_hard_limit,
        std::size_t catalog_hard_limit);

    FontCatalogGeneration(const FontCatalogGeneration&) = delete;
    FontCatalogGeneration& operator=(const FontCatalogGeneration&) = delete;

    std::uint64_t generation_id() const noexcept;
    FontGenerationFingerprint fingerprint() const noexcept;
    const FontCatalog& catalog() const noexcept;

    std::span<const FontDiscoveryRecord> discovery_records() const noexcept;
    std::span<const FontFamilyRecord> families() const noexcept;
    std::string_view identity(FontFaceId face_id) const noexcept;
    std::string_view family_name(std::uint32_t family_index) const noexcept;

    core::ResourceSnapshot discovery_resource_snapshot() const noexcept;
    core::ResourceSnapshot catalog_resource_snapshot() const noexcept;
    bool accounting_clean() const noexcept;
    bool within_hard_limits() const noexcept;

private:
    friend bool build_font_catalog_generation(
        std::uint64_t,
        std::span<const FontDiscoveryFace>,
        std::size_t,
        std::size_t,
        std::shared_ptr<const FontCatalogGeneration>*,
        FontDiscoveryStats*,
        FontDiscoveryError*) noexcept;

    core::ResourceLedger ledger_;
    core::LedgerMemoryResource discovery_resource_;
    core::LedgerMemoryResource catalog_resource_;
    std::pmr::vector<char> string_bytes_;
    std::pmr::vector<FontDiscoveryRecord> discovery_records_;
    std::pmr::vector<FontFamilyRecord> families_;
    FontCatalog catalog_;
    std::uint64_t generation_id_{0};
    FontGenerationFingerprint fingerprint_{};
};

const char* font_discovery_error_kind_name(FontDiscoveryErrorKind kind) noexcept;

bool build_font_catalog_generation(
    std::uint64_t generation_id,
    std::span<const FontDiscoveryFace> faces,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit,
    std::shared_ptr<const FontCatalogGeneration>* output,
    FontDiscoveryStats* stats,
    FontDiscoveryError* error) noexcept;

enum class FontGenerationPublishResult : std::uint8_t {
    Published = 0,
    InvalidCandidate,
    StaleGeneration,
    IdenticalSnapshot
};

const char* font_generation_publish_result_name(
    FontGenerationPublishResult result) noexcept;

class FontCatalogGenerationStore final {
public:
    FontCatalogGenerationStore() noexcept;

    FontGenerationPublishResult publish(
        std::shared_ptr<const FontCatalogGeneration> candidate) noexcept;

    std::shared_ptr<const FontCatalogGeneration> snapshot() const noexcept;

private:
    std::atomic<std::shared_ptr<const FontCatalogGeneration>> current_;
};

} // namespace zevryon::text
