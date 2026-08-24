#include "zenith_process_runtime_services.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

void test_large_dormant_registry_without_runtime_materialization() {
    ZenithProcessRuntimeServices services;
    require(services.valid(), "process runtime services are invalid");

    const std::filesystem::path deliberately_missing_root =
        std::filesystem::temp_directory_path() /
        "zevryon-dormant-registry-must-not-open-this-store";
    std::error_code ignored;
    std::filesystem::remove_all(deliberately_missing_root, ignored);

    constexpr std::uint64_t kRegressionSampleTabs = 4096U;
    std::string error;
    for (std::uint64_t id = 1U; id <= kRegressionSampleTabs; ++id) {
        require(
            services.open_tab(
                id,
                deliberately_missing_root,
                DeviceFrameProfile::Desktop,
                LayoutConfig{},
                FrameVisibility::Hidden,
                12345,
                &error),
            "dormant registry rejected hidden regression-sample tab");
    }

    const ZenithProcessRuntimeServicesStatus opened = services.status();
    require(opened.tabs == kRegressionSampleTabs,
            "dormant registry tab count mismatch");
    require(opened.materialized_tabs == 0U,
            "dormant registry eagerly materialized runtime state");
    require(opened.prefetch_pool.sessions == 0U &&
                opened.prefetch_pool.active_sessions == 0U &&
                opened.prefetch_pool.live_threads == 0U,
            "dormant registry consumed source-prefetch runtime resources");
    require(opened.prefetch_pool.ready_bytes == 0U &&
                opened.prefetch_pool.ready_results == 0U,
            "dormant registry retained speculative payload");
    require(opened.foreground_layout_pool.sessions == 0U &&
                opened.foreground_layout_pool.active_sessions == 0U &&
                opened.foreground_layout_pool.live_threads == 0U &&
                opened.foreground_layout_pool.handoff.ready_results == 0U &&
                opened.foreground_layout_pool.handoff.ready_bytes == 0U,
            "dormant registry consumed foreground-layout runtime resources");
    require(opened.tab_controller.registered_tabs == 0U &&
                opened.tab_controller.visible_tabs == 0U &&
                opened.tab_controller.hidden_tabs == 0U,
            "dormant registry polluted materialized controller working set");

    error.clear();
    require(
        !services.set_tab_activity(
            1U,
            FrameVisibility::Visible,
            4096,
            &error),
        "missing-store dormant slot unexpectedly materialized");
    require(!error.empty(),
            "failed dormant materialization lost diagnostic");

    const ZenithProcessRuntimeServicesStatus failed_once = services.status();
    require(failed_once.materialized_tabs == 0U &&
                failed_once.prefetch_pool.sessions == 0U &&
                failed_once.foreground_layout_pool.sessions == 0U,
            "failed materialization leaked runtime or shared-pool session");
    require(failed_once.tab_controller.registered_tabs == 0U,
            "failed materialization leaked controller working-set entry");

    error.clear();
    require(
        !services.set_tab_activity(
            1U,
            FrameVisibility::Visible,
            4096,
            &error),
        "second materialization attempt was incorrectly treated as already applied");
    require(!error.empty(),
            "second failed materialization lost diagnostic");

    for (std::uint64_t id = 1U; id <= kRegressionSampleTabs; ++id) {
        require(services.close_tab(id),
                "dormant registry failed to close regression-sample tab");
    }

    const ZenithProcessRuntimeServicesStatus closed = services.status();
    require(closed.tabs == 0U && closed.materialized_tabs == 0U &&
                closed.prefetch_pool.sessions == 0U &&
                closed.foreground_layout_pool.sessions == 0U &&
                closed.tab_controller.registered_tabs == 0U,
            "dormant registry retained state after close");
}

} // namespace

int main() {
    test_large_dormant_registry_without_runtime_materialization();
    std::cout << "Zevryon dormant registry scale tests passed\n";
    return 0;
}
