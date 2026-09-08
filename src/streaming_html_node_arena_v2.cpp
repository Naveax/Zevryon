#include "streaming_html_node_arena_v2.hpp"

#include <system_error>
#include <utility>

namespace zevryon::massivedoc {
namespace {

bool fail_pipeline(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool source_parent_is_inside_store(
    const std::filesystem::path& store_root,
    const std::filesystem::path& source_path,
    bool* inside,
    std::string* error) {
    if (inside == nullptr || error == nullptr) {
        return false;
    }
    *inside = false;

    std::error_code filesystem_error;
    const std::filesystem::path canonical_store =
        std::filesystem::canonical(store_root, filesystem_error);
    if (filesystem_error) {
        return fail_pipeline(
            error,
            "failed to resolve authoritative native store path: " +
                filesystem_error.message());
    }

    filesystem_error.clear();
    const std::filesystem::path absolute_source =
        std::filesystem::absolute(source_path, filesystem_error);
    if (filesystem_error) {
        return fail_pipeline(
            error,
            "failed to resolve HTML v2 source path: " +
                filesystem_error.message());
    }

    filesystem_error.clear();
    const std::filesystem::path canonical_parent =
        std::filesystem::canonical(absolute_source.parent_path(), filesystem_error);
    if (filesystem_error) {
        return fail_pipeline(
            error,
            "failed to resolve HTML v2 source parent directory: " +
                filesystem_error.message());
    }

    for (std::filesystem::path current = canonical_parent;;) {
        filesystem_error.clear();
        const bool equivalent =
            std::filesystem::equivalent(current, canonical_store, filesystem_error);
        if (filesystem_error) {
            return fail_pipeline(
                error,
                "failed to compare HTML v2 source path with native store: " +
                    filesystem_error.message());
        }
        if (equivalent) {
            *inside = true;
            return true;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
    return true;
}

bool rollback_published_source(
    const std::filesystem::path& source_path,
    std::string* cleanup_error) {
    if (cleanup_error == nullptr) {
        return false;
    }
    cleanup_error->clear();

    std::error_code error;
    const bool removed = std::filesystem::remove(source_path, error);
    if (error) {
        *cleanup_error =
            "failed to roll back published HTML v2 source: " + error.message();
        return false;
    }
    if (!removed) {
        std::error_code exists_error;
        const bool still_exists = std::filesystem::exists(source_path, exists_error);
        if (exists_error) {
            *cleanup_error =
                "failed to verify HTML v2 source rollback: " + exists_error.message();
            return false;
        }
        if (still_exists) {
            *cleanup_error = "published HTML v2 source survived rollback";
            return false;
        }
    }

    // A successful producer publication should already have removed staging.
    // Remove a stale sibling only as defensive cleanup for this owned target.
    std::filesystem::path building = source_path;
    building += ".building";
    error.clear();
    std::filesystem::remove(building, error);
    if (error) {
        *cleanup_error =
            "failed to clean HTML v2 source staging during rollback: " +
            error.message();
        return false;
    }
    return true;
}

} // namespace

bool build_streaming_html_node_arena_v2(
    const std::filesystem::path& store_root,
    const std::filesystem::path& source_path,
    StreamingHtmlNodeArenaV2Config config,
    StreamingHtmlNodeArenaV2Stats* stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();

    StreamingHtmlNodeArenaV2Stats local_stats{};
    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (store_root.empty() || source_path.empty()) {
        return fail_pipeline(error, "HTML v2 arena pipeline paths must not be empty");
    }

    bool source_inside_store = false;
    if (!source_parent_is_inside_store(
            store_root, source_path, &source_inside_store, error)) {
        return false;
    }
    if (source_inside_store) {
        return fail_pipeline(
            error,
            "HTML v2 source artifact must reside outside the authoritative native store tree");
    }

    if (!produce_streaming_html_node_source_v2(
            store_root,
            source_path,
            config.parser,
            &local_stats.parser,
            error)) {
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return false;
    }
    local_stats.source_published = true;

    if (!import_logical_node_source_v2_to_arena_v2(
            source_path,
            store_root,
            std::move(config.import),
            &local_stats.import,
            error)) {
        const std::string import_error = *error;
        std::string cleanup_error;
        if (rollback_published_source(source_path, &cleanup_error)) {
            local_stats.source_published = false;
            *error = import_error;
        } else {
            *error = import_error + "; " + cleanup_error;
        }
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return false;
    }

    local_stats.arena_published = true;
    if (stats != nullptr) {
        *stats = local_stats;
    }
    return true;
}

} // namespace zevryon::massivedoc
