#include "font_discovery_generation.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace zevryon::text;

bool build_generation(
    std::uint64_t generation_id,
    std::uint16_t weight,
    std::shared_ptr<const FontCatalogGeneration>* output) {
    const std::array<FontCoverageRange, 1> coverage{{{0x0020U, 0x007eU}}};
    const std::array<FontDiscoveryFace, 1> faces{{{
        "adapter/concurrency-font#0",
        "Concurrency Sans",
        weight,
        5U,
        FontSlant::Upright,
        ScriptId::Latn,
        0U,
        coverage,
    }}};
    FontDiscoveryStats stats;
    FontDiscoveryError error;
    return build_font_catalog_generation(
        generation_id,
        faces,
        64U * 1024U,
        64U * 1024U,
        output,
        &stats,
        &error);
}

} // namespace

int main() {
    std::shared_ptr<const FontCatalogGeneration> first;
    std::shared_ptr<const FontCatalogGeneration> second;
    if (!build_generation(1U, 400U, &first) ||
        !build_generation(3U, 700U, &second)) {
        std::cerr << "failed to build concurrency fixtures\n";
        return 1;
    }

    FontCatalogGenerationStore store;
    if (store.publish(first) != FontGenerationPublishResult::Published) {
        std::cerr << "failed to publish initial generation\n";
        return 1;
    }

    constexpr std::size_t kReaderCount = 8U;
    constexpr std::size_t kReadsPerThread = 50000U;
    std::atomic<bool> begin{false};
    std::atomic<bool> failed{false};
    std::atomic<std::uint64_t> first_reads{0U};
    std::atomic<std::uint64_t> second_reads{0U};
    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);

    for (std::size_t reader = 0U; reader < kReaderCount; ++reader) {
        readers.emplace_back([&] {
            while (!begin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t iteration = 0U;
                 iteration < kReadsPerThread;
                 ++iteration) {
                const auto snapshot = store.snapshot();
                if (snapshot == nullptr || snapshot->catalog().faces.size() != 1U ||
                    snapshot->identity(0U) != "adapter/concurrency-font#0" ||
                    snapshot->family_name(0U) != "Concurrency Sans") {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                if (snapshot->generation_id() == 1U &&
                    snapshot->catalog().faces[0].weight == 400U) {
                    first_reads.fetch_add(1U, std::memory_order_relaxed);
                } else if (snapshot->generation_id() == 3U &&
                           snapshot->catalog().faces[0].weight == 700U) {
                    second_reads.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }

    begin.store(true, std::memory_order_release);
    for (std::size_t iteration = 0U; iteration < 1000U; ++iteration) {
        if (iteration == 250U) {
            const FontGenerationPublishResult result = store.publish(second);
            if (result != FontGenerationPublishResult::Published) {
                std::cerr << "failed to publish replacement generation\n";
                failed.store(true, std::memory_order_release);
                break;
            }
        }
        std::this_thread::yield();
    }

    for (std::thread& reader : readers) {
        reader.join();
    }

    if (failed.load(std::memory_order_acquire) || first_reads.load() == 0U ||
        second_reads.load() == 0U || store.snapshot()->generation_id() != 3U) {
        std::cerr << "atomic generation reader stress failed\n";
        return 1;
    }

    std::cout << "font discovery generation concurrency tests passed\n";
    return 0;
}
