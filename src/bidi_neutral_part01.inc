    }
    return false;
}

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

bool valid_bidi_class(BidiClass value) noexcept {
    return static_cast<std::size_t>(value) <
           static_cast<std::size_t>(BidiClass::Count);
}

BidiClass strong_direction(BidiClass value) noexcept {
    if (value == BidiClass::L) {
        return BidiClass::L;
    }
    if (value == BidiClass::R ||
        value == BidiClass::EN ||
        value == BidiClass::AN) {
        return BidiClass::R;
    }
    return BidiClass::Count;
}

bool is_neutral_or_isolate(BidiClass value) noexcept {
    return value == BidiClass::B ||
           value == BidiClass::S ||
           value == BidiClass::WS ||
           value == BidiClass::ON ||
           value == BidiClass::FSI ||
           value == BidiClass::LRI ||
           value == BidiClass::RLI ||
           value == BidiClass::PDI;
}

bool validate_topology(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const BidiExplicitUnit> units,
    const BidiSequenceTopology& topology,
    std::span<const BidiClass> weak_types,
    std::pmr::memory_resource* resource,
    BidiNeutralError* error) {
    const std::size_t active_count = topology.active_unit_indices.size();
    if (weak_types.size() != active_count ||
        active_count > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            BidiNeutralErrorKind::InvalidInput,
            0U,
            "weak types do not match the 32-bit active-unit stream",
            error);
    }

    for (std::size_t active = 0U; active < active_count; ++active) {
        const std::uint32_t unit_index = topology.active_unit_indices[active];
        if (unit_index >= units.size() ||
            (active != 0U &&
             topology.active_unit_indices[active - 1U] >= unit_index) ||
            (units[unit_index].flags & kBidiUnitRemovedByX9) != 0U ||
            units[unit_index].codepoint_index >= codepoints.size() ||
            !valid_bidi_class(weak_types[active])) {
            return fail(
                BidiNeutralErrorKind::InvalidInput,
                active,
                "active indices, codepoints, or weak types are invalid",
                error);
        }
    }

    std::size_t expected_active = 0U;
    for (std::size_t run_index = 0U;
         run_index < topology.level_runs.size();
         ++run_index) {
        const BidiLevelRun& run = topology.level_runs[run_index];
        if (run.active_count == 0U ||
            run.first_active_index != expected_active ||
            static_cast<std::size_t>(run.first_active_index) +
                    static_cast<std::size_t>(run.active_count) >
                active_count) {
            return fail(
                BidiNeutralErrorKind::TopologyViolation,
                expected_active,
                "level runs do not form a contiguous active partition",
                error);
        }
        const std::size_t end =
            static_cast<std::size_t>(run.first_active_index) + run.active_count;
        for (std::size_t active = run.first_active_index;
             active < end;
             ++active) {
            if (units[topology.active_unit_indices[active]].level != run.level) {
                return fail(
                    BidiNeutralErrorKind::TopologyViolation,
                    active,
                    "level-run level disagrees with explicit units",
                    error);
            }
        }
        expected_active = end;
    }
    if (expected_active != active_count ||
        topology.sequence_run_indices.size() != topology.level_runs.size()) {
