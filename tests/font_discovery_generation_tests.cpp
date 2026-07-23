#include "font_discovery_generation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>

namespace {

using namespace zevryon::text;

constexpr std::size_t kDiscoveryLimit = 64U * 1024U;
constexpr std::size_t kCatalogLimit = 64U * 1024U;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

struct FaceData {
    std::array<FontCoverageRange, 2> alpha_coverage{{
        {0x0020U, 0x007eU},
        {0x0300U, 0x036fU},
    }};
    std::array<FontCoverageRange, 1> arabic_coverage{{
        {0x0600U, 0x06ffU},
    }};

    FontDiscoveryFace alpha() const {
        return {
            "adapter/font-alpha#0",
            "Zeta Sans",
            400U,
            5U,
            FontSlant::Upright,
            ScriptId::Latn,
            kFontFaceSystem,
            alpha_coverage,
        };
    }

    FontDiscoveryFace arabic() const {
        return {
            "adapter/font-arabic#1",
            "Alpha Arabic",
            500U,
            5U,
            FontSlant::Upright,
            ScriptId::Arab,
            0U,
            arabic_coverage,
        };
    }
};

bool build_generation(
    std::uint64_t generation_id,
    std::span<const FontDiscoveryFace> faces,
    std::shared_ptr<const FontCatalogGeneration>* output,
    FontDiscoveryStats* stats = nullptr,
    FontDiscoveryError* external_error = nullptr,
    std::size_t discovery_limit = kDiscoveryLimit,
    std::size_t catalog_limit = kCatalogLimit) {
    FontDiscoveryStats local_stats;
    FontDiscoveryError local_error;
    const bool result = build_font_catalog_generation(
        generation_id,
        faces,
        discovery_limit,
        catalog_limit,
        output,
        stats == nullptr ? &local_stats : stats,
        external_error == nullptr ? &local_error : external_error);
    return result;
}

bool deterministic_order_and_fingerprint() {
    const FaceData data;
    const std::array<FontDiscoveryFace, 2> first_order{{
        data.arabic(),
        data.alpha(),
    }};
    const std::array<FontDiscoveryFace, 2> second_order{{
        data.alpha(),
        data.arabic(),
    }};

    std::shared_ptr<const FontCatalogGeneration> first;
    std::shared_ptr<const FontCatalogGeneration> second;
    FontDiscoveryStats first_stats;
    FontDiscoveryStats second_stats;
    bool ok = expect(
        build_generation(1U, first_order, &first, &first_stats),
        "first enumeration order must build");
    ok &= expect(
        build_generation(2U, second_order, &second, &second_stats),
        "second enumeration order must build");
    if (!ok) {
        return false;
    }

    ok &= expect(first->fingerprint() == second->fingerprint(),
                 "enumeration order must not change semantic fingerprint");
    ok &= expect(first->identity(0U) == "adapter/font-alpha#0",
                 "canonical identity order must be lexical");
    ok &= expect(first->identity(1U) == "adapter/font-arabic#1",
                 "second canonical identity must be stable");
    ok &= expect(first->families().size() == 2U,
                 "two unique families are required");
    ok &= expect(first->family_name(0U) == "Alpha Arabic",
                 "family keys must use lexical canonical order");
    ok &= expect(first->family_name(1U) == "Zeta Sans",
                 "second family key must be stable");
    ok &= expect(first->catalog().faces.size() == 2U,
                 "catalog must contain every canonical face");
    ok &= expect(first->catalog().faces[0].stable_key == 1U &&
                     first->catalog().faces[1].stable_key == 2U,
                 "stable keys must be collision-free one-based canonical indices");
    ok &= expect(first->catalog().faces[0].family_key == 2U &&
                     first->catalog().faces[1].family_key == 1U,
                 "family keys must follow canonical family order");
    ok &= expect(first_stats.snapshot_bytes > 0U,
                 "snapshot accounting must include persistent discovery data");
    ok &= expect(first->accounting_clean() && first->within_hard_limits(),
                 "generation accounting must remain clean and bounded");
    ok &= expect(first_stats.input_faces == 2U &&
                     first_stats.output_faces == 2U &&
                     first_stats.unique_families == 2U,
                 "generation statistics must match canonical output");
    return ok;
}

bool invalid_identity_and_duplicate_rejection() {
    const FaceData data;
    const FontDiscoveryFace alpha = data.alpha();
    const std::array<FontDiscoveryFace, 2> duplicates{{alpha, alpha}};
    std::shared_ptr<const FontCatalogGeneration> output;
    FontDiscoveryError error;
    bool ok = expect(
        !build_generation(1U, duplicates, &output, nullptr, &error),
        "duplicate platform identities must fail");
    ok &= expect(error.kind == FontDiscoveryErrorKind::DuplicateIdentity,
                 "duplicate identity must have a dedicated error");
    ok &= expect(output == nullptr,
                 "duplicate identity failure must publish no generation");

    const std::string invalid_utf8("\xc0\xaf", 2U);
    FontDiscoveryFace invalid = alpha;
    invalid.platform_identity = invalid_utf8;
    const std::array<FontDiscoveryFace, 1> invalid_faces{{invalid}};
    error = {};
    ok &= expect(
        !build_generation(2U, invalid_faces, &output, nullptr, &error),
        "overlong UTF-8 identity must fail");
    ok &= expect(error.kind == FontDiscoveryErrorKind::InvalidIdentity,
                 "invalid UTF-8 must report invalid identity");

    const std::string nul_family("Bad\0Family", 10U);
    invalid = alpha;
    invalid.family_name = nul_family;
    const std::array<FontDiscoveryFace, 1> nul_faces{{invalid}};
    error = {};
    ok &= expect(
        !build_generation(3U, nul_faces, &output, nullptr, &error),
        "embedded NUL family name must fail");
    ok &= expect(error.kind == FontDiscoveryErrorKind::InvalidFamily,
                 "embedded NUL must report invalid family");
    return ok;
}

bool budget_failures_are_atomic() {
    const FaceData data;
    const std::array<FontDiscoveryFace, 2> faces{{data.alpha(), data.arabic()}};
    std::shared_ptr<const FontCatalogGeneration> output;
    FontDiscoveryError error;
    bool ok = expect(
        !build_generation(1U, faces, &output, nullptr, &error, 32U, kCatalogLimit),
        "tiny discovery budget must fail");
    ok &= expect(error.kind == FontDiscoveryErrorKind::SnapshotBudgetExceeded,
                 "discovery budget failure must be explicit");
    ok &= expect(output == nullptr,
                 "discovery budget failure must publish no generation");

    error = {};
    ok &= expect(
        !build_generation(2U, faces, &output, nullptr, &error, kDiscoveryLimit, 32U),
        "tiny catalog budget must fail");
    ok &= expect(error.kind == FontDiscoveryErrorKind::CatalogBuildFailed,
                 "catalog budget failure must propagate atomically");
    ok &= expect(error.catalog_error == FontCatalogErrorKind::OutputBudgetExceeded,
                 "nested catalog budget error must be retained");
    ok &= expect(output == nullptr,
                 "catalog build failure must publish no generation");
    return ok;
}

bool fingerprint_changes_with_semantics() {
    const FaceData data;
    const std::array<FontDiscoveryFace, 1> original{{data.alpha()}};
    FontDiscoveryFace changed_face = data.alpha();
    changed_face.weight = 700U;
    const std::array<FontDiscoveryFace, 1> changed{{changed_face}};

    std::shared_ptr<const FontCatalogGeneration> first;
    std::shared_ptr<const FontCatalogGeneration> second;
    bool ok = expect(build_generation(1U, original, &first),
                     "original fingerprint fixture must build");
    ok &= expect(build_generation(2U, changed, &second),
                 "changed fingerprint fixture must build");
    ok &= expect(first->fingerprint() != second->fingerprint(),
                 "semantic style changes must change fingerprint");
    return ok;
}

bool generation_store_publication() {
    const FaceData data;
    const std::array<FontDiscoveryFace, 2> faces{{data.alpha(), data.arabic()}};
    FontDiscoveryFace changed_alpha = data.alpha();
    changed_alpha.weight = 600U;
    const std::array<FontDiscoveryFace, 2> changed{{changed_alpha, data.arabic()}};

    std::shared_ptr<const FontCatalogGeneration> first;
    std::shared_ptr<const FontCatalogGeneration> identical_newer;
    std::shared_ptr<const FontCatalogGeneration> changed_newer;
    bool ok = expect(build_generation(1U, faces, &first),
                     "first store generation must build");
    ok &= expect(build_generation(2U, faces, &identical_newer),
                 "identical newer generation must build");
    ok &= expect(build_generation(3U, changed, &changed_newer),
                 "changed newer generation must build");
    if (!ok) {
        return false;
    }

    FontCatalogGenerationStore store;
    ok &= expect(
        store.publish(nullptr) == FontGenerationPublishResult::InvalidCandidate,
        "null candidates must be rejected");
    ok &= expect(
        store.publish(first) == FontGenerationPublishResult::Published,
        "first generation must publish");
    const std::shared_ptr<const FontCatalogGeneration> retained = store.snapshot();
    ok &= expect(retained != nullptr && retained->generation_id() == 1U,
                 "reader snapshot must observe first generation");
    ok &= expect(
        store.publish(identical_newer) ==
            FontGenerationPublishResult::IdenticalSnapshot,
        "identical semantic snapshot must avoid invalidation");
    ok &= expect(store.snapshot()->generation_id() == 1U,
                 "identical publish must retain current generation");
    ok &= expect(
        store.publish(changed_newer) == FontGenerationPublishResult::Published,
        "changed newer generation must publish");
    ok &= expect(store.snapshot()->generation_id() == 3U,
                 "store must expose changed generation");
    ok &= expect(
        store.publish(first) == FontGenerationPublishResult::StaleGeneration,
        "older generation must be rejected");
    ok &= expect(retained->identity(0U) == "adapter/font-alpha#0",
                 "old reader snapshot must remain alive after swap");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= deterministic_order_and_fingerprint();
    ok &= invalid_identity_and_duplicate_rejection();
    ok &= budget_failures_are_atomic();
    ok &= fingerprint_changes_with_semantics();
    ok &= generation_store_publication();
    if (!ok) {
        return 1;
    }
    std::cout << "font discovery generation tests passed\n";
    return 0;
}
