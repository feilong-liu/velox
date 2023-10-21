/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/file/FileSystems.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"

using namespace facebook::velox::exec::test;

namespace facebook::velox::exec {

namespace {

class TopNRowNumberTest : public OperatorTestBase {
 protected:
  TopNRowNumberTest() {
    filesystems::registerLocalFileSystem();
  }
};

TEST_F(TopNRowNumberTest, basic) {
  auto data = makeRowVector({
      // Partitioning key.
      makeFlatVector<int64_t>({1, 1, 2, 2, 1, 2, 1}),
      // Sorting key.
      makeFlatVector<int64_t>({77, 66, 55, 44, 33, 22, 11}),
      // Data.
      makeFlatVector<int64_t>({10, 20, 30, 40, 50, 60, 70}),
  });

  createDuckDbTable({data});

  auto testLimit = [&](auto limit) {
    // Emit row numbers.
    auto plan = PlanBuilder()
                    .values({data})
                    .topNRowNumber({"c0"}, {"c1"}, limit, true)
                    .planNode();
    assertQuery(
        plan,
        fmt::format(
            "SELECT * FROM (SELECT *, row_number() over (partition by c0 order by c1) as rn FROM tmp) "
            " WHERE rn <= {}",
            limit));

    // Do not emit row numbers.
    plan = PlanBuilder()
               .values({data})
               .topNRowNumber({"c0"}, {"c1"}, limit, false)
               .planNode();

    assertQuery(
        plan,
        fmt::format(
            "SELECT c0, c1, c2 FROM (SELECT *, row_number() over (partition by c0 order by c1) as rn FROM tmp) "
            " WHERE rn <= {}",
            limit));

    // No partitioning keys.
    plan = PlanBuilder()
               .values({data})
               .topNRowNumber({}, {"c1"}, limit, true)
               .planNode();
    assertQuery(
        plan,
        fmt::format(
            "SELECT * FROM (SELECT *, row_number() over (order by c1) as rn FROM tmp) "
            " WHERE rn <= {}",
            limit));
  };

  testLimit(1);
  testLimit(2);
  testLimit(3);
  testLimit(5);
}

TEST_F(TopNRowNumberTest, largeOutput) {
  // Make 10 vectors. Use different types for partitioning key, sorting key and
  // data. Use order of columns different from partitioning keys, followed by
  // sorting keys, followed by data.
  const vector_size_t size = 10'000;
  auto data = split(
      makeRowVector(
          {"d", "p", "s"},
          {
              // Data.
              makeFlatVector<float>(size, [](auto row) { return row; }),
              // Partitioning key.
              makeFlatVector<int16_t>(size, [](auto row) { return row % 7; }),
              // Sorting key.
              makeFlatVector<int32_t>(
                  size, [](auto row) { return (size - row) * 10; }),
          }),
      10);

  createDuckDbTable(data);

  auto spillDirectory = exec::test::TempDirectoryPath::create();

  auto testLimit = [&](auto limit) {
    SCOPED_TRACE(fmt::format("Limit: {}", limit));
    core::PlanNodeId topNRowNumberId;
    auto plan = PlanBuilder()
                    .values(data)
                    .topNRowNumber({"p"}, {"s"}, limit, true)
                    .capturePlanNodeId(topNRowNumberId)
                    .planNode();

    auto sql = fmt::format(
        "SELECT * FROM (SELECT *, row_number() over (partition by p order by s) as rn FROM tmp) "
        " WHERE rn <= {}",
        limit);
    AssertQueryBuilder(plan, duckDbQueryRunner_)
        .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
        .assertResults(sql);

    // Spilling.
    auto task =
        AssertQueryBuilder(plan, duckDbQueryRunner_)
            .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
            .config(core::QueryConfig::kTestingSpillPct, "100")
            .config(core::QueryConfig::kSpillEnabled, "true")
            .config(core::QueryConfig::kTopNRowNumberSpillEnabled, "true")
            .spillDirectory(spillDirectory->path)
            .assertResults(sql);

    auto taskStats = exec::toPlanStats(task->taskStats());
    const auto& stats = taskStats.at(topNRowNumberId);

    ASSERT_GT(stats.spilledBytes, 0);
    ASSERT_GT(stats.spilledRows, 0);
    ASSERT_GT(stats.spilledFiles, 0);
    ASSERT_GT(stats.spilledPartitions, 0);

    // No partitioning keys.
    plan = PlanBuilder()
               .values(data)
               .topNRowNumber({}, {"s"}, limit, true)
               .planNode();

    AssertQueryBuilder(plan, duckDbQueryRunner_)
        .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
        .assertResults(fmt::format(
            "SELECT * FROM (SELECT *, row_number() over (order by s) as rn FROM tmp) "
            " WHERE rn <= {}",
            limit));
  };

  testLimit(1);
  testLimit(5);
  testLimit(100);
  testLimit(1000);
  testLimit(2000);
}

TEST_F(TopNRowNumberTest, manyPartitions) {
  const vector_size_t size = 10'000;
  auto data = split(
      makeRowVector(
          {"d", "s", "p"},
          {
              // Data.
              makeFlatVector<int64_t>(
                  size, [](auto row) { return row; }, nullEvery(11)),
              // Sorting key.
              makeFlatVector<int64_t>(
                  size,
                  [](auto row) { return (size - row) * 10; },
                  [](auto row) { return row == 123; }),
              // Partitioning key.
              makeFlatVector<int64_t>(
                  size, [](auto row) { return row / 2; }, nullEvery(7)),
          }),
      10);

  createDuckDbTable(data);

  auto spillDirectory = exec::test::TempDirectoryPath::create();

  auto testLimit = [&](auto limit) {
    SCOPED_TRACE(fmt::format("Limit: {}", limit));
    core::PlanNodeId topNRowNumberId;
    auto plan = PlanBuilder()
                    .values(data)
                    .topNRowNumber({"p"}, {"s"}, limit, true)
                    .capturePlanNodeId(topNRowNumberId)
                    .planNode();

    auto sql = fmt::format(
        "SELECT * FROM (SELECT *, row_number() over (partition by p order by s) as rn FROM tmp) "
        " WHERE rn <= {}",
        limit);
    assertQuery(plan, sql);

    // Spilling.
    auto task =
        AssertQueryBuilder(plan, duckDbQueryRunner_)
            .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
            .config(core::QueryConfig::kTestingSpillPct, "100")
            .config(core::QueryConfig::kSpillEnabled, "true")
            .config(core::QueryConfig::kTopNRowNumberSpillEnabled, "true")
            .spillDirectory(spillDirectory->path)
            .assertResults(sql);

    auto taskStats = exec::toPlanStats(task->taskStats());
    const auto& stats = taskStats.at(topNRowNumberId);

    ASSERT_GT(stats.spilledBytes, 0);
    ASSERT_GT(stats.spilledRows, 0);
    ASSERT_GT(stats.spilledFiles, 0);
    ASSERT_GT(stats.spilledPartitions, 0);
  };

  testLimit(1);
  testLimit(2);
  testLimit(100);
}

TEST_F(TopNRowNumberTest, planNodeValidation) {
  auto data = makeRowVector(
      ROW({"a", "b", "c", "d", "e"},
          {
              BIGINT(),
              BIGINT(),
              BIGINT(),
              BIGINT(),
              BIGINT(),
          }),
      10);

  auto plan = [&](const std::vector<std::string>& partitionKeys,
                  const std::vector<std::string>& sortingKeys,
                  int32_t limit = 10) {
    PlanBuilder()
        .values({data})
        .topNRowNumber(partitionKeys, sortingKeys, limit, true)
        .planNode();
  };

  VELOX_ASSERT_THROW(
      plan({"a", "a"}, {"b"}),
      "Partitioning keys must be unique. Found duplicate key: a");

  VELOX_ASSERT_THROW(
      plan({"a", "b"}, {"c", "d", "c"}),
      "Sorting keys must be unique and not overlap with partitioning keys. Found duplicate key: c");

  VELOX_ASSERT_THROW(
      plan({"a", "b"}, {"c", "b"}),
      "Sorting keys must be unique and not overlap with partitioning keys. Found duplicate key: b");

  VELOX_ASSERT_THROW(
      plan({"a", "b"}, {}), "Number of sorting keys must be greater than zero");

  VELOX_ASSERT_THROW(
      plan({"a", "b"}, {"c"}, -5), "Limit must be greater than zero");

  VELOX_ASSERT_THROW(
      plan({"a", "b"}, {"c"}, 0), "Limit must be greater than zero");
}

} // namespace
} // namespace facebook::velox::exec
