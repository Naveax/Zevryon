#include "html_tree_builder_v1.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTreeBuilderV1Node;
using zevryon::massivedoc::HtmlTreeBuilderV1NodeKind;
using zevryon::massivedoc::HtmlTreeBuilderV1Result;
using zevryon::massivedoc::build_html_tree_v1;

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decode_hex(std::string_view value, std::string* output) {
    if ((value.size() & 1U) != 0U) return false;
    output->clear();
    output->reserve(value.size() / 2U);
    for (std::size_t index = 0U; index < value.size(); index += 2U) {
        const int high = hex_value(value[index]);
        const int low = hex_value(value[index + 1U]);
        if (high < 0 || low < 0) return false;
        output->push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::string encode_hex(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(value.size() * 2U, '\0');
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const unsigned byte = static_cast<unsigned char>(value[index]);
        output[index * 2U] = digits[(byte >> 4U) & 0xFU];
        output[index * 2U + 1U] = digits[byte & 0xFU];
    }
    return output;
}

void append_node_lines(
    const HtmlTreeBuilderV1Result& result,
    std::size_t node_index,
    std::size_t depth,
    std::vector<std::string>* lines) {
    const auto& parent = result.nodes[node_index];
    for (const std::size_t child_index : parent.children) {
        const HtmlTreeBuilderV1Node& node = result.nodes[child_index];
        std::string line = "| ";
        line.append(depth * 2U, ' ');
        switch (node.kind) {
        case HtmlTreeBuilderV1NodeKind::Element: {
            line.push_back('<');
            line += node.name;
            line.push_back('>');
            lines->push_back(std::move(line));
            std::vector<std::pair<std::string, std::string>> attributes;
            attributes.reserve(node.attributes.size());
            for (const auto& attribute : node.attributes) {
                attributes.emplace_back(attribute.name, attribute.value);
            }
            std::sort(attributes.begin(), attributes.end());
            for (const auto& [name, value] : attributes) {
                std::string attribute_line = "| ";
                attribute_line.append((depth + 1U) * 2U, ' ');
                attribute_line += name;
                attribute_line += "=\"";
                attribute_line += value;
                attribute_line.push_back('"');
                lines->push_back(std::move(attribute_line));
            }
            append_node_lines(result, child_index, depth + 1U, lines);
            break;
        }
        case HtmlTreeBuilderV1NodeKind::Text:
            line.push_back('"');
            line += node.data;
            line.push_back('"');
            lines->push_back(std::move(line));
            break;
        case HtmlTreeBuilderV1NodeKind::Comment:
            line += "<!-- ";
            line += node.data;
            line += " -->";
            lines->push_back(std::move(line));
            break;
        case HtmlTreeBuilderV1NodeKind::Doctype:
            line += "<!DOCTYPE ";
            line += node.name;
            line.push_back('>');
            lines->push_back(std::move(line));
            break;
        case HtmlTreeBuilderV1NodeKind::Document:
            break;
        }
    }
}

int unsupported(std::string_view reason) {
    std::cout << "UNSUPPORTED\t" << encode_hex(reason) << '\n';
    return 2;
}

int failure(std::string_view reason) {
    std::cout << "FAIL\t" << encode_hex(reason) << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        return failure("usage: html_tree_dump_v1_probe <scripting:0|1> <html-hex>");
    }
    const std::string_view mode = argv[1];
    if (mode != "0" && mode != "1") {
        return failure("scripting mode must be 0 or 1");
    }
    std::string html;
    if (!decode_hex(argv[2], &html)) {
        return failure("HTML argument is not valid hexadecimal");
    }

    HtmlTreeBuilderV1Result result;
    std::string error;
    if (!build_html_tree_v1(html, mode == "1", {}, &result, &error)) {
        return unsupported(error.empty() ? "production tree builder failed closed" : error);
    }
    if (result.nodes.empty() || result.nodes.front().kind != HtmlTreeBuilderV1NodeKind::Document) {
        return failure("production tree builder omitted document root");
    }

    std::vector<std::string> lines;
    append_node_lines(result, 0U, 0U, &lines);
    std::cout << "MODE\t" << mode << '\n';
    std::cout << "CAPS\tparse-errors=0\tfragments=0\ttext-nodes=1\tcomments=1\tnamespaces=0\n";
    for (const auto& line : lines) {
        std::cout << "TREE\t" << encode_hex(line) << '\n';
    }
    std::cout << "STATS\t" << result.nodes.size() << '\t' << lines.size() << '\n';
    return 0;
}
