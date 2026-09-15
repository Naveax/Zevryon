        if (name == "br") {
            tree_error("unexpected-br-end-tag");
            HtmlTokenizerV1Token synthetic;
            synthetic.kind = HtmlTokenizerV1TokenKind::StartTag;
            synthetic.name = "br";
            return process_in_body(synthetic);
        }
        return generic_end_tag(name);
    }

    bool process_in_table(const HtmlTokenizerV1Token& token) {
        if (token.kind == HtmlTokenizerV1TokenKind::Character && is_whitespace(token.data)) {
            return append_text(token.data);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::Comment) {
            return append_comment(token.data);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag) {
            const auto& name = token.name;
            if (name == "caption") {
                if (!insert_element(token)) return false;
                mode_ = InsertionMode::InBody;
                return true;
            }
            if (name == "colgroup") {
                if (!insert_element(token)) return false;
                mode_ = InsertionMode::InTableBody;
                return true;
            }
            if (name == "col") {
                HtmlTokenizerV1Token group;
                group.kind = HtmlTokenizerV1TokenKind::StartTag;
                group.name = "colgroup";
                if (!insert_element(group)) return false;
                mode_ = InsertionMode::InTableBody;
                return process(token, true);
            }
            if (equals_any(name, {"tbody", "tfoot", "thead"})) {
                if (!insert_element(token)) return false;
                mode_ = InsertionMode::InTableBody;
                return true;
            }
            if (equals_any(name, {"td", "th", "tr"})) {
                HtmlTokenizerV1Token tbody;
                tbody.kind = HtmlTokenizerV1TokenKind::StartTag;
                tbody.name = "tbody";
                if (!insert_element(tbody)) return false;
                mode_ = InsertionMode::InTableBody;
                return process(token, true);
            }
            if (name == "table") {
                tree_error("unexpected-table-start-tag");
                HtmlTokenizerV1Token end;
                end.kind = HtmlTokenizerV1TokenKind::EndTag;
                end.name = "table";
                if (!process_in_table(end)) return false;
                return process(token, true);
            }
            if (equals_any(name, {"input", "form"}) && name == "form") {
                return process_in_body(token);
            }
        }
        if (token.kind == HtmlTokenizerV1TokenKind::EndTag && token.name == "table") {
            const std::size_t table = find_open_by_name("table");
            if (table == kNoNode) {
                tree_error("unexpected-table-end-tag");
                return true;
            }
            open_elements_.resize(stack_position(table));
            reset_insertion_mode();
            return true;
        }
        if (token.kind == HtmlTokenizerV1TokenKind::EndTag &&
            equals_any(token.name, {"body", "caption", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"})) {
            tree_error("unexpected-end-tag-in-table");
            return true;
        }
        tree_error("unexpected-start-tag-implies-table-voodoo");
        const bool old = foster_parenting_;
        foster_parenting_ = true;
        const bool ok = process_in_body(token);
        foster_parenting_ = old;
        return ok;
    }

    bool process_in_table_body(const HtmlTokenizerV1Token& token) {
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag && token.name == "tr") {
            if (!insert_element(token)) return false;
            mode_ = InsertionMode::InRow;
            return true;
        }
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag &&
            equals_any(token.name, {"td", "th"})) {
            HtmlTokenizerV1Token tr;
            tr.kind = HtmlTokenizerV1TokenKind::StartTag;
            tr.name = "tr";
            if (!insert_element(tr)) return false;
            mode_ = InsertionMode::InRow;
            return process(token, true);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::EndTag &&
            equals_any(token.name, {"tbody", "tfoot", "thead"})) {
            if (!in_scope_name(token.name)) {
                tree_error("unexpected-table-body-end-tag");
                return true;
            }
            pop_until(token.name);
            mode_ = InsertionMode::InTable;
            return true;
        }
        if ((token.kind == HtmlTokenizerV1TokenKind::StartTag &&
             equals_any(token.name, {"caption", "col", "colgroup", "tbody", "tfoot", "thead"})) ||
            (token.kind == HtmlTokenizerV1TokenKind::EndTag && token.name == "table")) {
            for (const auto section : {"tbody", "thead", "tfoot"}) {
                const auto node = find_open_by_name(section);
                if (node != kNoNode) {
                    open_elements_.resize(stack_position(node));
                    break;
                }
            }
            mode_ = InsertionMode::InTable;
            return process(token, true);
        }
        return process_in_table(token);
    }

    bool process_in_row(const HtmlTokenizerV1Token& token) {
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag &&
            equals_any(token.name, {"td", "th"})) {
            if (!insert_element(token)) return false;
            mode_ = InsertionMode::InCell;
            active_formatting_.push_back(kNoNode);
            return true;
        }
        if (token.kind == HtmlTokenizerV1TokenKind::EndTag && token.name == "tr") {
            if (!in_scope_name("tr")) {
                tree_error("unexpected-tr-end-tag");
                return true;
            }
            pop_until("tr");
            mode_ = InsertionMode::InTableBody;
            return true;
        }
        if ((token.kind == HtmlTokenizerV1TokenKind::StartTag &&
             equals_any(token.name, {"caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr"})) ||
            (token.kind == HtmlTokenizerV1TokenKind::EndTag && token.name == "table")) {
            HtmlTokenizerV1Token end;
            end.kind = HtmlTokenizerV1TokenKind::EndTag;
            end.name = "tr";
            if (!process_in_row(end)) return false;
            return process(token, true);
        }
        return process_in_table(token);
    }

    bool clear_active_to_marker() {
        while (!active_formatting_.empty()) {
            const std::size_t value = active_formatting_.back();
            active_formatting_.pop_back();
            if (value == kNoNode) {
                break;
            }
        }
        return true;
    }

    bool process_in_cell(const HtmlTokenizerV1Token& token) {
        if (token.kind == HtmlTokenizerV1TokenKind::EndTag &&
            equals_any(token.name, {"td", "th"})) {
            if (!in_scope_name(token.name)) {
                tree_error("unexpected-cell-end-tag");
                return true;
            }
            pop_until(token.name);
            clear_active_to_marker();
            mode_ = InsertionMode::InRow;
            return true;
        }
        if ((token.kind == HtmlTokenizerV1TokenKind::StartTag &&
             equals_any(token.name, {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"})) ||
            (token.kind == HtmlTokenizerV1TokenKind::EndTag &&
             equals_any(token.name, {"table", "tbody", "tfoot", "thead", "tr"}))) {
            const std::string cell = in_scope_name("td") ? "td" : "th";
            HtmlTokenizerV1Token end;
            end.kind = HtmlTokenizerV1TokenKind::EndTag;
            end.name = cell;
            if (!process_in_cell(end)) return false;
            return process(token, true);
        }
        return process_in_body(token);
    }

    bool process_in_select(const HtmlTokenizerV1Token& token) {
        if (token.kind == HtmlTokenizerV1TokenKind::Character) {
            return append_text(token.data);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::Comment) {
            return append_comment(token.data);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag && token.name == "option") {
            if (node_name(current_node()) == "option") {
                open_elements_.pop_back();
            }
            return insert_element(token);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag && token.name == "optgroup") {
            if (node_name(current_node()) == "option") open_elements_.pop_back();
            if (node_name(current_node()) == "optgroup") open_elements_.pop_back();
            return insert_element(token);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::EndTag && token.name == "option") {
            if (node_name(current_node()) == "option") open_elements_.pop_back();
            else tree_error("unexpected-option-end-tag");
            return true;
        }
        if (token.kind == HtmlTokenizerV1TokenKind::EndTag && token.name == "select") {
            const auto select = find_open_by_name("select");
            if (select == kNoNode) {
                tree_error("unexpected-select-end-tag");
                return true;
            }
            open_elements_.resize(stack_position(select));
            reset_insertion_mode();
            return true;
        }
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag && token.name == "select") {
            tree_error("unexpected-select-start-tag");
            HtmlTokenizerV1Token end;
            end.kind = HtmlTokenizerV1TokenKind::EndTag;
            end.name = "select";
            return process_in_select(end);
        }
        tree_error("unexpected-token-in-select");
        return true;
    }

    bool process_after_body(const HtmlTokenizerV1Token& token) {
        if (token.kind == HtmlTokenizerV1TokenKind::Character && is_whitespace(token.data)) {
            mode_ = InsertionMode::InBody;
            const bool ok = process_in_body(token);
            mode_ = InsertionMode::AfterBody;
            return ok;
        }
        if (token.kind == HtmlTokenizerV1TokenKind::Comment) {
            const std::size_t node = append_raw_node(HtmlTreeBuilderV1NodeKind::Comment, "#comment", kNoNode);
            if (node == kNoNode) return false;
            result_->nodes[node].data = token.data;
            return attach(node, html_);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag && token.name == "html") {
            return process_in_body(token);
        }
        tree_error("unexpected-token-after-body");
        mode_ = InsertionMode::InBody;
        return process(token, true);
    }

    bool scripting_enabled_{false};
    HtmlTreeBuilderV1Config config_{};
    HtmlTreeBuilderV1Result* result_{nullptr};
    std::string* error_{nullptr};
    std::size_t document_{kNoNode};
    std::size_t html_{kNoNode};
    std::size_t head_{kNoNode};
    std::size_t body_{kNoNode};
    std::size_t form_element_{kNoNode};
    std::vector<std::size_t> open_elements_;
    std::vector<std::size_t> active_formatting_;
    InsertionMode mode_{InsertionMode::InBody};
    bool foster_parenting_{false};
    std::size_t table_for_foster_{kNoNode};
};

} // namespace

bool build_html_tree_v1(
    std::string_view input,
    bool scripting_enabled,
    HtmlTreeBuilderV1Config config,
    HtmlTreeBuilderV1Result* result,
    std::string* error) {
    if (result == nullptr || error == nullptr) {
        return false;
    }
    Builder builder(scripting_enabled, config, result, error);
    if (!builder.begin(input)) {
        return false;
    }
    HtmlTokenizerV1Config tokenizer_config;
    tokenizer_config.maximum_input_bytes = config.maximum_input_bytes;
    tokenizer_config.maximum_token_bytes = config.maximum_token_bytes;
    HtmlTokenizerV1Stats tokenizer_stats;
    return tokenize_html_token_stream_v1(
        input,
        HtmlTokenizerV1InitialState::Data,
        {},
        tokenizer_config,
        &builder,
        &tokenizer_stats,
        error);
}

} // namespace zevryon::massivedoc
