#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"
#include "streaming_html_node_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeSourceNode;
using zevryon::massivedoc::LogicalNodeSourceReader;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::StreamingHtmlNodeSourceStats;
using zevryon::massivedoc::produce_streaming_html_node_source;

constexpr std::string_view kEnvelopeMismatch =
    "HTML parser node count disagrees with native store logical_nodes metadata";

struct RootCleanup {
    explicit RootCleanup(std::filesystem::path value) : root(std::move(value)) {}
    ~RootCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    RootCleanup(const RootCleanup&) = delete;
    RootCleanup& operator=(const RootCleanup&) = delete;
    std::filesystem::path root;
};

std::filesystem::path unique_root() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-z7-tree-dump-") + std::to_string(tick));
}

std::span<const std::byte> bytes(std::string_view value) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(value.data()), value.size());
}

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool decode_hex(std::string_view value, std::string* output) {
    if ((value.size() & 1U) != 0U) {
        return false;
    }
    output->clear();
    output->reserve(value.size() / 2U);
    for (std::size_t index = 0U; index < value.size(); index += 2U) {
        const int high = hex_value(value[index]);
        const int low = hex_value(value[index + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output->push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::string encode_hex(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.resize(value.size() * 2U);
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const unsigned byte = static_cast<unsigned char>(value[index]);
        output[index * 2U] = digits[(byte >> 4U) & 0xFU];
        output[index * 2U + 1U] = digits[byte & 0xFU];
    }
    return output;
}

bool build_store(
    const std::filesystem::path& root,
    std::string_view html,
    std::uint64_t logical_nodes,
    std::string* error) {
    StoreWriter writer(root);
    if (!writer.append(1U, bytes(html), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = static_cast<std::uint64_t>(html.size());
    metadata.logical_records = 1U;
    metadata.logical_nodes = logical_nodes;
    metadata.largest_record_bytes = static_cast<std::uint64_t>(html.size());
    return writer.finalize(metadata, nullptr, error);
}

bool collect_nodes(
    const std::filesystem::path& source_path,
    std::vector<LogicalNodeSourceNode>* nodes,
    std::string* error) {
    LogicalNodeSourceReader reader(source_path);
    if (!reader.open(error)) {
        return false;
    }
    nodes->clear();
    for (;;) {
        LogicalNodeSourceNode node;
        bool has_node = false;
        if (!reader.next(&node, &has_node, error)) {
            return false;
        }
        if (!has_node) {
            break;
        }
        nodes->push_back(std::move(node));
    }
    return true;
}

bool serialize_wpt_tree(
    const std::vector<LogicalNodeSourceNode>& nodes,
    std::vector<std::string>* lines,
    std::string* error) {
    if (nodes.empty() || nodes.front().tag != "#document") {
        *error = "production node source is missing the #document root";
        return false;
    }
    lines->clear();
    std::vector<std::uint64_t> depth(nodes.size(), 0U);
    for (std::size_t index = 1U; index < nodes.size(); ++index) {
        const auto parent = nodes[index].parent_ordinal;
        if (parent >= index || parent >= nodes.size()) {
            *error = "production node source contains invalid parent topology";
            return false;
        }
        depth[index] = parent == 0U ? 0U : depth[static_cast<std::size_t>(parent)] + 1U;
        std::string line = "| ";
        line.append(static_cast<std::size_t>(depth[index] * 2U), ' ');
        line.push_back('<');
        line += nodes[index].tag;
        line.push_back('>');
        lines->push_back(std::move(line));

        std::vector<std::pair<std::string, std::string>> attributes;
        attributes.reserve(nodes[index].attributes.size());
        for (const auto& attribute : nodes[index].attributes) {
            attributes.emplace_back(attribute.name, attribute.value);
        }
        std::sort(attributes.begin(), attributes.end());
        for (const auto& [name, value] : attributes) {
            std::string attribute_line = "| ";
            attribute_line.append(static_cast<std::size_t>((depth[index] + 1U) * 2U), ' ');
            attribute_line += name;
            attribute_line += "=\"";
            attribute_line += value;
            attribute_line.push_back('"');
            lines->push_back(std::move(attribute_line));
        }
    }
    return true;
}

int unsupported(std::string_view reason) {
    std::cout << "UNSUPPORTED\t" << encode_hex(reason) << '\n';
    return 2;
}

int internal_failure(std::string_view reason) {
    std::cout << "FAIL\t" << encode_hex(reason) << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        return internal_failure("usage: html_tree_dump_v1_probe <scripting:0|1> <html-hex>");
    }
    const std::string_view scripting = argv[1];
    if (scripting != "0" && scripting != "1") {
        return internal_failure("scripting mode must be 0 or 1");
    }
    std::string html;
    if (!decode_hex(argv[2], &html)) {
        return internal_failure("HTML argument is not valid hexadecimal");
    }

    const std::filesystem::path root = unique_root();
    RootCleanup cleanup(root);
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        return internal_failure("cannot create tree-dump temporary root");
    }

    std::string error;
    const auto discovery_store = root / "discovery-store";
    const auto discovery_source = root / "discovery.zvnsrc";
    if (!build_store(discovery_store, html, 1U, &error)) {
        return internal_failure(error);
    }

    StreamingHtmlNodeSourceStats discovery_stats;
    const bool discovery_ok = produce_streaming_html_node_source(
        discovery_store, discovery_source, {}, &discovery_stats, &error);

    std::uint64_t exact_nodes = 0U;
    if (discovery_ok) {
        exact_nodes = 1U;
    } else if (error == kEnvelopeMismatch && discovery_stats.nodes_emitted != 0U) {
        exact_nodes = discovery_stats.nodes_emitted;
    } else {
        return unsupported(error.empty() ? "production tree builder failed closed" : error);
    }

    std::filesystem::path source_path = discovery_source;
    if (!discovery_ok) {
        const auto exact_store = root / "exact-store";
        source_path = root / "exact.zvnsrc";
        error.clear();
        if (!build_store(exact_store, html, exact_nodes, &error)) {
            return internal_failure(error);
        }
        StreamingHtmlNodeSourceStats exact_stats;
        if (!produce_streaming_html_node_source(
                exact_store, source_path, {}, &exact_stats, &error)) {
            return unsupported(error.empty() ? "production tree builder failed closed" : error);
        }
        if (exact_stats.nodes_emitted != exact_nodes) {
            return internal_failure("tree-dump node discovery was not deterministic");
        }
    }

    std::vector<LogicalNodeSourceNode> nodes;
    if (!collect_nodes(source_path, &nodes, &error)) {
        return internal_failure(error);
    }
    std::vector<std::string> lines;
    if (!serialize_wpt_tree(nodes, &lines, &error)) {
        return internal_failure(error);
    }

    std::cout << "MODE\t" << scripting << '\n';
    std::cout << "CAPS\tparse-errors=0\tfragments=0\ttext-nodes=0\tcomments=0\tnamespaces=0\n";
    for (const std::string& line : lines) {
        std::cout << "TREE\t" << encode_hex(line) << '\n';
    }
    std::cout << "STATS\t" << nodes.size() << '\t' << lines.size() << '\n';
    return 0;
}
