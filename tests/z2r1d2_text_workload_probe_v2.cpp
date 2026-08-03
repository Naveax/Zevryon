#include "bidi_explicit.hpp"
#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "script_run.hpp"
#include "unicode_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;

constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& value, std::uint64_t input) noexcept {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        value ^= static_cast<std::uint8_t>((input >> shift) & 0xffU);
        value *= kPrime;
    }
}

void mix_text(std::uint64_t& value, std::string_view text) noexcept {
    for (const char character : text) {
        value ^= static_cast<std::uint8_t>(character);
        value *= kPrime;
    }
}

std::string hex64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::vector<std::byte> fixture(std::size_t bytes) {
    const std::vector<std::uint8_t> pattern{
        'Z', 'e', 'v', 'r', 'y', 'o', 'n', ' ',
        0xc5U, 0x9fU, 0xc4U, 0x9fU, 0xc4U, 0xb1U, ' ',
        'e', 0xccU, 0x81U, ' ',
        0xe4U, 0xb8U, 0xadU, ' ',
        0xd7U, 0xa9U, ' ', 0xd8U, 0xb9U, ' ',
        0xf0U, 0x9fU, 0x91U, 0xa9U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x9aU, 0x80U, ' ',
        0xe2U, 0x81U, 0xa6U, 'a', 'b', 0xe2U, 0x81U, 0xa9U, ' ',
        0xe2U, 0x81U, 0xa7U, 0xd7U, 0x90U, 0xd7U, 0x91U,
        0xe2U, 0x81U, 0xa9U, ' ',
    };
    std::vector<std::byte> output;
    output.reserve(bytes);
    while (output.size() < bytes) {
        const std::size_t count = std::min(bytes - output.size(), pattern.size());
        for (std::size_t index = 0U; index < count; ++index) {
            output.push_back(static_cast<std::byte>(pattern[index]));
        }
    }
    std::size_t suffix = output.size();
    while (suffix > 0U) {
        const auto byte = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(output[suffix - 1U]));
        if ((byte & 0xc0U) != 0x80U) {
            break;
        }
        output[suffix - 1U] = static_cast<std::byte>('x');
        --suffix;
    }
    if (suffix > 0U) {
        const auto byte = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(output[suffix - 1U]));
        if (byte >= 0xc2U) {
            output[suffix - 1U] = static_cast<std::byte>('x');
        }
    }
    return output;
}

struct Evidence {
    std::string name;
    double wall_ms{0.0};
    std::uint64_t input_items{0};
    std::uint64_t output_items{0};
    std::uint64_t checksum{kOffset};
    std::size_t current_bytes{0};
    std::size_t peak_bytes{0};
    std::uint64_t operations{0};
    std::uint64_t verifications{0};
    std::uint64_t mismatches{0};
    bool enabled{false};
    bool exact{false};
    bool healthy{false};
    bool within_limits{false};
    bool accounting_clean{false};
    std::string shadow;
};

bool certify(ResourceLedger& ledger, Evidence& evidence) {
    evidence.enabled = ledger.rust_shadow_enabled();
    evidence.exact = !evidence.enabled || ledger.verify_rust_shadow();
    evidence.healthy = !evidence.enabled || ledger.rust_shadow_healthy();
    evidence.operations = ledger.rust_shadow_operations();
    evidence.verifications = ledger.rust_shadow_verifications();
    evidence.mismatches = ledger.rust_shadow_mismatches();
    evidence.current_bytes = ledger.total_current_bytes();
    evidence.peak_bytes = ledger.total_peak_bytes();
    evidence.within_limits = ledger.within_hard_limits();
    evidence.accounting_clean = ledger.accounting_clean();
    evidence.shadow = ledger.rust_shadow_json();
    return evidence.exact && evidence.healthy && evidence.mismatches == 0U &&
        evidence.current_bytes == 0U && evidence.within_limits &&
        evidence.accounting_clean && (!evidence.enabled || evidence.operations > 0U);
}

void write_json(const Evidence& evidence) {
    std::cout << "{\"name\":\"" << evidence.name << "\""
              << ",\"wall_ms\":" << evidence.wall_ms
              << ",\"input_items\":" << evidence.input_items
              << ",\"output_items\":" << evidence.output_items
              << ",\"semantic_checksum\":\"" << hex64(evidence.checksum) << "\""
              << ",\"current_bytes\":" << evidence.current_bytes
              << ",\"peak_bytes\":" << evidence.peak_bytes
              << ",\"within_hard_limits\":" << (evidence.within_limits ? "true" : "false")
              << ",\"accounting_clean\":" << (evidence.accounting_clean ? "true" : "false")
              << ",\"shadow_enabled\":" << (evidence.enabled ? "true" : "false")
              << ",\"shadow_exact\":" << (evidence.exact ? "true" : "false")
              << ",\"shadow_healthy\":" << (evidence.healthy ? "true" : "false")
              << ",\"shadow_operations\":" << evidence.operations
              << ",\"shadow_verifications\":" << evidence.verifications
              << ",\"shadow_mismatches\":" << evidence.mismatches
              << ",\"shadow\":" << evidence.shadow << '}';
}
} // namespace

int main() {
    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    const auto bytes = fixture(kFixtureBytes);
    std::vector<zevryon::text::DecodedCodePoint> codepoints;
    std::vector<zevryon::text::GraphemeBoundary> boundaries;
    std::vector<Evidence> stages;
    stages.reserve(4U);
    std::uint64_t pipeline = kOffset;

    {
        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::UnicodeBuffer, 4U * 1024U * 1024U);
        Evidence evidence{.name = "unicode", .input_items = bytes.size()};
        const auto started = std::chrono::steady_clock::now();
        {
            LedgerMemoryResource memory(ledger, ResourceClass::UnicodeBuffer);
            std::pmr::vector<zevryon::text::DecodedCodePoint> output(&memory);
            zevryon::text::Utf8StreamDecoder decoder(zevryon::text::Utf8ErrorPolicy::Strict);
            zevryon::text::Utf8DecodeError error;
            for (std::size_t offset = 0U; offset < bytes.size();) {
                const std::size_t count = std::min<std::size_t>(4096U, bytes.size() - offset);
                if (!decoder.feed(
                        std::span<const std::byte>(bytes.data() + offset, count),
                        static_cast<std::uint64_t>(offset), &output, &error)) {
                    std::cerr << "Unicode decode failed: " << error.message << '\n';
                    return 1;
                }
                offset += count;
            }
            if (!decoder.finish(&output, &error) || output.empty()) {
                std::cerr << "Unicode finish failed: " << error.message << '\n';
                return 1;
            }
            codepoints.assign(output.begin(), output.end());
        }
        evidence.wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        evidence.output_items = codepoints.size();
        for (const auto& item : codepoints) {
            mix(evidence.checksum, item.source_start);
            mix(evidence.checksum, item.value);
            mix(evidence.checksum, item.source_length);
        }
        if (!certify(ledger, evidence)) {
            std::cerr << "Unicode ledger certification failed\n";
            return 1;
        }
        mix_text(pipeline, evidence.name);
        mix(pipeline, evidence.checksum);
        stages.push_back(std::move(evidence));
    }

    {
        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::GraphemeCluster, 2U * 1024U * 1024U);
        Evidence evidence{.name = "grapheme", .input_items = codepoints.size()};
        zevryon::text::GraphemeSegmentStats stats;
        const auto started = std::chrono::steady_clock::now();
        {
            LedgerMemoryResource memory(ledger, ResourceClass::GraphemeCluster);
            std::pmr::vector<zevryon::text::GraphemeBoundary> output(&memory);
            zevryon::text::GraphemeError error;
            if (!zevryon::text::segment_graphemes(codepoints, &output, &stats, &error)) {
                std::cerr << "Grapheme segmentation failed: " << error.message << '\n';
                return 1;
            }
            boundaries.assign(output.begin(), output.end());
        }
        evidence.wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        evidence.output_items = stats.output_clusters;
        for (const auto& item : boundaries) {
            mix(evidence.checksum, item.source_offset);
            mix(evidence.checksum, item.codepoint_index);
        }
        mix(evidence.checksum, stats.suppressed_breaks);
        if (!certify(ledger, evidence)) {
            std::cerr << "Grapheme ledger certification failed\n";
            return 1;
        }
        mix_text(pipeline, evidence.name);
        mix(pipeline, evidence.checksum);
        stages.push_back(std::move(evidence));
    }

    {
        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::ScriptRun, 1U * 1024U * 1024U);
        Evidence evidence{
            .name = "script",
            .input_items = boundaries.empty() ? 0U : boundaries.size() - 1U,
        };
        zevryon::text::ScriptRunStats stats;
        const auto started = std::chrono::steady_clock::now();
        {
            LedgerMemoryResource memory(ledger, ResourceClass::ScriptRun);
            std::pmr::vector<zevryon::text::ScriptRunBoundary> output(&memory);
            zevryon::text::ScriptRunError error;
            if (!zevryon::text::resolve_script_runs(
                    codepoints, boundaries, &output, &stats, &error)) {
                std::cerr << "Script resolution failed: " << error.message << '\n';
                return 1;
            }
            for (const auto& item : output) {
                mix(evidence.checksum, item.source_offset);
                mix(evidence.checksum, item.cluster_index);
                mix(evidence.checksum, static_cast<std::uint64_t>(item.script));
            }
        }
        evidence.wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        evidence.output_items = stats.output_runs;
        mix(evidence.checksum, stats.neutral_clusters);
        mix(evidence.checksum, stats.explicit_extension_lookups);
        if (!certify(ledger, evidence)) {
            std::cerr << "Script ledger certification failed\n";
            return 1;
        }
        mix_text(pipeline, evidence.name);
        mix(pipeline, evidence.checksum);
        stages.push_back(std::move(evidence));
    }

    {
        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::BidiRun, 2U * 1024U * 1024U);
        Evidence evidence{.name = "bidi", .input_items = codepoints.size()};
        zevryon::text::BidiExplicitStats stats;
        const auto started = std::chrono::steady_clock::now();
        {
            LedgerMemoryResource memory(ledger, ResourceClass::BidiRun);
            std::pmr::vector<zevryon::text::BidiExplicitUnit> output(&memory);
            zevryon::text::BidiExplicitError error;
            if (!zevryon::text::resolve_bidi_explicit(
                    codepoints,
                    zevryon::text::BidiParagraphDirection::Auto,
                    &output, &stats, &error)) {
                std::cerr << "Bidi resolution failed: " << error.message << '\n';
                return 1;
            }
            for (const auto& item : output) {
                mix(evidence.checksum, item.source_offset);
                mix(evidence.checksum, item.codepoint_index);
                mix(evidence.checksum, static_cast<std::uint64_t>(item.original_class));
                mix(evidence.checksum, static_cast<std::uint64_t>(item.resolved_class));
                mix(evidence.checksum, item.level);
                mix(evidence.checksum, item.flags);
            }
        }
        evidence.wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        evidence.output_items = stats.output_units;
        mix(evidence.checksum, stats.explicit_controls);
        mix(evidence.checksum, stats.isolate_initiators);
        mix(evidence.checksum, stats.fsi_resolutions);
        if (!certify(ledger, evidence)) {
            std::cerr << "Bidi ledger certification failed\n";
            return 1;
        }
        mix_text(pipeline, evidence.name);
        mix(pipeline, evidence.checksum);
        stages.push_back(std::move(evidence));
    }

    const bool enabled = std::all_of(stages.begin(), stages.end(), [](const Evidence& item) {
        return item.enabled;
    });
    const bool disabled = std::all_of(stages.begin(), stages.end(), [](const Evidence& item) {
        return !item.enabled;
    });
    if (!enabled && !disabled) {
        std::cerr << "Mixed Rust-shadow state\n";
        return 1;
    }

    std::uint64_t operations = 0U;
    std::uint64_t verifications = 0U;
    std::uint64_t mismatches = 0U;
    std::size_t peak_bytes = 0U;
    double wall_ms = 0.0;
    for (const auto& item : stages) {
        operations += item.operations;
        verifications += item.verifications;
        mismatches += item.mismatches;
        peak_bytes += item.peak_bytes;
        wall_ms += item.wall_ms;
    }

    std::cout << std::fixed << std::setprecision(6)
              << "{\"schema\":\"zevryon.rust-shadow-text-probe.v1\""
              << ",\"fixture_bytes\":" << bytes.size()
              << ",\"pipeline_checksum\":\"" << hex64(pipeline) << "\""
              << ",\"wall_ms\":" << wall_ms
              << ",\"rust_shadow_enabled\":" << (enabled ? "true" : "false")
              << ",\"exact_verification\":true"
              << ",\"rust_shadow_operations\":" << operations
              << ",\"rust_shadow_verifications\":" << verifications
              << ",\"rust_shadow_mismatches\":" << mismatches
              << ",\"total_peak_bytes\":" << peak_bytes
              << ",\"stages\":[";
    for (std::size_t index = 0U; index < stages.size(); ++index) {
        if (index != 0U) {
            std::cout << ',';
        }
        write_json(stages[index]);
    }
    std::cout << "]}\n";
    return 0;
}
