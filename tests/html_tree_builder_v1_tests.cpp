#include "html_tree_builder_v1.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTreeBuilderV1Config;
using zevryon::massivedoc::HtmlTreeBuilderV1NodeKind;
using zevryon::massivedoc::HtmlTreeBuilderV1Result;
using zevryon::massivedoc::build_html_tree_v1;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

void serialize_node(
    const HtmlTreeBuilderV1Result& result,
    std::size_t node,
    std::size_t depth,
    std::vector<std::string>* lines) {
    for (const std::size_t child : result.nodes[node].children) {
        const auto& value = result.nodes[child];
        std::string line = "| ";
        line.append(depth * 2U, ' ');
        switch (value.kind) {
        case HtmlTreeBuilderV1NodeKind::Element:
            line.push_back('<');
            line += value.name;
            line.push_back('>');
            break;
        case HtmlTreeBuilderV1NodeKind::Text:
            line.push_back('"');
            line += value.data;
            line.push_back('"');
            break;
        case HtmlTreeBuilderV1NodeKind::Comment:
            line += "<!-- ";
            line += value.data;
            line += " -->";
            break;
        case HtmlTreeBuilderV1NodeKind::Doctype:
            line += "<!DOCTYPE ";
            line += value.name;
            line.push_back('>');
            break;
        case HtmlTreeBuilderV1NodeKind::Document:
            line += "#document";
            break;
        }
        lines->push_back(std::move(line));
        serialize_node(result, child, depth + 1U, lines);
    }
}

bool exact_tree(
    std::string_view input,
    const std::vector<std::string>& expected,
    std::string_view label) {
    HtmlTreeBuilderV1Result result;
    std::string error;
    if (!require(
            build_html_tree_v1(input, false, {}, &result, &error),
            std::string(label) + ": build")) {
        std::cerr << "  error: " << error << '\n';
        return false;
    }
    if (!require(!result.nodes.empty(), std::string(label) + ": root exists") ||
        !require(result.nodes.front().kind == HtmlTreeBuilderV1NodeKind::Document,
                 std::string(label) + ": document root")) {
        return false;
    }
    std::vector<std::string> actual;
    serialize_node(result, 0U, 0U, &actual);
    if (!require(actual == expected, std::string(label) + ": exact tree")) {
        std::cerr << "  actual:\n";
        for (const auto& line : actual) std::cerr << "    " << line << '\n';
        return false;
    }
    return true;
}

bool test_adoption02_exact_trees() {
    return exact_tree(
               "<b>1<i>2<p>3</b>4",
               {
                   "| <html>",
                   "|   <head>",
                   "|   <body>",
                   "|     <b>",
                   "|       \"1\"",
                   "|       <i>",
                   "|         \"2\"",
                   "|     <i>",
                   "|       <p>",
                   "|         <b>",
                   "|           \"3\"",
                   "|         \"4\"",
               },
               "adoption02[0]") &&
        exact_tree(
               "<a><div><style></style><address><a>",
               {
                   "| <html>",
                   "|   <head>",
                   "|   <body>",
                   "|     <a>",
                   "|     <div>",
                   "|       <a>",
                   "|         <style>",
                   "|       <address>",
                   "|         <a>",
                   "|         <a>",
               },
               "adoption02[1]") &&
        exact_tree(
               "<nobr><table><marquee></table><nobr>",
               {
                   "| <html>",
                   "|   <head>",
                   "|   <body>",
                   "|     <nobr>",
                   "|       <marquee>",
                   "|       <table>",
                   "|     <nobr>",
               },
               "adoption02[2]") &&
        exact_tree(
               "<a><table><marquee></table><a>",
               {
                   "| <html>",
                   "|   <head>",
                   "|   <body>",
                   "|     <a>",
                   "|       <marquee>",
                   "|       <table>",
                   "|       <a>",
               },
               "adoption02[3]");
}

bool test_text_comment_and_attribute_materialization() {
    HtmlTreeBuilderV1Result result;
    std::string error;
    if (!require(
            build_html_tree_v1("<div id=x>alpha<!--beta-->gamma</div>", false, {}, &result, &error),
            "materialization build")) {
        std::cerr << "  error: " << error << '\n';
        return false;
    }
    bool saw_div = false;
    bool saw_text = false;
    bool saw_comment = false;
    for (const auto& node : result.nodes) {
        if (node.kind == HtmlTreeBuilderV1NodeKind::Element && node.name == "div") {
            saw_div = node.attributes.size() == 1U &&
                node.attributes[0].name == "id" && node.attributes[0].value == "x";
        } else if (node.kind == HtmlTreeBuilderV1NodeKind::Text &&
                   (node.data == "alpha" || node.data == "gamma")) {
            saw_text = true;
        } else if (node.kind == HtmlTreeBuilderV1NodeKind::Comment && node.data == "beta") {
            saw_comment = true;
        }
    }
    return require(saw_div, "attribute materialization") &&
        require(saw_text, "text materialization") &&
        require(saw_comment, "comment materialization");
}

bool test_bounds_fail_closed() {
    HtmlTreeBuilderV1Result result;
    std::string error;
    HtmlTreeBuilderV1Config config;
    config.maximum_nodes = 3U;
    if (!require(
            !build_html_tree_v1("<div>x</div>", false, config, &result, &error),
            "node bound rejects")) {
        return false;
    }
    return require(!error.empty(), "node bound reports error");
}

} // namespace

int main() {
    if (!test_adoption02_exact_trees() ||
        !test_text_comment_and_attribute_materialization() ||
        !test_bounds_fail_closed()) {
        return 1;
    }
    return 0;
}
