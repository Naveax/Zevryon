    std::size_t find_open_by_name(std::string_view name) const {
        for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
            if (node_name(*it) == name) {
                return *it;
            }
        }
        return kNoNode;
    }

    bool in_scope(std::size_t node) const {
        if (node == kNoNode) {
            return false;
        }
        for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
            if (*it == node) {
                return true;
            }
            if (equals_any(node_name(*it), {
                    "applet", "caption", "html", "table", "td", "th", "marquee",
                    "object", "template"})) {
                return false;
            }
        }
        return false;
    }

    bool in_scope_name(std::string_view name) const {
        return in_scope(find_open_by_name(name));
    }

    bool in_button_scope(std::string_view name) const {
        for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
            if (node_name(*it) == name) {
                return true;
            }
            if (equals_any(node_name(*it), {
                    "applet", "caption", "html", "table", "td", "th", "marquee",
                    "object", "template", "button"})) {
                return false;
            }
        }
        return false;
    }

    void tree_error(std::string code) {
        if (result_->parse_errors.size() >= config_.maximum_nodes) {
            return;
        }
        result_->parse_errors.push_back(
            HtmlTreeBuilderV1ParseError{std::move(code), 1U, 1U, false});
        ++result_->stats.tree_parse_errors;
    }

    bool append_text(std::string_view data) {
        if (data.empty()) {
            return true;
        }
        const std::size_t parent = appropriate_parent();
        if (parent >= result_->nodes.size()) {
            return fail(error_, "HTML tree builder text parent is invalid");
        }
        auto& children = result_->nodes[parent].children;
        if (!children.empty()) {
            const std::size_t last = children.back();
            if (last < result_->nodes.size() &&
                result_->nodes[last].kind == HtmlTreeBuilderV1NodeKind::Text) {
                result_->nodes[last].data.append(data);
                result_->stats.text_bytes += static_cast<std::uint64_t>(data.size());
                return true;
            }
        }
        const std::size_t node = append_raw_node(HtmlTreeBuilderV1NodeKind::Text, "#text", kNoNode);
        if (node == kNoNode) {
            return false;
        }
        result_->nodes[node].data.assign(data);
        result_->stats.text_bytes += static_cast<std::uint64_t>(data.size());
        return attach(node, parent, foster_parenting_ && table_for_foster_ != kNoNode);
    }

    bool append_comment(std::string_view data) {
        const std::size_t parent = current_node();
        const std::size_t node = append_raw_node(HtmlTreeBuilderV1NodeKind::Comment, "#comment", kNoNode);
        if (node == kNoNode) {
            return false;
        }
        result_->nodes[node].data.assign(data);
        return attach(node, parent);
    }

    bool insert_element(const HtmlTokenizerV1Token& token, bool formatting = false) {
        const std::size_t parent = appropriate_parent();
        const std::size_t node = append_element(
            token.name, parent, token.attributes,
            foster_parenting_ && table_for_foster_ != kNoNode);
        if (node == kNoNode) {
            return false;
        }
        if (!token.self_closing && !is_void(token.name)) {
            if (!push_open(node)) {
                return false;
            }
        }
        if (formatting) {
            if (!add_active(node)) {
                return false;
            }
        }
        return true;
    }

    bool clone_element(std::size_t original, std::size_t parent, std::size_t* output) {
        if (original >= result_->nodes.size() || output == nullptr) {
            return fail(error_, "HTML tree builder clone source is invalid");
        }
        const auto& src = result_->nodes[original];
        const std::size_t clone = append_element(src.name, parent, src.attributes);
        if (clone == kNoNode) {
            return false;
        }
        result_->nodes[clone].name_space = src.name_space;
        *output = clone;
        return true;
    }

    bool reconstruct_active_formatting() {
        if (active_formatting_.empty()) {
            return true;
        }
        std::size_t index = active_formatting_.size();
        while (index > 0U) {
            const std::size_t entry = active_formatting_[index - 1U];
            if (entry == kNoNode || stack_position(entry) != kNoNode) {
                break;
            }
            --index;
        }
        for (; index < active_formatting_.size(); ++index) {
            const std::size_t entry = active_formatting_[index];
            if (entry == kNoNode || entry >= result_->nodes.size()) {
                continue;
            }
            std::size_t clone = kNoNode;
            if (!clone_element(entry, appropriate_parent(), &clone)) {
                return false;
            }
            if (!push_open(clone)) {
                return false;
            }
            active_formatting_[index] = clone;
        }
        return true;
    }

    bool close_p_if_open() {
        if (!in_button_scope("p")) {
            return true;
        }
        return generic_end_tag("p");
    }

    bool generic_end_tag(std::string_view name) {
        for (std::size_t pos = open_elements_.size(); pos > 0U; --pos) {
            const std::size_t node = open_elements_[pos - 1U];
            if (node_name(node) == name) {
                open_elements_.resize(pos - 1U);
                if (open_elements_.empty()) {
                    open_elements_.push_back(html_);
                    open_elements_.push_back(body_);
                }
                return true;
            }
            if (is_special(node_name(node))) {
                tree_error("unexpected-end-tag");
                return true;
            }
        }
        tree_error("unexpected-end-tag");
        return true;
    }

    bool pop_until(std::string_view name) {
        const std::size_t node = find_open_by_name(name);
        if (node == kNoNode) {
            tree_error("unexpected-end-tag");
            return true;
        }
        const std::size_t pos = stack_position(node);
        if (pos == kNoNode) {
            return true;
        }
        open_elements_.resize(pos);
        return true;
    }

    void reset_insertion_mode() {
        mode_ = InsertionMode::InBody;
        for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
            const auto name = node_name(*it);
            if (name == "select") {
                mode_ = InsertionMode::InSelect;
                return;
            }
            if (name == "td" || name == "th") {
                mode_ = InsertionMode::InCell;
                return;
            }
            if (name == "tr") {
                mode_ = InsertionMode::InRow;
                return;
            }
            if (name == "tbody" || name == "thead" || name == "tfoot") {
                mode_ = InsertionMode::InTableBody;
                return;
            }
            if (name == "table") {
                mode_ = InsertionMode::InTable;
                return;
            }
        }
    }

    bool adoption_agency(std::string_view subject) {
        if (node_name(current_node()) == subject &&
            find_active_by_name(subject) == kNoNode) {
            open_elements_.pop_back();
            return true;
        }

        for (unsigned outer = 0U; outer < 8U; ++outer) {
            const std::size_t formatting = find_active_by_name(subject);
            if (formatting == kNoNode) {
                return generic_end_tag(subject);
            }
            const std::size_t formatting_stack = stack_position(formatting);
            if (formatting_stack == kNoNode) {
                tree_error("adoption-agency-formatting-not-open");
                active_formatting_.erase(active_formatting_.begin() +
                    static_cast<std::ptrdiff_t>(active_position(formatting)));
                return true;
            }
            if (!in_scope(formatting)) {
                tree_error("adoption-agency-formatting-not-in-scope");
                return true;
            }
            if (formatting != current_node()) {
                tree_error("adoption-agency-formatting-not-current");
            }

            std::size_t furthest = kNoNode;
            for (std::size_t pos = formatting_stack + 1U; pos < open_elements_.size(); ++pos) {
                if (is_special(node_name(open_elements_[pos]))) {
                    furthest = open_elements_[pos];
                    break;
                }
            }
            if (furthest == kNoNode) {
                open_elements_.resize(formatting_stack);
                const std::size_t active = active_position(formatting);
                if (active != kNoNode) {
                    active_formatting_.erase(active_formatting_.begin() +
                        static_cast<std::ptrdiff_t>(active));
                }
                return true;
            }

            const std::size_t common_ancestor = formatting_stack == 0U
                ? body_
                : open_elements_[formatting_stack - 1U];
            std::size_t bookmark = active_position(formatting);
            std::size_t last_node = furthest;
            std::size_t node_pos = stack_position(furthest);
            if (node_pos == kNoNode || node_pos == 0U) {
                return fail(error_, "HTML adoption-agency stack topology is invalid");
            }
            unsigned inner_counter = 0U;

            for (;;) {
                ++inner_counter;
                if (node_pos == 0U) {
                    return fail(error_, "HTML adoption-agency stack topology is invalid");
                }
                --node_pos;
                const std::size_t node = open_elements_[node_pos];
                if (node == formatting) {
                    break;
                }
                std::size_t active_pos = active_position(node);
                if (inner_counter > 3U && active_pos != kNoNode) {
                    active_formatting_.erase(active_formatting_.begin() +
                        static_cast<std::ptrdiff_t>(active_pos));
                    active_pos = kNoNode;
                }
                if (active_pos == kNoNode) {
                    open_elements_.erase(open_elements_.begin() +
                        static_cast<std::ptrdiff_t>(node_pos));
                    continue;
                }

                std::size_t replacement = kNoNode;
                if (!clone_element(node, kNoNode, &replacement)) {
                    return false;
                }
                active_formatting_[active_pos] = replacement;
                open_elements_[node_pos] = replacement;
                if (last_node == furthest) {
                    bookmark = active_pos + 1U;
                }
                if (!attach(last_node, replacement)) {
                    return false;
                }
                last_node = replacement;
            }

            bool old_foster = foster_parenting_;
            foster_parenting_ = equals_any(node_name(common_ancestor), {
                "table", "tbody", "tfoot", "thead", "tr"});
            if (!attach(last_node, foster_parenting_ ? appropriate_parent() : common_ancestor,
                        foster_parenting_ && table_for_foster_ != kNoNode)) {
                foster_parenting_ = old_foster;
