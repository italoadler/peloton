//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// insert_performance_test.cpp
//
// Identification: test/performance/insert_performance_test.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//


#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <atomic>

#include "executor/testing_executor_util.h"
#include "common/harness.h"

#include "catalog/schema.h"
#include "type/value_factory.h"
#include "common/timer.h"
#include "concurrency/transaction_manager_factory.h"

#include "executor/executor_context.h"
#include "executor/insert_executor.h"
#include "executor/logical_tile_factory.h"
#include "expression/expression_util.h"
#include "expression/tuple_value_expression.h"
#include "expression/comparison_expression.h"
#include "expression/abstract_expression.h"
#include "storage/tile.h"
#include "storage/tile_group.h"
#include "storage/table_factory.h"

#include "executor/mock_executor.h"

#include "planner/insert_plan.h"

using ::testing::NotNull;
using ::testing::Return;

namespace peloton {
namespace test {

//===--------------------------------------------------------------------===//
// Insert Tests
//===--------------------------------------------------------------------===//

class InsertPerformanceTests : public PelotonTest {};

std::atomic<int> loader_tuple_id;

//===------------------------------===//
// Utility
//===------------------------------===//

void InsertTuple(storage::DataTable *table, type::AbstractPool *pool,
                 oid_t tilegroup_count_per_loader,
                 UNUSED_ATTRIBUTE uint64_t thread_itr) {
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();

  oid_t tuple_count = tilegroup_count_per_loader * TEST_TUPLES_PER_TILEGROUP;

  // Start a txn for each insert
  auto txn = txn_manager.BeginTransaction();
  std::unique_ptr<storage::Tuple> tuple(
      TestingExecutorUtil::GetTuple(table, ++loader_tuple_id, pool));

  std::unique_ptr<executor::ExecutorContext> context(
      new executor::ExecutorContext(txn));

  planner::InsertPlan node(table, std::move(tuple));

  // Insert the desired # of tuples
  for (oid_t tuple_itr = 0; tuple_itr < tuple_count; tuple_itr++) {
    executor::InsertExecutor executor(&node, context.get());
    executor.Execute();
  }

  txn_manager.CommitTransaction(txn);
}

/**
 * This will test a single thread doing direct
 * inserts into the DataTable (i.e., it is not going
 * through the execution layer).
 */
TEST_F(InsertPerformanceTests, RawInsertTest) {
  const int tuple_count = 5000000;
  const int batch_size = 100000;
  const bool build_indexes = false;

  std::unique_ptr<storage::DataTable> table(
      TestingExecutorUtil::CreateTable(TEST_TUPLES_PER_TILEGROUP, build_indexes));
  auto pool = TestingHarness::GetInstance().GetTestingPool();
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  ItemPointer *index_entry_ptr = nullptr;

  LOG_INFO("Speed Test [numTuples=%d / batchSize=%d]",
           tuple_count, batch_size);

  // We'll use a single txn for all of the inserts so that
  // our measurements are mostly based on how fast we can insert.
  // There will be some small overhead of updating the txn's write set
  // but that is unavoidable.
  auto txn = txn_manager.BeginTransaction();
  Timer<> timer;
  timer.Start();
  for (int tuple_id = 0; tuple_id < tuple_count; tuple_id++) {
    std::unique_ptr<storage::Tuple> tuple(
        TestingExecutorUtil::GetTuple(table.get(), tuple_id, pool));
    ItemPointer tuple_slot_id =
        table->InsertTuple(tuple.get(), txn, &index_entry_ptr);
    PL_ASSERT(tuple_slot_id.block != INVALID_OID);
    PL_ASSERT(tuple_slot_id.offset != INVALID_OID);

    if (tuple_id != 0 && tuple_id % batch_size == 0) {
      timer.Stop();
      LOG_INFO("Batch #%02d -- Duration: %.2lf / Total # of Tuples: %d",
               (tuple_id / batch_size), timer.GetDuration(),
               (int)table->GetTupleCount()
      );
      timer.Reset();
      timer.Start();
    }

  } // FOR
  LOG_INFO("Total Duration: %.2lf", timer.GetTotalDuration());
  txn_manager.CommitTransaction(txn);

  // Sanity Check
  EXPECT_EQ(tuple_count, table->GetTupleCount());
}

//TEST_F(InsertPerformanceTests, LoadingTest) {
//  // We are going to simply load tile groups concurrently in this test
//  // WARNING: This test may potentially run for a long time if
//  // TEST_TUPLES_PER_TILEGROUP is large, consider rewrite the test or hard
//  // code the number of tuples per tile group in this test
//  oid_t tuples_per_tilegroup = TEST_TUPLES_PER_TILEGROUP;
//  bool build_indexes = false;
//
//  // Control the scale
//  oid_t loader_threads_count = 1;
//  oid_t tilegroup_count_per_loader = 1000;
//
//  // Each tuple size ~40 B.
//  UNUSED_ATTRIBUTE oid_t tuple_size = 41;
//
//  std::unique_ptr<storage::DataTable> data_table(
//      TestingExecutorUtil::CreateTable(tuples_per_tilegroup, build_indexes));
//
//  auto testing_pool = TestingHarness::GetInstance().GetTestingPool();
//
//  Timer<> timer;
//
//  timer.Start();
//
//  LaunchParallelTest(loader_threads_count, InsertTuple, data_table.get(),
//                     testing_pool, tilegroup_count_per_loader);
//
//  timer.Stop();
//  UNUSED_ATTRIBUTE auto duration = timer.GetDuration();
//
//  LOG_INFO("Duration: %.2lf", duration);
//
//  auto expected_tile_group_count = 0;
//
//  int total_tuple_count = loader_threads_count * tilegroup_count_per_loader * TEST_TUPLES_PER_TILEGROUP;
//  int max_cached_tuple_count = TEST_TUPLES_PER_TILEGROUP * storage::DataTable::default_active_tilegroup_count_;
//  int max_unfill_cached_tuple_count = (TEST_TUPLES_PER_TILEGROUP - 1) * storage::DataTable::default_active_tilegroup_count_;
//
//  if (total_tuple_count - max_cached_tuple_count <= 0) {
//    if (total_tuple_count <= max_unfill_cached_tuple_count) {
//      expected_tile_group_count = storage::DataTable::default_active_tilegroup_count_;
//    } else {
//      expected_tile_group_count = storage::DataTable::default_active_tilegroup_count_ + total_tuple_count - max_unfill_cached_tuple_count;
//    }
//  } else {
//    int filled_tile_group_count = total_tuple_count / max_cached_tuple_count * storage::DataTable::default_active_tilegroup_count_;
//
//    if (total_tuple_count - filled_tile_group_count * TEST_TUPLES_PER_TILEGROUP - max_unfill_cached_tuple_count <= 0) {
//      expected_tile_group_count = filled_tile_group_count + storage::DataTable::default_active_tilegroup_count_;
//    } else {
//      expected_tile_group_count = filled_tile_group_count + storage::DataTable::default_active_tilegroup_count_ + (total_tuple_count - filled_tile_group_count - max_unfill_cached_tuple_count);
//    }
//  }
//
//  UNUSED_ATTRIBUTE auto bytes_to_megabytes_converter = (1024 * 1024);
//
//  EXPECT_EQ(data_table->GetTileGroupCount(), expected_tile_group_count);
//
//  LOG_INFO("Dataset size : %u MB \n",
//           (expected_tile_group_count * tuples_per_tilegroup * tuple_size) /
//               bytes_to_megabytes_converter);
//}

}  // namespace test
}  // namespace peloton
