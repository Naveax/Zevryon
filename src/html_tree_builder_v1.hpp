#pragma once

#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::massivedoc {

enum class HtmlTreeBuilderV1NodeKind : std::uint8_t {
    Document,
    Element,
    Text,
    Comment,
    Doctype,
};

enum class HtmlTreeBuilderV1Namespace : std::uint8_t {
    Html,
    MathMl,
    Svg,
};

struct HtmlTreeBuilderV1Node {
    HtmlTreeBuilderV1NodeKind kind{HtmlTreeBuilderV1NodeKind::Element};
    HtmlTreeBuilderV1Namespace name_space{HtmlTreeBuilderV1Namespace::Html};
    std::string name;
    std::string data;
    std::vector<HtmlTokenizerV1Attribute> attributes;
    std::size_t parent{0U};
    std::vector<std::size_t> children;
};

struct HtmlTreeBuilderV1ParseError {
    std::string code;
    std::uint64_t line{1U};
    std::uint64_t column{1U};
    bool from_tokenizer{false};
};

struct HtmlTreeBuilderV1Config {
    std::size_t maximum_input_bytes{1024U * 1024U};
    std::size_t maximum_token_bytes{64U * 1024U};
    std::size_t maximum_nodes{262144U};
    std::size_t maximum_open_elements{4096U};
    std::size_t maximum_active_formatting_elements{4096U};
};

struct HtmlTreeBuilderV1Stats {
    std::uint64_t input_bytes{0U};
    std::uint64_t tokens_seen{0U};
    std::uint64_t nodes_created{0U};
    std::uint64_t text_bytes{0U};
    std::uint64_t tree_parse_errors{0U};
    std::uint64_t tokenizer_parse_errors{0U};
    std::uint32_t maximum_open_element_depth{0U};
};

struct HtmlTreeBuilderV1Result {
    std::vector<HtmlTreeBuilderV1Node> nodes;
    std::vector<HtmlTreeBuilderV1ParseError> parse_errors;
    HtmlTreeBuilderV1Stats stats;
};

// Bounded HTML tree-construction slice that consumes the production tokenizer
// token stream. The builder owns stable index-based nodes, supports implicit
// html/head/body construction, active formatting elements, the adoption agency
// algorithm, table foster parenting and core in-body scope recovery. Unsupported
// tokenizer states still fail closed through tokenize_html_token_stream_v1().
bool build_html_tree_v1(
    std::string_view input,
    bool scripting_enabled,
    HtmlTreeBuilderV1Config config,
    HtmlTreeBuilderV1Result* result,
    std::string* error);

} // namespace zevryon::massivedoc
