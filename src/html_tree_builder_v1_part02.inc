                return false;
            }
            foster_parenting_ = old_foster;

            std::size_t replacement_formatting = kNoNode;
            if (!clone_element(formatting, furthest, &replacement_formatting)) {
                return false;
            }
            std::vector<std::size_t> old_children = result_->nodes[furthest].children;
            for (const std::size_t child : old_children) {
                if (child == replacement_formatting) {
                    continue;
                }
                if (!attach(child, replacement_formatting)) {
                    return false;
                }
            }
            // Ensure the reconstructed formatting element is the last child.
            detach(replacement_formatting);
            if (!attach(replacement_formatting, furthest)) {
                return false;
            }

            const std::size_t formatting_active = active_position(formatting);
            if (formatting_active != kNoNode) {
                active_formatting_.erase(active_formatting_.begin() +
                    static_cast<std::ptrdiff_t>(formatting_active));
                if (bookmark > formatting_active && bookmark != 0U) {
                    --bookmark;
                }
            }
            bookmark = std::min(bookmark, active_formatting_.size());
            active_formatting_.insert(
                active_formatting_.begin() + static_cast<std::ptrdiff_t>(bookmark),
                replacement_formatting);

            const std::size_t formatting_open = stack_position(formatting);
            if (formatting_open != kNoNode) {
                open_elements_.erase(open_elements_.begin() +
                    static_cast<std::ptrdiff_t>(formatting_open));
            }
            const std::size_t furthest_open = stack_position(furthest);
            if (furthest_open == kNoNode) {
                return fail(error_, "HTML adoption-agency furthest block vanished");
            }
            open_elements_.insert(
                open_elements_.begin() + static_cast<std::ptrdiff_t>(furthest_open + 1U),
                replacement_formatting);
        }
        return true;
    }

    bool merge_attributes(std::size_t node,
                          const std::vector<HtmlTokenizerV1Attribute>& attributes) {
        if (node >= result_->nodes.size()) {
            return fail(error_, "HTML tree builder attribute merge target is invalid");
        }
        auto& existing = result_->nodes[node].attributes;
        for (const auto& attribute : attributes) {
            const auto found = std::find_if(existing.begin(), existing.end(), [&](const auto& value) {
                return value.name == attribute.name;
            });
            if (found == existing.end()) {
                existing.push_back(attribute);
            }
        }
        return true;
    }

    bool process(const HtmlTokenizerV1Token& token, bool reprocess) {
        (void)reprocess;
        switch (mode_) {
        case InsertionMode::InBody:
            return process_in_body(token);
        case InsertionMode::InTable:
            return process_in_table(token);
        case InsertionMode::InTableBody:
            return process_in_table_body(token);
        case InsertionMode::InRow:
            return process_in_row(token);
        case InsertionMode::InCell:
            return process_in_cell(token);
        case InsertionMode::InSelect:
            return process_in_select(token);
        case InsertionMode::AfterBody:
            return process_after_body(token);
        }
        return fail(error_, "HTML tree builder insertion mode is invalid");
    }

    bool process_in_body(const HtmlTokenizerV1Token& token) {
        if (token.kind == HtmlTokenizerV1TokenKind::Character) {
            if (!reconstruct_active_formatting()) {
                return false;
            }
            return append_text(token.data);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::Comment) {
            return append_comment(token.data);
        }
        if (token.kind == HtmlTokenizerV1TokenKind::Doctype) {
            tree_error("unexpected-doctype");
            return true;
        }
        if (token.kind == HtmlTokenizerV1TokenKind::StartTag) {
            const auto& name = token.name;
            if (name == "html") {
                return merge_attributes(html_, token.attributes);
            }
            if (name == "head") {
                return merge_attributes(head_, token.attributes);
            }
            if (name == "body") {
                return merge_attributes(body_, token.attributes);
            }
            if (is_block_start(name)) {
                if (!close_p_if_open()) {
                    return false;
                }
                return insert_element(token);
            }
            if (equals_any(name, {"h1", "h2", "h3", "h4", "h5", "h6"})) {
                if (!close_p_if_open()) {
                    return false;
                }
                if (equals_any(node_name(current_node()), {"h1", "h2", "h3", "h4", "h5", "h6"})) {
                    open_elements_.pop_back();
                }
                return insert_element(token);
            }
            if (name == "pre" || name == "listing") {
                if (!close_p_if_open()) {
                    return false;
                }
                return insert_element(token);
            }
            if (name == "form") {
                if (form_element_ != kNoNode) {
                    tree_error("unexpected-form-start-tag");
                    return true;
                }
                if (!close_p_if_open() || !insert_element(token)) {
                    return false;
                }
                form_element_ = current_node();
                return true;
            }
            if (name == "li") {
                for (std::size_t pos = open_elements_.size(); pos > 0U; --pos) {
                    const auto candidate = node_name(open_elements_[pos - 1U]);
                    if (candidate == "li") {
                        open_elements_.resize(pos - 1U);
                        break;
                    }
                    if (is_special(candidate) && !equals_any(candidate, {"address", "div", "p"})) {
                        break;
                    }
                }
                if (!close_p_if_open()) {
                    return false;
                }
                return insert_element(token);
            }
            if (name == "dd" || name == "dt") {
                if (!close_p_if_open()) {
                    return false;
                }
                return insert_element(token);
            }
            if (name == "plaintext") {
                if (!close_p_if_open()) {
                    return false;
                }
                return insert_element(token);
            }
            if (name == "button") {
                if (in_scope_name("button")) {
                    tree_error("unexpected-button-start-tag");
                    generic_end_tag("button");
                }
                if (!reconstruct_active_formatting()) {
                    return false;
                }
                return insert_element(token);
            }
            if (name == "a") {
                if (find_active_by_name("a") != kNoNode) {
                    tree_error("unexpected-start-tag-implies-end-tag");
                    if (!adoption_agency("a")) {
                        return false;
                    }
                }
                if (!reconstruct_active_formatting()) {
                    return false;
                }
                return insert_element(token, true);
            }
            if (is_formatting(name)) {
                if (name == "nobr" && in_scope_name("nobr")) {
                    tree_error("unexpected-start-tag-implies-end-tag");
                    if (!adoption_agency("nobr") || !reconstruct_active_formatting()) {
                        return false;
                    }
                } else if (!reconstruct_active_formatting()) {
                    return false;
                }
                return insert_element(token, true);
            }
            if (equals_any(name, {"applet", "marquee", "object"})) {
                if (!reconstruct_active_formatting() || !insert_element(token)) {
                    return false;
                }
                return add_active(kNoNode);
            }
            if (name == "table") {
                if (!close_p_if_open() || !insert_element(token)) {
                    return false;
                }
                mode_ = InsertionMode::InTable;
                return true;
            }
            if (equals_any(name, {"area", "br", "embed", "img", "keygen", "wbr", "input", "param", "source", "track", "hr"})) {
                if (!reconstruct_active_formatting()) {
                    return false;
                }
                return insert_element(token);
            }
            if (name == "select") {
                if (!reconstruct_active_formatting() || !insert_element(token)) {
                    return false;
                }
                mode_ = InsertionMode::InSelect;
                return true;
            }
            if (name == "option" || name == "optgroup") {
                if (node_name(current_node()) == "option") {
                    open_elements_.pop_back();
                }
                if (name == "optgroup" && node_name(current_node()) == "optgroup") {
                    open_elements_.pop_back();
                }
                if (!reconstruct_active_formatting()) {
                    return false;
                }
                return insert_element(token);
            }
            if (!reconstruct_active_formatting()) {
                return false;
            }
            return insert_element(token);
        }

        const auto& name = token.name;
        if (name == "body" || name == "html") {
            mode_ = InsertionMode::AfterBody;
            return true;
        }
        if (equals_any(name, {"address", "article", "aside", "blockquote", "button", "center", "details", "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "listing", "main", "menu", "nav", "ol", "pre", "search", "section", "summary", "ul"})) {
            if (!in_scope_name(name)) {
                tree_error("unexpected-end-tag");
                return true;
            }
            return pop_until(name);
        }
        if (name == "form") {
            if (form_element_ == kNoNode) {
                tree_error("unexpected-form-end-tag");
                return true;
            }
            const std::size_t pos = stack_position(form_element_);
            if (pos != kNoNode) {
                open_elements_.erase(open_elements_.begin() + static_cast<std::ptrdiff_t>(pos));
            }
            form_element_ = kNoNode;
            return true;
        }
        if (name == "p") {
            if (!in_button_scope("p")) {
                tree_error("unexpected-p-end-tag");
                HtmlTokenizerV1Token synthetic;
                synthetic.kind = HtmlTokenizerV1TokenKind::StartTag;
                synthetic.name = "p";
                if (!insert_element(synthetic)) {
                    return false;
                }
            }
            return pop_until("p");
        }
        if (name == "li" || name == "dd" || name == "dt") {
            return pop_until(name);
        }
        if (equals_any(name, {"h1", "h2", "h3", "h4", "h5", "h6"})) {
            std::size_t found = kNoNode;
            for (const auto heading : {"h1", "h2", "h3", "h4", "h5", "h6"}) {
                const auto candidate = find_open_by_name(heading);
                if (candidate != kNoNode) {
                    found = candidate;
                    break;
                }
            }
            if (found == kNoNode) {
                tree_error("unexpected-heading-end-tag");
                return true;
            }
            open_elements_.resize(stack_position(found));
            return true;
        }
        if (is_formatting(name)) {
            return adoption_agency(name);
        }
        if (equals_any(name, {"applet", "marquee", "object"})) {
            if (!in_scope_name(name)) {
                tree_error("unexpected-end-tag");
                return true;
            }
            if (!pop_until(name)) {
                return false;
            }
            return clear_active_to_marker();
        }
