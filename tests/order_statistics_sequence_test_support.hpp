#pragma once

#define run_order_statistics_sequence_tests run_order_statistics_sequence_core_tests
#include "order_statistics_sequence_test_support_impl.hpp"
#undef run_order_statistics_sequence_tests

#include "compact_durable_order_test_support.hpp"
#include "durable_consumer_reopen_test_support.hpp"
#include "logical_order_persistence_test_support.hpp"
#include "logical_order_publication_test_support.hpp"
#include "order_statistics_sequence_fork_test_support.hpp"

namespace zevryon_test {

inline bool run_order_statistics_sequence_tests() {
    return run_logical_order_persistence_tests() &&
           run_logical_order_publication_tests() &&
           run_order_statistics_sequence_fork_tests() &&
           run_compact_durable_order_tests() &&
           run_durable_consumer_reopen_tests() &&
           run_order_statistics_sequence_core_tests();
}

} // namespace zevryon_test
