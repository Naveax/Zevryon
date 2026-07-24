#include "verified_font_resource_cache.hpp"

#include <memory>
#include <span>

namespace zevryon::text {

VerifiedFontResourceCacheKey verified_font_cache_key(
    FontContentIdentity identity) noexcept {
    return VerifiedFontResourceCacheKey{
        identity.high,
        identity.low,
        identity.face_index};
}

bool VerifiedFontResourceCache::get_or_build_content_addressed(
    std::span<const std::byte> source,
    std::uint32_t face_index,
    std::shared_ptr<const VerifiedFontResource>* output,
    FontContentIdentity* identity,
    VerifiedFontResourceCacheStats* stats,
    VerifiedFontResourceCacheError* error) noexcept {
    if (identity != nullptr) {
        *identity = {};
    }

    FontContentIdentity computed;
    if (!compute_font_content_identity(source, face_index, &computed)) {
        if (output != nullptr) {
            output->reset();
        }
        if (stats != nullptr) {
            *stats = snapshot();
        }
        if (error != nullptr) {
            error->kind = VerifiedFontResourceCacheErrorKind::InvalidArgument;
            error->resource_error = VerifiedFontResourceErrorKind::None;
            error->byte_offset = 0U;
            error->table_tag = 0U;
            error->parse_error = SfntParseErrorKind::None;
            error->integrity_error = SfntIntegrityErrorKind::None;
            try {
                error->message = "font content identity computation failed";
            } catch (...) {
                error->message.clear();
            }
        }
        return false;
    }

    if (identity != nullptr) {
        *identity = computed;
    }
    return get_or_build(
        verified_font_cache_key(computed),
        source,
        output,
        stats,
        error);
}

} // namespace zevryon::text
