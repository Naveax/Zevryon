#include "zenith_linux_memory_context.hpp"

#include "zenith_process_memory_pressure.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace zevryon::massivedoc {
namespace {

constexpr std::uint32_t kMaxPsiMilliPercent = 100U * 1000U;
#if defined(__linux__)
constexpr std::size_t kTinyControlFileLimit = 256U;
constexpr std::size_t kPressureFileLimit = 16U * 1024U;
#endif

std::string_view trim(std::string_view value) noexcept {
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
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0U;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parse_milli_percent(
    std::string_view text,
    std::uint32_t* milli_percent) noexcept {
    if (milli_percent == nullptr) {
        return false;
    }
    text = trim(text);
    if (text.empty()) {
        return false;
    }

    std::uint64_t whole = 0U;
    std::size_t index = 0U;
    bool saw_digit = false;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        saw_digit = true;
        const std::uint64_t digit = static_cast<std::uint64_t>(text[index] - '0');
        if (whole > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        whole = whole * 10U + digit;
        ++index;
    }
    if (!saw_digit) {
        return false;
    }

    std::uint32_t fraction = 0U;
    std::uint32_t fraction_digits = 0U;
    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            if (fraction_digits == 3U) {
                return false;
            }
            fraction = fraction * 10U +
                       static_cast<std::uint32_t>(text[index] - '0');
            ++fraction_digits;
            ++index;
        }
    }
    if (index != text.size()) {
        return false;
    }
    while (fraction_digits < 3U) {
        fraction *= 10U;
        ++fraction_digits;
    }
    if (whole > 100U) {
        return false;
    }
    const std::uint64_t scaled = whole * 1000U + fraction;
    if (scaled > kMaxPsiMilliPercent) {
        return false;
    }
    *milli_percent = static_cast<std::uint32_t>(scaled);
    return true;
}

bool psi_avg10(
    std::string_view line,
    std::string_view prefix,
    std::uint32_t* value,
    bool* found,
    std::string* error) {
    line = trim(line);
    if (!line.starts_with(prefix)) {
        return true;
    }
    const std::size_t position = line.find("avg10=");
    if (position == std::string_view::npos) {
        *error = "Linux memory PSI line is missing avg10";
        return false;
    }
    std::string_view token = line.substr(position + 6U);
    const std::size_t end = token.find_first_of(" \t\r");
    if (end != std::string_view::npos) {
        token = token.substr(0U, end);
    }
    if (!parse_milli_percent(token, value)) {
        *error = "Linux memory PSI avg10 is invalid";
        return false;
    }
    *found = true;
    return true;
}

#if defined(__linux__)
bool read_bounded_file(
    const std::filesystem::path& path,
    std::size_t limit,
    std::string* output) {
    if (output == nullptr || limit == 0U) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    output->clear();
    char buffer[1024];
    while (input) {
        input.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            break;
        }
        const auto amount = static_cast<std::size_t>(count);
        if (output->size() > limit || amount > limit - output->size()) {
            output->clear();
            return false;
        }
        output->append(buffer, amount);
    }
    return true;
}

bool resolve_cgroup_v2_directory(std::filesystem::path* directory) {
    if (directory == nullptr) {
        return false;
    }
    std::ifstream input("/proc/self/cgroup");
    if (!input) {
        return false;
    }
    std::string line;
    while (std::getline(input, line)) {
        constexpr std::string_view prefix = "0::";
        if (!std::string_view(line).starts_with(prefix)) {
            continue;
        }
        std::string_view relative = std::string_view(line).substr(prefix.size());
        relative = trim(relative);
        while (!relative.empty() && relative.front() == '/') {
            relative.remove_prefix(1U);
        }
        std::filesystem::path resolved("/sys/fs/cgroup");
        if (!relative.empty()) {
            resolved /= std::filesystem::path(std::string(relative));
        }
        *directory = std::move(resolved);
        return true;
    }
    return false;
}
#endif

} // namespace

bool parse_zenith_cgroup_v2_memory(
    std::string_view memory_max,
    std::string_view memory_current,
    ZenithLinuxMemoryContext* context,
    std::string* error) {
    if (context == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    memory_max = trim(memory_max);
    memory_current = trim(memory_current);
    if (memory_max.empty() || memory_current.empty()) {
        *error = "cgroup v2 memory controls are empty";
        return false;
    }

    std::uint64_t current = 0U;
    if (!parse_u64(memory_current, &current)) {
        *error = "cgroup v2 memory.current is invalid";
        return false;
    }

    context->cgroup_v2_detected = true;
    context->cgroup_current_bytes = current;
    if (memory_max == "max") {
        context->cgroup_v2_limited = false;
        context->cgroup_limit_bytes = 0U;
        return true;
    }

    std::uint64_t limit = 0U;
    if (!parse_u64(memory_max, &limit) || limit == 0U) {
        *error = "cgroup v2 memory.max is invalid";
        return false;
    }
    context->cgroup_v2_limited = true;
    context->cgroup_limit_bytes = limit;
    return true;
}

bool parse_zenith_linux_memory_psi(
    std::string_view pressure_text,
    ZenithLinuxMemoryContext* context,
    std::string* error) {
    if (context == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    bool some_found = false;
    bool full_found = false;
    std::uint32_t some = 0U;
    std::uint32_t full = 0U;

    std::size_t offset = 0U;
    while (offset <= pressure_text.size()) {
        const std::size_t newline = pressure_text.find('\n', offset);
        const std::size_t end = newline == std::string_view::npos
                                    ? pressure_text.size()
                                    : newline;
        const std::string_view line = pressure_text.substr(offset, end - offset);
        if (!psi_avg10(line, "some ", &some, &some_found, error) ||
            !psi_avg10(line, "full ", &full, &full_found, error)) {
            return false;
        }
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1U;
    }
    if (!some_found) {
        *error = "Linux memory PSI some avg10 is missing";
        return false;
    }

    context->psi_available = true;
    context->psi_some_avg10_milli_percent = some;
    context->psi_full_avg10_milli_percent = full_found ? full : 0U;
    return true;
}

bool capture_zenith_linux_memory_context(
    ZenithLinuxMemoryContext* context,
    std::string* error) {
    if (context == nullptr || error == nullptr) {
        return false;
    }
    *context = {};
    error->clear();
#if defined(__linux__)
    std::filesystem::path cgroup_directory;
    if (resolve_cgroup_v2_directory(&cgroup_directory)) {
        std::string maximum;
        std::string current;
        if (read_bounded_file(
                cgroup_directory / "memory.max",
                kTinyControlFileLimit,
                &maximum) &&
            read_bounded_file(
                cgroup_directory / "memory.current",
                kTinyControlFileLimit,
                &current)) {
            ZenithLinuxMemoryContext parsed = *context;
            std::string parse_error;
            if (parse_zenith_cgroup_v2_memory(
                    maximum,
                    current,
                    &parsed,
                    &parse_error)) {
                *context = parsed;
            }
        }

        std::string pressure;
        if (read_bounded_file(
                cgroup_directory / "memory.pressure",
                kPressureFileLimit,
                &pressure)) {
            ZenithLinuxMemoryContext parsed = *context;
            std::string parse_error;
            if (parse_zenith_linux_memory_psi(
                    pressure,
                    &parsed,
                    &parse_error)) {
                *context = parsed;
                return true;
            }
        }
    }

    std::string host_pressure;
    if (read_bounded_file(
            "/proc/pressure/memory",
            kPressureFileLimit,
            &host_pressure)) {
        ZenithLinuxMemoryContext parsed = *context;
        std::string parse_error;
        if (parse_zenith_linux_memory_psi(
                host_pressure,
                &parsed,
                &parse_error)) {
            *context = parsed;
        }
    }
#endif
    return true;
}

bool apply_zenith_linux_memory_context(
    const ZenithLinuxMemoryContext& context,
    ZenithProcessMemorySnapshot* snapshot,
    std::string* error) {
    if (snapshot == nullptr || error == nullptr || !snapshot->valid()) {
        if (error != nullptr) {
            *error = "invalid Linux memory context application";
        }
        return false;
    }
    error->clear();
    snapshot->cgroup_v2_detected = context.cgroup_v2_detected;
    snapshot->psi_available = context.psi_available;
    snapshot->psi_some_avg10_milli_percent =
        context.psi_some_avg10_milli_percent;
    snapshot->psi_full_avg10_milli_percent =
        context.psi_full_avg10_milli_percent;

    if (!context.cgroup_v2_limited ||
        context.cgroup_limit_bytes >= snapshot->system_total_bytes) {
        snapshot->cgroup_v2_limited = false;
        snapshot->memory_domain = ZenithProcessMemoryDomain::Host;
        return snapshot->valid();
    }
    if (context.cgroup_limit_bytes == 0U) {
        *error = "cgroup v2 effective memory limit is zero";
        return false;
    }

    const std::uint64_t cgroup_available =
        context.cgroup_current_bytes >= context.cgroup_limit_bytes
            ? 0U
            : context.cgroup_limit_bytes - context.cgroup_current_bytes;
    snapshot->system_total_bytes = context.cgroup_limit_bytes;
    snapshot->system_available_bytes =
        std::min(snapshot->system_available_bytes, cgroup_available);
    snapshot->cgroup_v2_limited = true;
    snapshot->memory_domain = ZenithProcessMemoryDomain::CgroupV2;
    return snapshot->valid();
}

} // namespace zevryon::massivedoc
