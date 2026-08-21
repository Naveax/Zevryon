#include "zenith_linux_memory_scope.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace zevryon::massivedoc {
namespace {

constexpr std::uint64_t kQ16One = 65'536U;
constexpr std::uint64_t kPercentMicros = 1'000'000U;
constexpr std::uint64_t kHundredPercentMicros = 100U * kPercentMicros;

std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1U);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1U);
    }
    return value;
}

bool parse_u64(std::string_view text, std::uint64_t* value) noexcept {
    if (value == nullptr) {
        return false;
    }
    text = trim_ascii(text);
    if (text.empty()) {
        return false;
    }
    std::uint64_t result = 0U;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

bool safe_cgroup_path(std::string_view path) noexcept {
    if (path.empty() || path.front() != '/' || path.size() > 4096U) {
        return false;
    }
    std::size_t start = 1U;
    while (start <= path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string_view segment =
            end == std::string_view::npos
                ? path.substr(start)
                : path.substr(start, end - start);
        if (segment == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return path.find('\0') == std::string_view::npos;
}

bool parse_percent_q16(std::string_view text, std::uint32_t* value) noexcept {
    if (value == nullptr) {
        return false;
    }
    text = trim_ascii(text);
    if (text.empty()) {
        return false;
    }

    const std::size_t dot = text.find('.');
    const std::string_view whole_text =
        dot == std::string_view::npos ? text : text.substr(0U, dot);
    const std::string_view fraction_text =
        dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1U);

    std::uint64_t whole = 0U;
    if (!parse_u64(whole_text, &whole) || whole > 100U) {
        return false;
    }
    if (fraction_text.size() > 6U) {
        return false;
    }

    std::uint64_t fraction = 0U;
    std::uint64_t scale = 1U;
    for (const char ch : fraction_text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        fraction = fraction * 10U + static_cast<std::uint64_t>(ch - '0');
        scale *= 10U;
    }

    std::uint64_t micros = whole * kPercentMicros;
    if (!fraction_text.empty()) {
        micros += (fraction * kPercentMicros) / scale;
    }
    if (micros > kHundredPercentMicros) {
        return false;
    }

    const std::uint64_t scaled =
        (micros * kQ16One + (kHundredPercentMicros / 2U)) /
        kHundredPercentMicros;
    *value = static_cast<std::uint32_t>(std::min<std::uint64_t>(scaled, kQ16One));
    return true;
}

bool parse_avg10_line(
    std::string_view line,
    std::string_view prefix,
    std::uint32_t* value) noexcept {
    line = trim_ascii(line);
    if (!line.starts_with(prefix)) {
        return false;
    }

    std::size_t cursor = prefix.size();
    while (cursor < line.size()) {
        while (cursor < line.size() &&
               std::isspace(static_cast<unsigned char>(line[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= line.size()) {
            break;
        }
        const std::size_t end = line.find(' ', cursor);
        const std::string_view token =
            end == std::string_view::npos
                ? line.substr(cursor)
                : line.substr(cursor, end - cursor);
        constexpr std::string_view key = "avg10=";
        if (token.starts_with(key)) {
            return parse_percent_q16(token.substr(key.size()), value);
        }
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1U;
    }
    return false;
}

} // namespace

bool parse_linux_cgroup_v2_path(
    std::string_view cgroup_text,
    std::string* relative_path) {
    if (relative_path == nullptr) {
        return false;
    }

    std::size_t cursor = 0U;
    while (cursor <= cgroup_text.size()) {
        const std::size_t end = cgroup_text.find('\n', cursor);
        std::string_view line =
            end == std::string_view::npos
                ? cgroup_text.substr(cursor)
                : cgroup_text.substr(cursor, end - cursor);
        line = trim_ascii(line);
        constexpr std::string_view prefix = "0::";
        if (line.starts_with(prefix)) {
            const std::string_view path = line.substr(prefix.size());
            if (!safe_cgroup_path(path)) {
                return false;
            }
            *relative_path = std::string(path);
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1U;
    }
    return false;
}

bool parse_linux_cgroup_memory_values(
    std::string_view current_text,
    std::string_view max_text,
    ZenithLinuxMemoryScopeObservation* observation) noexcept {
    if (observation == nullptr) {
        return false;
    }

    std::uint64_t current = 0U;
    if (!parse_u64(current_text, &current)) {
        return false;
    }

    ZenithLinuxMemoryScopeObservation result = *observation;
    result.cgroup_v2_detected = true;
    result.cgroup_current_bytes = current;

    max_text = trim_ascii(max_text);
    if (max_text == "max") {
        result.cgroup_v2_limited = false;
        result.cgroup_limit_bytes = 0U;
        *observation = result;
        return true;
    }

    std::uint64_t limit = 0U;
    if (!parse_u64(max_text, &limit) || limit == 0U) {
        return false;
    }
    result.cgroup_v2_limited = true;
    result.cgroup_limit_bytes = limit;
    *observation = result;
    return true;
}

bool parse_linux_memory_psi(
    std::string_view pressure_text,
    ZenithLinuxMemoryScopeObservation* observation) noexcept {
    if (observation == nullptr) {
        return false;
    }

    bool some_found = false;
    bool full_found = false;
    std::uint32_t some_q16 = 0U;
    std::uint32_t full_q16 = 0U;

    std::size_t cursor = 0U;
    while (cursor <= pressure_text.size()) {
        const std::size_t end = pressure_text.find('\n', cursor);
        const std::string_view line =
            end == std::string_view::npos
                ? pressure_text.substr(cursor)
                : pressure_text.substr(cursor, end - cursor);
        if (trim_ascii(line).starts_with("some")) {
            some_found = parse_avg10_line(line, "some", &some_q16);
            if (!some_found) {
                return false;
            }
        } else if (trim_ascii(line).starts_with("full")) {
            full_found = parse_avg10_line(line, "full", &full_q16);
            if (!full_found) {
                return false;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1U;
    }

    if (!some_found && !full_found) {
        return false;
    }
    observation->psi_available = true;
    observation->psi_some_avg10_q16 = some_q16;
    observation->psi_full_avg10_q16 = full_q16;
    return true;
}

bool apply_linux_cgroup_memory_scope(
    std::uint64_t host_total_bytes,
    std::uint64_t host_available_bytes,
    const ZenithLinuxMemoryScopeObservation& observation,
    std::uint64_t* effective_total_bytes,
    std::uint64_t* effective_available_bytes) noexcept {
    if (effective_total_bytes == nullptr || effective_available_bytes == nullptr ||
        host_total_bytes == 0U || host_available_bytes > host_total_bytes) {
        return false;
    }

    std::uint64_t total = host_total_bytes;
    std::uint64_t available = host_available_bytes;
    if (observation.cgroup_v2_detected && observation.cgroup_v2_limited) {
        if (observation.cgroup_limit_bytes == 0U) {
            return false;
        }
        total = std::min(host_total_bytes, observation.cgroup_limit_bytes);
        const std::uint64_t cgroup_available =
            observation.cgroup_current_bytes >= observation.cgroup_limit_bytes
                ? 0U
                : observation.cgroup_limit_bytes - observation.cgroup_current_bytes;
        available = std::min({host_available_bytes, cgroup_available, total});
    }

    *effective_total_bytes = total;
    *effective_available_bytes = available;
    return true;
}

} // namespace zevryon::massivedoc
