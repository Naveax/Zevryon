#pragma once

#include "html_tokenizer_token_stream_v1.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {

struct HtmlTreeBuilderV1Config {
    std::size_t maximum_input_bytes{1024U * 1024U};
    std::size_t maximum_nodes{1024U * 1024U};
    std::size_t maximum_tree_text_bytes{64U * 1024U * 1024U};
    std::size_t maximum_open_elements{4096U};
    std::size_t maximum_active_formatting_elements{4096U};
};

struct HtmlTreeBuilderV1Node {
    enum class Kind : std::uint8_t {
        Document,
        Element,
        Text,
        Comment,
    };

    Kind kind{Kind::Element};
    std::string name;
    std::string data;
    std::vector<HtmlTokenizerV1Attribute> attributes;
    std::uint64_t parent{std::numeric_limits<std::uint64_t>::max()};
    std::vector<std::uint64_t> children;
};

struct HtmlTreeBuilderV1Stats {
    std::uint64_t tokenizer_parse_errors{0U};
    std::uint64_t tree_builder_parse_errors{0U};
    std::uint64_t adoption_agency_runs{0U};
    std::uint64_t foster_parent_insertions{0U};
    std::uint64_t active_formatting_reconstructions{0U};
    std::uint64_t nodes_created{0U};
    std::uint64_t text_bytes_materialized{0U};
};

struct HtmlTreeBuilderV1Result {
    std::vector<HtmlTreeBuilderV1Node> nodes;
    HtmlTreeBuilderV1Stats stats;
};

namespace html_tree_builder_v1_detail {

constexpr std::uint64_t kNoNode = std::numeric_limits<std::uint64_t>::max();

inline bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

inline bool formatting_tag(std::string_view tag) noexcept {
    constexpr std::string_view values[] = {
        "a", "b", "big", "code", "em", "font", "i", "nobr", "s",
        "small", "strike", "strong", "tt", "u"};
    return std::find(std::begin(values), std::end(values), tag) != std::end(values);
}

inline bool special_tag(std::string_view tag) noexcept {
    constexpr std::string_view values[] = {
        "address", "applet", "area", "article", "aside", "base", "basefont",
        "bgsound", "blockquote", "body", "br", "button", "caption", "center",
        "col", "colgroup", "dd", "details", "dialog", "dir", "div", "dl",
        "dt", "embed", "fieldset", "figcaption", "figure", "footer", "form",
        "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head",
        "header", "hgroup", "hr", "html", "iframe", "img", "input", "keygen",
        "li", "link", "listing", "main", "marquee", "menu", "meta", "nav",
        "noembed", "noframes", "noscript", "object", "ol", "p", "param",
        "plaintext", "pre", "script", "search", "section", "select", "source",
        "style", "summary", "table", "tbody", "td", "template", "textarea",
        "tfoot", "th", "thead", "title", "tr", "track", "ul", "wbr", "xmp"};
    return std::find(std::begin(values), std::end(values), tag) != std::end(values);
}

inline bool void_tag(std::string_view tag) noexcept {
    constexpr std::string_view values[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"};
    return std::find(std::begin(values), std::end(values), tag) != std::end(values);
}

inline bool table_context_tag(std::string_view tag) noexcept {
    return tag == "table" || tag == "tbody" || tag == "tfoot" ||
        tag == "thead" || tag == "tr";
}

class Builder final : public HtmlTokenizerV1Sink {
public:
    Builder(
        HtmlTreeBuilderV1Config config,
        HtmlTreeBuilderV1Result* result,
        std::string* error)
        : config_(config), result_(result), error_(error) {}

    bool begin() {
        if (result_ == nullptr || error_ == nullptr) {
            return false;
        }
        result_->nodes.clear();
        result_->stats = {};
        if (config_.maximum_nodes < 4U || config_.maximum_open_elements < 3U) {
            return fail(error_, "HTML tree-builder bounds cannot contain the implicit document tree");
        }
        const std::uint64_t document = create_node(HtmlTreeBuilderV1Node::Kind::Document, "#document");
        const std::uint64_t html = create_node(HtmlTreeBuilderV1Node::Kind::Element, "html");
        const std::uint64_t head = create_node(HtmlTreeBuilderV1Node::Kind::Element, "head");
        const std::uint64_t body = create_node(HtmlTreeBuilderV1Node::Kind::Element, "body");
        if (document == kNoNode || html == kNoNode || head == kNoNode || body == kNoNode) {
            return false;
        }
        if (!append_child(document, html) || !append_child(html, head) || !append_child(html, body)) {
            return false;
        }
        open_.push_back(document);
        open_.push_back(html);
        open_.push_back(body);
        body_ = body;
        return true;
    }

    bool finish() {
        if (open_.size() > 3U) {
            ++result_->stats.tree_builder_parse_errors;
        }
        return true;
    }

    bool on_token(const HtmlTokenizerV1Token& token, std::string* error) override {
        if (error == nullptr) {
            return false;
        }
        switch (token.kind) {
        case HtmlTokenizerV1TokenKind::Doctype:
            saw_doctype_ = true;
            return true;
        case HtmlTokenizerV1TokenKind::Comment:
            return insert_comment(token.data);
        case HtmlTokenizerV1TokenKind::Character:
            note_missing_doctype();
            return insert_character(token.data);
        case HtmlTokenizerV1TokenKind::StartTag:
            note_missing_doctype();
            return start_tag(token);
        case HtmlTokenizerV1TokenKind::EndTag:
            note_missing_doctype();
            return end_tag(token.name);
        }
        return fail(error_, "HTML tree-builder received an unknown tokenizer token kind");
    }

    bool on_parse_error(const HtmlTokenizerV1ParseError&, std::string* error) override {
        if (error == nullptr) {
            return false;
        }
        ++result_->stats.tokenizer_parse_errors;
        return true;
    }

private:
    std::uint64_t create_node(HtmlTreeBuilderV1Node::Kind kind, std::string_view name) {
        if (result_->nodes.size() >= config_.maximum_nodes) {
            fail(error_, "HTML tree-builder node bound exceeded");
            return kNoNode;
        }
        HtmlTreeBuilderV1Node node;
        node.kind = kind;
        node.name.assign(name);
        result_->nodes.push_back(std::move(node));
        ++result_->stats.nodes_created;
        return static_cast<std::uint64_t>(result_->nodes.size() - 1U);
    }

    std::uint64_t clone_element(std::uint64_t source) {
        if (source >= result_->nodes.size() ||
            result_->nodes[static_cast<std::size_t>(source)].kind != HtmlTreeBuilderV1Node::Kind::Element) {
            fail(error_, "HTML tree-builder attempted to clone a non-element");
            return kNoNode;
        }
        const auto& original = result_->nodes[static_cast<std::size_t>(source)];
        const std::uint64_t clone = create_node(HtmlTreeBuilderV1Node::Kind::Element, original.name);
        if (clone == kNoNode) {
            return kNoNode;
        }
        result_->nodes[static_cast<std::size_t>(clone)].attributes = original.attributes;
        return clone;
    }

    bool detach(std::uint64_t child) {
        if (child >= result_->nodes.size()) {
            return fail(error_, "HTML tree-builder child ordinal is outside tree");
        }
        auto& child_node = result_->nodes[static_cast<std::size_t>(child)];
        if (child_node.parent == kNoNode) {
            return true;
        }
        if (child_node.parent >= result_->nodes.size()) {
            return fail(error_, "HTML tree-builder parent ordinal is outside tree");
        }
        auto& siblings = result_->nodes[static_cast<std::size_t>(child_node.parent)].children;
        const auto iterator = std::find(siblings.begin(), siblings.end(), child);
        if (iterator == siblings.end()) {
            return fail(error_, "HTML tree-builder parent/child topology is inconsistent");
        }
        siblings.erase(iterator);
        child_node.parent = kNoNode;
        return true;
    }

    bool append_child(std::uint64_t parent, std::uint64_t child) {
        if (parent >= result_->nodes.size() || child >= result_->nodes.size() || parent == child) {
            return fail(error_, "HTML tree-builder append topology is invalid");
        }
        if (!detach(child)) {
            return false;
        }
        result_->nodes[static_cast<std::size_t>(child)].parent = parent;
        result_->nodes[static_cast<std::size_t>(parent)].children.push_back(child);
        return true;
    }

    bool insert_before(std::uint64_t parent, std::uint64_t child, std::uint64_t before) {
        if (parent >= result_->nodes.size() || child >= result_->nodes.size() || before >= result_->nodes.size()) {
            return fail(error_, "HTML tree-builder insertion topology is invalid");
        }
        if (!detach(child)) {
            return false;
        }
        auto& siblings = result_->nodes[static_cast<std::size_t>(parent)].children;
        const auto iterator = std::find(siblings.begin(), siblings.end(), before);
        if (iterator == siblings.end()) {
            return fail(error_, "HTML tree-builder foster insertion anchor is missing");
        }
        result_->nodes[static_cast<std::size_t>(child)].parent = parent;
        siblings.insert(iterator, child);
        return true;
    }

    std::size_t open_position(std::uint64_t node) const noexcept {
        const auto iterator = std::find(open_.begin(), open_.end(), node);
        return iterator == open_.end()
            ? open_.size()
            : static_cast<std::size_t>(iterator - open_.begin());
    }

    std::size_t active_position(std::uint64_t node) const noexcept {
        const auto iterator = std::find(active_.begin(), active_.end(), node);
        return iterator == active_.end()
            ? active_.size()
            : static_cast<std::size_t>(iterator - active_.begin());
    }

    std::size_t active_tag_position(std::string_view tag) const noexcept {
        for (std::size_t index = active_.size(); index != 0U; --index) {
            const std::uint64_t node = active_[index - 1U];
            if (node != kNoNode && node < result_->nodes.size() &&
                result_->nodes[static_cast<std::size_t>(node)].name == tag) {
                return index - 1U;
            }
        }
        return active_.size();
    }

    bool in_table_mode() const noexcept {
        if (open_.empty()) {
            return false;
        }
        for (std::size_t index = open_.size(); index != 0U; --index) {
            const std::string& name = result_->nodes[static_cast<std::size_t>(open_[index - 1U])].name;
            if (name == "table") {
                return true;
            }
            if (name == "body" || name == "html") {
                return false;
            }
        }
        return false;
    }

    std::uint64_t last_table_on_stack() const noexcept {
        for (std::size_t index = open_.size(); index != 0U; --index) {
            const std::uint64_t node = open_[index - 1U];
            if (result_->nodes[static_cast<std::size_t>(node)].name == "table") {
                return node;
            }
        }
        return kNoNode;
    }

    bool insert_at_appropriate_place(std::uint64_t node, std::uint64_t override_target = kNoNode) {
        if (open_.empty()) {
            return fail(error_, "HTML tree-builder has no insertion target");
        }
        const std::uint64_t target = override_target == kNoNode ? open_.back() : override_target;
        const bool foster = in_table_mode() &&
            (override_target == kNoNode || table_context_tag(result_->nodes[static_cast<std::size_t>(target)].name));
        if (!foster) {
            return append_child(target, node);
        }
        const std::uint64_t table = last_table_on_stack();
        if (table != kNoNode && table < result_->nodes.size()) {
            const std::uint64_t parent = result_->nodes[static_cast<std::size_t>(table)].parent;
            if (parent != kNoNode) {
                ++result_->stats.foster_parent_insertions;
                return insert_before(parent, node, table);
            }
        }
        ++result_->stats.foster_parent_insertions;
        return append_child(open_.size() >= 2U ? open_[open_.size() - 2U] : body_, node);
    }

    bool move_all_children(std::uint64_t source, std::uint64_t destination) {
        if (source >= result_->nodes.size() || destination >= result_->nodes.size()) {
            return fail(error_, "HTML tree-builder move-children ordinal is invalid");
        }
        const std::vector<std::uint64_t> children =
            result_->nodes[static_cast<std::size_t>(source)].children;
        for (const std::uint64_t child : children) {
            if (!append_child(destination, child)) {
                return false;
            }
        }
        return true;
    }

    void note_missing_doctype() {
        if (!saw_non_doctype_token_) {
            saw_non_doctype_token_ = true;
            if (!saw_doctype_) {
                ++result_->stats.tree_builder_parse_errors;
            }
        }
    }

    bool reconstruct_active_formatting() {
        if (active_.empty()) {
            return true;
        }
        std::size_t index = active_.size();
        while (index != 0U) {
            const std::uint64_t node = active_[index - 1U];
            if (node == kNoNode || open_position(node) != open_.size()) {
                break;
            }
            --index;
        }
        if (index == active_.size()) {
            return true;
        }
        for (; index < active_.size(); ++index) {
            const std::uint64_t old_node = active_[index];
            if (old_node == kNoNode) {
                continue;
            }
            const std::uint64_t clone = clone_element(old_node);
            if (clone == kNoNode || !insert_at_appropriate_place(clone)) {
                return false;
            }
            if (open_.size() >= config_.maximum_open_elements) {
                return fail(error_, "HTML tree-builder open-element bound exceeded during reconstruction");
            }
            open_.push_back(clone);
            active_[index] = clone;
            ++result_->stats.active_formatting_reconstructions;
        }
        return true;
    }

    bool insert_text_node(std::string_view data) {
        if (data.empty()) {
            return true;
        }
        if (result_->stats.text_bytes_materialized > config_.maximum_tree_text_bytes -
                std::min(config_.maximum_tree_text_bytes, data.size())) {
            return fail(error_, "HTML tree-builder text-byte bound exceeded");
        }
        const std::uint64_t target = current_insertion_parent();
        if (target == kNoNode) {
            return fail(error_, "HTML tree-builder text insertion parent is unavailable");
        }
        auto& target_node = result_->nodes[static_cast<std::size_t>(target)];
        if (!target_node.children.empty()) {
            const std::uint64_t previous = target_node.children.back();
            if (previous < result_->nodes.size()) {
                auto& previous_node = result_->nodes[static_cast<std::size_t>(previous)];
                if (previous_node.kind == HtmlTreeBuilderV1Node::Kind::Text) {
                    previous_node.data.append(data);
                    result_->stats.text_bytes_materialized += static_cast<std::uint64_t>(data.size());
                    return true;
                }
            }
        }
        const std::uint64_t node = create_node(HtmlTreeBuilderV1Node::Kind::Text, "#text");
        if (node == kNoNode) {
            return false;
        }
        result_->nodes[static_cast<std::size_t>(node)].data.assign(data);
        result_->stats.text_bytes_materialized += static_cast<std::uint64_t>(data.size());
        return insert_at_appropriate_place(node);
    }

    std::uint64_t current_insertion_parent() const noexcept {
        if (open_.empty()) {
            return kNoNode;
        }
        if (!in_table_mode()) {
            return open_.back();
        }
        const std::uint64_t table = last_table_on_stack();
        if (table != kNoNode && table < result_->nodes.size()) {
            const std::uint64_t parent = result_->nodes[static_cast<std::size_t>(table)].parent;
            if (parent != kNoNode) {
                return parent;
            }
        }
        return open_.size() >= 2U ? open_[open_.size() - 2U] : body_;
    }

    bool insert_character(std::string_view data) {
        if (!reconstruct_active_formatting()) {
            return false;
        }
        return insert_text_node(data);
    }

    bool insert_comment(std::string_view data) {
        const std::uint64_t node = create_node(HtmlTreeBuilderV1Node::Kind::Comment, "#comment");
        if (node == kNoNode) {
            return false;
        }
        result_->nodes[static_cast<std::size_t>(node)].data.assign(data);
        return insert_at_appropriate_place(node);
    }

    bool push_element(const HtmlTokenizerV1Token& token, bool add_formatting) {
        if (open_.size() >= config_.maximum_open_elements) {
            return fail(error_, "HTML tree-builder open-element bound exceeded");
        }
        if (add_formatting && active_.size() >= config_.maximum_active_formatting_elements) {
            return fail(error_, "HTML tree-builder active-formatting bound exceeded");
        }
        const std::uint64_t node = create_node(HtmlTreeBuilderV1Node::Kind::Element, token.name);
        if (node == kNoNode) {
            return false;
        }
        result_->nodes[static_cast<std::size_t>(node)].attributes = token.attributes;
        if (!insert_at_appropriate_place(node)) {
            return false;
        }
        if (!token.self_closing && !void_tag(token.name)) {
            open_.push_back(node);
        }
        if (add_formatting) {
            active_.push_back(node);
        }
        return true;
    }

    bool start_tag(const HtmlTokenizerV1Token& token) {
        if (token.name == "html" || token.name == "head" || token.name == "body") {
            ++result_->stats.tree_builder_parse_errors;
            return true;
        }
        if (formatting_tag(token.name)) {
            if ((token.name == "a" || token.name == "nobr") &&
                active_tag_position(token.name) != active_.size()) {
                ++result_->stats.tree_builder_parse_errors;
                if (!adoption_agency(token.name)) {
                    return false;
                }
            }
            if (!reconstruct_active_formatting()) {
                return false;
            }
            return push_element(token, true);
        }
        if (token.name == "table") {
            return push_element(token, false);
        }
        if (token.name == "marquee" && in_table_mode()) {
            ++result_->stats.tree_builder_parse_errors;
            if (!reconstruct_active_formatting()) {
                return false;
            }
            return push_element(token, false);
        }
        if (token.name == "style" || token.name == "address" || token.name == "div" ||
            token.name == "p") {
            return push_element(token, false);
        }
        if (!reconstruct_active_formatting()) {
            return false;
        }
        return push_element(token, false);
    }

    bool end_tag(std::string_view tag) {
        if (formatting_tag(tag)) {
            return adoption_agency(tag);
        }
        if (tag == "table") {
            const std::uint64_t table = last_table_on_stack();
            if (table == kNoNode) {
                ++result_->stats.tree_builder_parse_errors;
                return true;
            }
            const std::size_t position = open_position(table);
            open_.resize(position);
            return true;
        }
        for (std::size_t index = open_.size(); index > 3U; --index) {
            const std::uint64_t node = open_[index - 1U];
            if (result_->nodes[static_cast<std::size_t>(node)].name == tag) {
                open_.resize(index - 1U);
                return true;
            }
            if (special_tag(result_->nodes[static_cast<std::size_t>(node)].name)) {
                ++result_->stats.tree_builder_parse_errors;
                return true;
            }
        }
        ++result_->stats.tree_builder_parse_errors;
        return true;
    }

    bool adoption_agency(std::string_view subject) {
        ++result_->stats.adoption_agency_runs;
        for (unsigned outer = 0U; outer < 8U; ++outer) {
            const std::size_t active_index = active_tag_position(subject);
            if (active_index == active_.size()) {
                return generic_formatting_end(subject);
            }
            const std::uint64_t formatting = active_[active_index];
            const std::size_t formatting_stack = open_position(formatting);
            if (formatting_stack == open_.size()) {
                active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(active_index));
                ++result_->stats.tree_builder_parse_errors;
                return true;
            }
            ++result_->stats.tree_builder_parse_errors;

            std::size_t furthest_stack = open_.size();
            for (std::size_t index = formatting_stack + 1U; index < open_.size(); ++index) {
                if (special_tag(result_->nodes[static_cast<std::size_t>(open_[index])].name)) {
                    furthest_stack = index;
                    break;
                }
            }
            if (furthest_stack == open_.size()) {
                open_.resize(formatting_stack);
                const std::size_t current_active = active_position(formatting);
                if (current_active != active_.size()) {
                    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(current_active));
                }
                return true;
            }

            if (formatting_stack == 0U) {
                return fail(error_, "HTML adoption agency lost common ancestor");
            }
            const std::uint64_t furthest = open_[furthest_stack];
            const std::uint64_t common_ancestor = open_[formatting_stack - 1U];
            std::size_t bookmark = active_index;
            std::uint64_t node = furthest;
            std::uint64_t last_node = furthest;
            unsigned inner = 0U;

            for (;;) {
                const std::size_t node_stack = open_position(node);
                if (node_stack == open_.size() || node_stack == 0U) {
                    return fail(error_, "HTML adoption agency stack topology drifted");
                }
                node = open_[node_stack - 1U];
                ++inner;
                if (node == formatting) {
                    break;
                }
                std::size_t node_active = active_position(node);
                if (inner > 3U && node_active != active_.size()) {
                    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(node_active));
                    node_active = active_.size();
                }
                if (node_active == active_.size()) {
                    const std::size_t remove_stack = open_position(node);
                    if (remove_stack == open_.size()) {
                        return fail(error_, "HTML adoption agency cannot remove stack node");
                    }
                    open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(remove_stack));
                    continue;
                }

                const std::uint64_t clone = clone_element(node);
                if (clone == kNoNode) {
                    return false;
                }
                const std::size_t replace_stack = open_position(node);
                node_active = active_position(node);
                if (replace_stack == open_.size() || node_active == active_.size()) {
                    return fail(error_, "HTML adoption agency replacement topology drifted");
                }
                open_[replace_stack] = clone;
                active_[node_active] = clone;
                if (last_node == furthest) {
                    bookmark = node_active + 1U;
                }
                if (!append_child(clone, last_node)) {
                    return false;
                }
                last_node = clone;
            }

            if (!insert_at_appropriate_place(last_node, common_ancestor)) {
                return false;
            }
            const std::uint64_t new_formatting = clone_element(formatting);
            if (new_formatting == kNoNode ||
                !move_all_children(furthest, new_formatting) ||
                !append_child(furthest, new_formatting)) {
                return false;
            }

            const std::size_t remove_active = active_position(formatting);
            if (remove_active == active_.size()) {
                return fail(error_, "HTML adoption agency lost active formatting element");
            }
            active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(remove_active));
            if (bookmark > active_.size()) {
                bookmark = active_.size();
            }
            active_.insert(active_.begin() + static_cast<std::ptrdiff_t>(bookmark), new_formatting);

            const std::size_t remove_stack = open_position(formatting);
            if (remove_stack == open_.size()) {
                return fail(error_, "HTML adoption agency lost formatting stack element");
            }
            open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(remove_stack));
            const std::size_t furthest_position = open_position(furthest);
            if (furthest_position == open_.size()) {
                return fail(error_, "HTML adoption agency lost furthest block");
            }
            open_.insert(
                open_.begin() + static_cast<std::ptrdiff_t>(furthest_position + 1U),
                new_formatting);
        }
        return true;
    }

    bool generic_formatting_end(std::string_view subject) {
        for (std::size_t index = open_.size(); index > 3U; --index) {
            const std::uint64_t node = open_[index - 1U];
            if (result_->nodes[static_cast<std::size_t>(node)].name == subject) {
                open_.resize(index - 1U);
                return true;
            }
        }
        ++result_->stats.tree_builder_parse_errors;
        return true;
    }

    HtmlTreeBuilderV1Config config_{};
    HtmlTreeBuilderV1Result* result_{nullptr};
    std::string* error_{nullptr};
    std::vector<std::uint64_t> open_;
    std::vector<std::uint64_t> active_;
    std::uint64_t body_{kNoNode};
    bool saw_doctype_{false};
    bool saw_non_doctype_token_{false};
};

} // namespace html_tree_builder_v1_detail

inline bool build_html_tree_v1(
    std::string_view input,
    HtmlTreeBuilderV1Config config,
    HtmlTreeBuilderV1Result* result,
    std::string* error) {
    if (error == nullptr || result == nullptr) {
        return false;
    }
    error->clear();
    if (input.size() > config.maximum_input_bytes) {
        return html_tree_builder_v1_detail::fail(error, "HTML tree-builder input bound exceeded");
    }
    if (config.maximum_tree_text_bytes == 0U || config.maximum_nodes == 0U ||
        config.maximum_open_elements == 0U || config.maximum_active_formatting_elements == 0U) {
        return html_tree_builder_v1_detail::fail(error, "HTML tree-builder configuration contains a zero bound");
    }

    html_tree_builder_v1_detail::Builder builder(config, result, error);
    if (!builder.begin()) {
        return false;
    }
    HtmlTokenizerV1Stats tokenizer_stats;
    HtmlTokenizerV1Config tokenizer_config;
    tokenizer_config.maximum_input_bytes = config.maximum_input_bytes;
    tokenizer_config.maximum_token_bytes = 64U * 1024U;
    if (!tokenize_html_token_stream_v1(
            input,
            HtmlTokenizerV1InitialState::Data,
            {},
            tokenizer_config,
            &builder,
            &tokenizer_stats,
            error)) {
        return false;
    }
    return builder.finish();
}

} // namespace zevryon::massivedoc
