#include "font_discovery_generation.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFingerprintHighOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFingerprintLowOffset = 7809847782465536322ULL;
constexpr std::uint64_t kFingerprintHighPrime = 1099511628211ULL;
constexpr std::uint64_t kFingerprintLowPrime = 14029467366897019727ULL;

struct FingerprintBuilder {
    std::uint64_t high{kFingerprintHighOffset};
    std::uint64_t low{kFingerprintLowOffset};

    void add_byte(std::uint8_t byte) noexcept {
        high ^= byte;
        high *= kFingerprintHighPrime;
        low ^= static_cast<std::uint8_t>(byte + 0x9dU);
        low *= kFingerprintLowPrime;
    }

    void add_u64(std::uint64_t value) noexcept {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            add_byte(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void add_string(std::string_view value) noexcept {
        add_u64(static_cast<std::uint64_t>(value.size()));
        for (const char raw_byte : value) {
            add_byte(static_cast<std::uint8_t>(
                static_cast<unsigned char>(raw_byte)));
        }
    }

    FontGenerationFingerprint finish() const noexcept {
        return {high, low};
    }
};

void clear_error(FontDiscoveryError* error) noexcept {
    if (error != nullptr) {
        error->kind = FontDiscoveryErrorKind::None;
        error->face_index = 0U;
        error->catalog_error = FontCatalogErrorKind::None;
        error->message.clear();
    }
}

bool fail(
    FontDiscoveryErrorKind kind,
    std::size_t face_index,
    const char* message,
    FontDiscoveryError* error,
    FontCatalogErrorKind catalog_error = FontCatalogErrorKind::None) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->face_index = face_index;
        error->catalog_error = catalog_error;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

std::uint8_t byte_at(std::string_view value, std::size_t index) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<unsigned char>(value[index]));
}

bool valid_utf8_scalar_sequence(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }

    std::size_t index = 0U;
    while (index < value.size()) {
        const std::uint8_t first = byte_at(value, index);
        if (first == 0U) {
            return false;
        }
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t length = 0U;
        std::uint32_t codepoint = 0U;
        std::uint32_t minimum = 0U;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2U;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3U;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4U;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }

        if (length > value.size() - index) {
            return false;
        }
        for (std::size_t offset = 1U; offset < length; ++offset) {
            const std::uint8_t continuation = byte_at(value, index + offset);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        index += length;
    }
    return true;
}

bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t* output) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

void append_bytes(std::pmr::vector<char>* output, std::string_view value) {
    output->insert(output->end(), value.begin(), value.end());
}

std::string_view pool_view(
    const std::pmr::vector<char>& pool,
    std::uint32_t offset,
    std::uint32_t length) noexcept {
    const std::size_t first = offset;
    const std::size_t count = length;
    if (first > pool.size() || count > pool.size() - first) {
        return {};
    }
    return std::string_view(pool.data() + first, count);
}

} // namespace

FontCatalogGeneration::FontCatalogGeneration(
    std::uint64_t generation_id,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit)
    : discovery_resource_(ledger_, core::ResourceClass::FontDiscoverySnapshot),
      catalog_resource_(ledger_, core::ResourceClass::FontCatalog),
      string_bytes_(&discovery_resource_),
      discovery_records_(&discovery_resource_),
      families_(&discovery_resource_),
      catalog_(&catalog_resource_),
      generation_id_(generation_id) {
    ledger_.set_hard_limit(
        core::ResourceClass::FontDiscoverySnapshot,
        discovery_hard_limit);
    ledger_.set_hard_limit(core::ResourceClass::FontCatalog, catalog_hard_limit);
}

std::uint64_t FontCatalogGeneration::generation_id() const noexcept {
    return generation_id_;
}

FontGenerationFingerprint FontCatalogGeneration::fingerprint() const noexcept {
    return fingerprint_;
}

const FontCatalog& FontCatalogGeneration::catalog() const noexcept {
    return catalog_;
}

std::span<const FontDiscoveryRecord>
FontCatalogGeneration::discovery_records() const noexcept {
    return discovery_records_;
}

std::span<const FontFamilyRecord>
FontCatalogGeneration::families() const noexcept {
    return families_;
}

std::string_view FontCatalogGeneration::identity(FontFaceId face_id) const noexcept {
    if (face_id >= discovery_records_.size()) {
        return {};
    }
    const FontDiscoveryRecord& record = discovery_records_[face_id];
    return pool_view(string_bytes_, record.identity_offset, record.identity_length);
}

std::string_view FontCatalogGeneration::family_name(
    std::uint32_t family_index) const noexcept {
    if (family_index >= families_.size()) {
        return {};
    }
    const FontFamilyRecord& family = families_[family_index];
    return pool_view(string_bytes_, family.name_offset, family.name_length);
}

core::ResourceSnapshot
FontCatalogGeneration::discovery_resource_snapshot() const noexcept {
    return ledger_.snapshot(core::ResourceClass::FontDiscoverySnapshot);
}

core::ResourceSnapshot
FontCatalogGeneration::catalog_resource_snapshot() const noexcept {
    return ledger_.snapshot(core::ResourceClass::FontCatalog);
}

bool FontCatalogGeneration::accounting_clean() const noexcept {
    return ledger_.accounting_clean();
}

bool FontCatalogGeneration::within_hard_limits() const noexcept {
    return ledger_.within_hard_limits();
}

const char* font_discovery_error_kind_name(FontDiscoveryErrorKind kind) noexcept {
    switch (kind) {
        case FontDiscoveryErrorKind::None:
            return "none";
        case FontDiscoveryErrorKind::InvalidGeneration:
            return "invalid_generation";
        case FontDiscoveryErrorKind::InvalidIdentity:
            return "invalid_identity";
        case FontDiscoveryErrorKind::InvalidFamily:
            return "invalid_family";
        case FontDiscoveryErrorKind::DuplicateIdentity:
            return "duplicate_identity";
        case FontDiscoveryErrorKind::IndexOverflow:
            return "index_overflow";
        case FontDiscoveryErrorKind::SnapshotBudgetExceeded:
            return "snapshot_budget_exceeded";
        case FontDiscoveryErrorKind::CatalogBuildFailed:
            return "catalog_build_failed";
    }
    return "invalid";
}

bool build_font_catalog_generation(
    std::uint64_t generation_id,
    std::span<const FontDiscoveryFace> faces,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit,
    std::shared_ptr<const FontCatalogGeneration>* output,
    FontDiscoveryStats* stats,
    FontDiscoveryError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->reset();
    *stats = {};
    clear_error(error);

    if (generation_id == 0U) {
        return fail(
            FontDiscoveryErrorKind::InvalidGeneration,
            0U,
            "font catalog generation identifiers must be non-zero",
            error);
    }
    if (faces.size() > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            FontDiscoveryErrorKind::IndexOverflow,
            0U,
            "font discovery face count exceeds the 32-bit catalog contract",
            error);
    }

    try {
        auto candidate = std::make_shared<FontCatalogGeneration>(
            generation_id,
            discovery_hard_limit,
            catalog_hard_limit);

        {
            std::pmr::vector<std::size_t> order(
                &candidate->discovery_resource_);
            std::pmr::vector<std::string_view> family_names(
                &candidate->discovery_resource_);
            order.reserve(faces.size());
            family_names.reserve(faces.size());

            std::size_t identity_bytes = 0U;
            for (std::size_t index = 0U; index < faces.size(); ++index) {
                const FontDiscoveryFace& face = faces[index];
                if (!valid_utf8_scalar_sequence(face.platform_identity)) {
                    return fail(
                        FontDiscoveryErrorKind::InvalidIdentity,
                        index,
                        "font platform identity must be non-empty, NUL-free UTF-8",
                        error);
                }
                if (!valid_utf8_scalar_sequence(face.family_name)) {
                    return fail(
                        FontDiscoveryErrorKind::InvalidFamily,
                        index,
                        "font family name must be non-empty, NUL-free UTF-8",
                        error);
                }
                if (!checked_add(
                        identity_bytes,
                        face.platform_identity.size(),
                        &identity_bytes)) {
                    return fail(
                        FontDiscoveryErrorKind::IndexOverflow,
                        index,
                        "font identity byte count overflowed",
                        error);
                }
                order.push_back(index);
                family_names.push_back(face.family_name);
            }

            std::sort(
                order.begin(),
                order.end(),
                [&faces](std::size_t left, std::size_t right) {
                    return faces[left].platform_identity <
                           faces[right].platform_identity;
                });
            for (std::size_t position = 1U; position < order.size(); ++position) {
                if (faces[order[position - 1U]].platform_identity ==
                    faces[order[position]].platform_identity) {
                    return fail(
                        FontDiscoveryErrorKind::DuplicateIdentity,
                        order[position],
                        "font platform identities must be unique within a generation",
                        error);
                }
            }

            std::sort(family_names.begin(), family_names.end());
            family_names.erase(
                std::unique(family_names.begin(), family_names.end()),
                family_names.end());
            if (family_names.size() > static_cast<std::size_t>(
                                          std::numeric_limits<std::uint32_t>::max())) {
                return fail(
                    FontDiscoveryErrorKind::IndexOverflow,
                    0U,
                    "font family count exceeds the 32-bit generation contract",
                    error);
            }

            std::size_t family_bytes = 0U;
            for (const std::string_view family : family_names) {
                if (!checked_add(family_bytes, family.size(), &family_bytes)) {
                    return fail(
                        FontDiscoveryErrorKind::IndexOverflow,
                        0U,
                        "font family byte count overflowed",
                        error);
                }
            }
            std::size_t pool_bytes = 0U;
            if (!checked_add(identity_bytes, family_bytes, &pool_bytes) ||
                pool_bytes > static_cast<std::size_t>(
                                 std::numeric_limits<std::uint32_t>::max())) {
                return fail(
                    FontDiscoveryErrorKind::IndexOverflow,
                    0U,
                    "font generation string pool exceeds the 32-bit contract",
                    error);
            }

            candidate->string_bytes_.reserve(pool_bytes);
            candidate->discovery_records_.reserve(faces.size());
            candidate->families_.reserve(family_names.size());

            std::pmr::vector<FontFaceSeed> seeds(
                &candidate->discovery_resource_);
            seeds.reserve(faces.size());
            FingerprintBuilder fingerprint;
            fingerprint.add_u64(static_cast<std::uint64_t>(faces.size()));

            for (std::size_t position = 0U; position < order.size(); ++position) {
                const FontDiscoveryFace& face = faces[order[position]];
                const auto family_iterator = std::lower_bound(
                    family_names.begin(),
                    family_names.end(),
                    face.family_name);
                if (family_iterator == family_names.end() ||
                    *family_iterator != face.family_name) {
                    return fail(
                        FontDiscoveryErrorKind::InvalidFamily,
                        order[position],
                        "font family interning diverged",
                        error);
                }
                const std::size_t family_index = static_cast<std::size_t>(
                    family_iterator - family_names.begin());
                const std::size_t identity_offset =
                    candidate->string_bytes_.size();
                append_bytes(&candidate->string_bytes_, face.platform_identity);
                candidate->discovery_records_.push_back(FontDiscoveryRecord{
                    static_cast<std::uint32_t>(identity_offset),
                    static_cast<std::uint32_t>(face.platform_identity.size()),
                    static_cast<std::uint32_t>(family_index),
                    static_cast<FontFaceId>(position)});

                const std::uint64_t stable_key =
                    static_cast<std::uint64_t>(position) + 1U;
                const std::uint32_t family_key =
                    static_cast<std::uint32_t>(family_index) + 1U;
                seeds.push_back(FontFaceSeed{
                    stable_key,
                    family_key,
                    face.weight,
                    face.width,
                    face.slant,
                    face.preferred_script,
                    face.flags,
                    face.coverage});

                fingerprint.add_string(face.platform_identity);
                fingerprint.add_string(face.family_name);
                fingerprint.add_u64(face.weight);
                fingerprint.add_u64(face.width);
                fingerprint.add_u64(static_cast<std::uint8_t>(face.slant));
                fingerprint.add_u64(
                    static_cast<std::uint16_t>(face.preferred_script));
                fingerprint.add_u64(face.flags);
                fingerprint.add_u64(
                    static_cast<std::uint64_t>(face.coverage.size()));
                for (const FontCoverageRange range : face.coverage) {
                    fingerprint.add_u64(range.first);
                    fingerprint.add_u64(range.last);
                }
            }

            for (std::size_t index = 0U; index < family_names.size(); ++index) {
                const std::string_view family = family_names[index];
                const std::size_t offset = candidate->string_bytes_.size();
                append_bytes(&candidate->string_bytes_, family);
                candidate->families_.push_back(FontFamilyRecord{
                    static_cast<std::uint32_t>(offset),
                    static_cast<std::uint32_t>(family.size()),
                    static_cast<std::uint32_t>(index) + 1U});
            }

            FontCatalogStats catalog_stats;
            FontCatalogError catalog_error;
            if (!build_font_catalog(
                    seeds,
                    &candidate->catalog_,
                    &catalog_stats,
                    &catalog_error)) {
                if (error != nullptr) {
                    error->catalog_error = catalog_error.kind;
                    error->face_index = catalog_error.face_index;
                    error->kind = FontDiscoveryErrorKind::CatalogBuildFailed;
                    try {
                        error->message = catalog_error.message;
                    } catch (...) {
                        error->message.clear();
                    }
                }
                return false;
            }

            candidate->fingerprint_ = fingerprint.finish();
            stats->input_faces = static_cast<std::uint64_t>(faces.size());
            stats->output_faces =
                static_cast<std::uint64_t>(candidate->catalog_.faces.size());
            stats->unique_families =
                static_cast<std::uint64_t>(candidate->families_.size());
            stats->identity_bytes = static_cast<std::uint64_t>(identity_bytes);
            stats->family_bytes = static_cast<std::uint64_t>(family_bytes);
            stats->catalog = catalog_stats;
        }

        stats->snapshot_bytes = static_cast<std::uint64_t>(
            candidate->discovery_resource_snapshot().current_bytes);
        if (!candidate->accounting_clean() || !candidate->within_hard_limits()) {
            return fail(
                FontDiscoveryErrorKind::SnapshotBudgetExceeded,
                0U,
                "font generation resource accounting is invalid",
                error);
        }
        *output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            FontDiscoveryErrorKind::SnapshotBudgetExceeded,
            0U,
            "font discovery snapshot exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            FontDiscoveryErrorKind::SnapshotBudgetExceeded,
            0U,
            "font discovery snapshot allocation failed",
            error);
    }
}

const char* font_generation_publish_result_name(
    FontGenerationPublishResult result) noexcept {
    switch (result) {
        case FontGenerationPublishResult::Published:
            return "published";
        case FontGenerationPublishResult::InvalidCandidate:
            return "invalid_candidate";
        case FontGenerationPublishResult::StaleGeneration:
            return "stale_generation";
        case FontGenerationPublishResult::IdenticalSnapshot:
            return "identical_snapshot";
    }
    return "invalid";
}

FontCatalogGenerationStore::FontCatalogGenerationStore() noexcept
    : current_(nullptr) {}

FontGenerationPublishResult FontCatalogGenerationStore::publish(
    std::shared_ptr<const FontCatalogGeneration> candidate) noexcept {
    if (candidate == nullptr || candidate->generation_id() == 0U ||
        !candidate->accounting_clean() || !candidate->within_hard_limits()) {
        return FontGenerationPublishResult::InvalidCandidate;
    }

    std::shared_ptr<const FontCatalogGeneration> observed =
        current_.load(std::memory_order_acquire);
    while (true) {
        if (observed != nullptr) {
            if (candidate->generation_id() <= observed->generation_id()) {
                return FontGenerationPublishResult::StaleGeneration;
            }
            if (candidate->fingerprint() == observed->fingerprint()) {
                return FontGenerationPublishResult::IdenticalSnapshot;
            }
        }
        if (current_.compare_exchange_weak(
                observed,
                candidate,
                std::memory_order_release,
                std::memory_order_acquire)) {
            return FontGenerationPublishResult::Published;
        }
    }
}

std::shared_ptr<const FontCatalogGeneration>
FontCatalogGenerationStore::snapshot() const noexcept {
    return current_.load(std::memory_order_acquire);
}

} // namespace zevryon::text
