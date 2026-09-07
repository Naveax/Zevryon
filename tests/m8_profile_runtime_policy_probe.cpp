#include "massivedoc_address_space.hpp"
#include "massivedoc_profile_policy.hpp"

#include <cstddef>
#include <iomanip>
#include <iostream>

int main() {
    using namespace zevryon::massivedoc;
    std::cout << std::setprecision(15);
    std::cout << "{\"schema\":\"zevryon.m8.profile-runtime-policy.v1\","
              << "\"pointer_bits\":" << current_pointer_bits() << ","
              << "\"profiles\":[";
    bool first = true;
    for (const auto& limits : kMassiveDocProfileLimits) {
        if (!first) {
            std::cout << ',';
        }
        first = false;
        const auto runtime = make_massive_doc_profile_runtime_config(limits.profile);
        const auto& budget = runtime.budgets;
        std::cout
            << "{\"name\":\"" << limits.name << "\","
            << "\"minimum_physical_ram_mib\":" << limits.minimum_physical_ram_mib << ','
            << "\"process_group_pss_target_mb\":" << limits.process_group_pss_target_mb << ','
            << "\"process_group_pss_hard_cap_mb\":" << limits.process_group_pss_hard_cap_mb << ','
            << "\"hot_cache_mb\":" << limits.hot_cache_mb << ','
            << "\"warm_cache_mb\":" << limits.warm_cache_mb << ','
            << "\"cold_cache_mb\":" << limits.cold_cache_mb << ','
            << "\"first_viewport_preindexed_ms\":" << limits.first_viewport_preindexed_ms << ','
            << "\"first_viewport_streaming_ms\":" << limits.first_viewport_streaming_ms << ','
            << "\"scroll_p99_ms\":" << limits.scroll_p99_ms << ','
            << "\"maximum_normal_stall_ms\":" << limits.maximum_normal_stall_ms << ','
            << "\"exact_search_warm_ms\":" << limits.exact_search_warm_ms << ','
            << "\"exact_search_cold_ms\":" << limits.exact_search_cold_ms << ','
            << "\"mutation_p95_us\":" << limits.mutation_p95_us << ','
            << "\"copy_throughput_mib_s\":" << limits.copy_throughput_mib_s << ','
            << "\"runtime\":{"
            << "\"block_bytes\":" << runtime.store_read.block_cache.block_bytes << ','
            << "\"block_hot_bytes\":" << runtime.store_read.block_cache.hot_bytes << ','
            << "\"block_warm_bytes\":" << runtime.store_read.block_cache.warm_bytes << ','
            << "\"cold_window_bytes\":" << runtime.store_read.cold_window_bytes << ','
            << "\"layout_cache_bytes\":" << runtime.layout.max_cache_bytes << ','
            << "\"checkpoint_cache_bytes\":" << runtime.layout.max_checkpoint_cache_bytes << ','
            << "\"source_window_cache_bytes\":" << runtime.layout.max_source_window_cache_bytes << "},"
            << "\"budgets\":{"
            << "\"hot_budget_bytes\":" << budget.hot_budget_bytes << ','
            << "\"hot_block_cache_bytes\":" << budget.hot_block_cache_bytes << ','
            << "\"hot_layout_cache_bytes\":" << budget.hot_layout_cache_bytes << ','
            << "\"hot_unallocated_bytes\":" << budget.hot_unallocated_bytes << ','
            << "\"warm_budget_bytes\":" << budget.warm_budget_bytes << ','
            << "\"warm_block_cache_bytes\":" << budget.warm_block_cache_bytes << ','
            << "\"warm_checkpoint_cache_bytes\":" << budget.warm_checkpoint_cache_bytes << ','
            << "\"warm_source_window_cache_bytes\":" << budget.warm_source_window_cache_bytes << ','
            << "\"warm_unallocated_bytes\":" << budget.warm_unallocated_bytes << ','
            << "\"cold_budget_bytes\":" << budget.cold_budget_bytes << ','
            << "\"cold_mapped_window_bytes\":" << budget.cold_mapped_window_bytes << ','
            << "\"cold_unallocated_bytes\":" << budget.cold_unallocated_bytes << ','
            << "\"within_profile_budgets\":"
            << (budget.within_profile_budgets ? "true" : "false") << "}}";
    }
    std::cout << "]}\n";
    return 0;
}
