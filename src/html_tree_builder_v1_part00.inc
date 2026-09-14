#include "html_tree_builder_v1.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kNoNode = std::numeric_limits<std::size_t>::max();

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool equals_any(std::string_view value, std::initializer_list<std::string_view> values) {
    for (const auto candidate : values) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

bool is_whitespace(std::string_view data) {
    for (const char c : data) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') {
            return false;
        }
    }
    return true;
}

bool is_void(std::string_view tag) {
    return equals_any(tag, {
        "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr"});
}

bool is_formatting(std::string_view tag) {
    return equals_any(tag, {
        "a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small",
        "strike", "strong", "tt", "u"});
}

bool is_special(std::string_view tag) {
    return equals_any(tag, {
        "address", "applet", "area", "article", "aside", "base", "basefont",
        "bgsound", "blockquote", "body", "br", "button", "caption", "center",
        "col", "colgroup", "dd", "details", "dir", "div", "dl", "dt", "embed",
        "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset",
        "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr",
        "html", "iframe", "img", "input", "li", "link", "listing", "main",
        "marquee", "menu", "meta", "nav", "noembed", "noframes", "noscript",
        "object", "ol", "p", "param", "plaintext", "pre", "script", "search",
        "section", "select", "source", "style", "summary", "table", "tbody", "td",
        "template", "textarea", "tfoot", "th", "thead", "title", "tr", "track",
        "ul", "wbr", "xmp"});
}

bool is_block_start(std::string_view tag) {
    return equals_any(tag, {
        "address", "article", "aside", "blockquote", "center", "details", "dialog",
        "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "header",
        "hgroup", "main", "menu", "nav", "ol", "p", "search", "section", "summary",
        "ul"});
}

class Builder final : public HtmlTokenizerV1Sink {
public:
    Builder(bool scripting_enabled,
            HtmlTreeBuilderV1Config config,
            HtmlTreeBuilderV1Result* result,
            std::string* error)
        : scripting_enabled_(scripting_enabled),
          config_(config),
          result_(result),
          error_(error) {}

    bool begin(std::string_view input) {
        if (result_ == nullptr || error_ == nullptr) {
            return false;
        }
        error_->clear();
        result_->nodes.clear();
        result_->parse_errors.clear();
        result_->stats = {};
        result_->stats.input_bytes = static_cast<std::uint64_t>(input.size());
        if (config_.maximum_input_bytes == 0U || input.size() > config_.maximum_input_bytes) {
            return fail(error_, "HTML tree builder input exceeds bounded limit");
        }
        if (config_.maximum_token_bytes == 0U || config_.maximum_nodes < 4U ||
            config_.maximum_open_elements < 2U ||
            config_.maximum_active_formatting_elements == 0U) {
            return fail(error_, "HTML tree builder configuration is outside supported bounds");
        }

        document_ = append_raw_node(HtmlTreeBuilderV1NodeKind::Document, "#document", kNoNode);
        if (document_ == kNoNode) {
            return false;
        }
        html_ = append_element("html", document_, {});
        head_ = append_element("head", html_, {});
        body_ = append_element("body", html_, {});
        if (html_ == kNoNode || head_ == kNoNode || body_ == kNoNode) {
            return false;
        }
        open_elements_.push_back(html_);
        open_elements_.push_back(body_);
        update_depth_stat();
        return true;
    }

    bool on_token(const HtmlTokenizerV1Token& token, std::string* error) override {
        if (error != error_) {
            return fail(error_, "HTML tree builder tokenizer error sink mismatch");
        }
        ++result_->stats.tokens_seen;
        return process(token, false);
    }

    bool on_parse_error(const HtmlTokenizerV1ParseError& parse_error,
                        std::string* error) override {
        if (error != error_) {
            return fail(error_, "HTML tree builder tokenizer parse-error sink mismatch");
        }
        if (result_->parse_errors.size() >= config_.maximum_nodes) {
            return fail(error_, "HTML tree builder parse-error bound exhausted");
        }
        result_->parse_errors.push_back(HtmlTreeBuilderV1ParseError{
            parse_error.code, parse_error.line, parse_error.column, true});
        ++result_->stats.tokenizer_parse_errors;
        return true;
    }

private:
    enum class InsertionMode : std::uint8_t {
        InBody,
        InTable,
        InTableBody,
        InRow,
        InCell,
        InSelect,
        AfterBody,
    };

    std::size_t append_raw_node(HtmlTreeBuilderV1NodeKind kind,
                                std::string name,
                                std::size_t parent) {
        if (result_->nodes.size() >= config_.maximum_nodes) {
            fail(error_, "HTML tree builder node bound exhausted");
            return kNoNode;
        }
        const std::size_t index = result_->nodes.size();
        HtmlTreeBuilderV1Node node;
        node.kind = kind;
        node.name = std::move(name);
        node.parent = parent;
        result_->nodes.push_back(std::move(node));
        ++result_->stats.nodes_created;
        if (parent != kNoNode) {
            if (parent >= result_->nodes.size()) {
                fail(error_, "HTML tree builder parent index is invalid");
                return kNoNode;
            }
            result_->nodes[parent].children.push_back(index);
        }
        return index;
    }

    std::size_t append_element(std::string_view name,
                               std::size_t parent,
                               const std::vector<HtmlTokenizerV1Attribute>& attributes,
                               bool before_table = false) {
        const std::size_t index = append_raw_node(
            HtmlTreeBuilderV1NodeKind::Element, std::string(name), kNoNode);
        if (index == kNoNode) {
            return kNoNode;
        }
        result_->nodes[index].attributes = attributes;
        if (parent != kNoNode && !attach(index, parent, before_table)) {
            return kNoNode;
        }
        return index;
    }

    bool attach(std::size_t child, std::size_t parent, bool before_table = false) {
        if (child >= result_->nodes.size() || parent >= result_->nodes.size()) {
            return fail(error_, "HTML tree builder attachment index is invalid");
        }
        detach(child);
        auto& children = result_->nodes[parent].children;
        if (before_table && table_for_foster_ != kNoNode) {
            const auto it = std::find(children.begin(), children.end(), table_for_foster_);
            if (it != children.end()) {
                children.insert(it, child);
                result_->nodes[child].parent = parent;
                return true;
            }
        }
        children.push_back(child);
        result_->nodes[child].parent = parent;
        return true;
    }

    void detach(std::size_t node) {
        if (node >= result_->nodes.size()) {
            return;
        }
        const std::size_t parent = result_->nodes[node].parent;
        if (parent == kNoNode || parent >= result_->nodes.size()) {
            result_->nodes[node].parent = kNoNode;
            return;
        }
        auto& children = result_->nodes[parent].children;
        const auto it = std::find(children.begin(), children.end(), node);
        if (it != children.end()) {
            children.erase(it);
        }
        result_->nodes[node].parent = kNoNode;
    }

    std::size_t current_node() const {
        return open_elements_.empty() ? body_ : open_elements_.back();
    }

    std::string_view node_name(std::size_t node) const {
        return node < result_->nodes.size() ? std::string_view(result_->nodes[node].name)
                                            : std::string_view{};
    }

    void update_depth_stat() {
        result_->stats.maximum_open_element_depth = std::max(
            result_->stats.maximum_open_element_depth,
            static_cast<std::uint32_t>(std::min<std::size_t>(
                open_elements_.size(), std::numeric_limits<std::uint32_t>::max())));
    }

    bool push_open(std::size_t node) {
        if (open_elements_.size() >= config_.maximum_open_elements) {
            return fail(error_, "HTML tree builder open-element bound exhausted");
        }
        open_elements_.push_back(node);
        update_depth_stat();
        return true;
    }

    bool add_active(std::size_t node) {
        if (active_formatting_.size() >= config_.maximum_active_formatting_elements) {
            return fail(error_, "HTML tree builder active-formatting bound exhausted");
        }
        active_formatting_.push_back(node);
        return true;
    }

    std::size_t appropriate_parent() {
        if (!foster_parenting_) {
            return current_node();
        }
        table_for_foster_ = kNoNode;
        std::size_t last_template = kNoNode;
        std::size_t last_table = kNoNode;
        for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
            if (last_template == kNoNode && node_name(*it) == "template") {
                last_template = *it;
            }
            if (last_table == kNoNode && node_name(*it) == "table") {
                last_table = *it;
            }
        }
        if (last_template != kNoNode &&
            (last_table == kNoNode || stack_position(last_template) > stack_position(last_table))) {
            return last_template;
        }
        if (last_table == kNoNode) {
            return open_elements_.empty() ? body_ : open_elements_.front();
        }
        table_for_foster_ = last_table;
        const std::size_t parent = result_->nodes[last_table].parent;
        if (parent != kNoNode) {
            return parent;
        }
        const std::size_t pos = stack_position(last_table);
        if (pos != kNoNode && pos > 0U) {
            return open_elements_[pos - 1U];
        }
        return body_;
    }

    std::size_t stack_position(std::size_t node) const {
        const auto it = std::find(open_elements_.begin(), open_elements_.end(), node);
        return it == open_elements_.end()
            ? kNoNode
            : static_cast<std::size_t>(it - open_elements_.begin());
    }

    std::size_t active_position(std::size_t node) const {
        const auto it = std::find(active_formatting_.begin(), active_formatting_.end(), node);
        return it == active_formatting_.end()
            ? kNoNode
            : static_cast<std::size_t>(it - active_formatting_.begin());
    }

    std::size_t find_active_by_name(std::string_view name) const {
        for (auto it = active_formatting_.rbegin(); it != active_formatting_.rend(); ++it) {
            if (*it == kNoNode) {
                break;
            }
            if (node_name(*it) == name) {
                return *it;
            }
        }
        return kNoNode;
    }

