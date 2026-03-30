// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <vector>

#include "column/column_helper.h"
#include "column/type_traits.h"
#include "exec/hdfs_scanner.h"
#include "exprs/binary_predicate.h"
#include "exprs/column_ref.h"
#include "exprs/min_max_predicate.h"
#include "exprs/runtime_filter.h"
#include "formats/parquet/file_reader.h"
#include "formats/parquet/group_reader.h"
#include "formats/parquet/parquet_test_util/util.h"
#include "formats/parquet/parquet_ut_base.h"
#include "fs/fs.h"
#include "io/shared_buffered_input_stream.h"
#include "testutil/assert.h"
#include "testutil/column_test_helper.h"
#include "testutil/exprs_test_helper.h"

namespace starrocks::parquet {

static HdfsScanStats g_hdfs_scan_stats;
using starrocks::HdfsScannerContext;

class PageIndexTest : public testing::Test {
public:
    void SetUp() override { _runtime_state = _pool.add(new RuntimeState(TQueryGlobals())); }

    void TearDown() override {}

protected:
    RuntimeFilterProbeDescriptor* gen_runtime_filter_desc(SlotId slot_id);
    StatusOr<HdfsScannerContext*> create_context_for_rf_decimal128(SlotId slot_id, int128_t start, int128_t end, bool has_null);
    std::unique_ptr<RandomAccessFile> _create_file(const std::string& file_path);

    HdfsScannerContext* _create_scan_context();

    THdfsScanRange* _create_scan_range(const std::string& file_path, size_t scan_length = 0);

    HdfsScannerContext* _create_file_random_read_context(const std::string& file_path);
    HdfsScannerContext* _create_file_only_c0_context(const std::string& file_path);
    HdfsScannerContext* _create_file_c0_c1_c2_context(const std::string& file_path);

    RuntimeState* _runtime_state = nullptr;
    ObjectPool _pool;
};

std::unique_ptr<RandomAccessFile> PageIndexTest::_create_file(const std::string& file_path) {
    return *FileSystem::Default()->new_random_access_file(file_path);
}

HdfsScannerContext* PageIndexTest::_create_scan_context() {
    auto* ctx = _pool.add(new HdfsScannerContext());
    auto* lazy_column_coalesce_counter = _pool.add(new std::atomic<int32_t>(0));
    ctx->lazy_column_coalesce_counter = lazy_column_coalesce_counter;
    ctx->timezone = "Asia/Shanghai";
    ctx->stats = &g_hdfs_scan_stats;
    return ctx;
}

HdfsScannerContext* PageIndexTest::_create_file_random_read_context(const std::string& file_path) {
    auto ctx = _create_scan_context();

    TypeDescriptor type_array(LogicalType::TYPE_ARRAY);
    type_array.children.emplace_back(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT));

    // tuple desc
    Utils::SlotDesc slot_descs[] = {
            {"c0", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT)},
            {"c1", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT)},
            {"c2", TypeDescriptor::from_logical_type(LogicalType::TYPE_VARCHAR)},
            {"c3", type_array},
            {""},
    };
    ctx->tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, slot_descs);
    Utils::make_column_info_vector(ctx->tuple_desc, &ctx->materialized_columns);
    ctx->scan_range = (_create_scan_range(file_path));

    return ctx;
}

HdfsScannerContext* PageIndexTest::_create_file_only_c0_context(const std::string& file_path) {
    auto ctx = _create_scan_context();

    // tuple desc
    Utils::SlotDesc slot_descs[] = {
            {"c0", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT)},
            {""},
    };
    ctx->tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, slot_descs);
    Utils::make_column_info_vector(ctx->tuple_desc, &ctx->materialized_columns);
    ctx->scan_range = (_create_scan_range(file_path));

    return ctx;
}

HdfsScannerContext* PageIndexTest::_create_file_c0_c1_c2_context(const std::string& file_path) {
    auto ctx = _create_scan_context();

    // tuple desc
    Utils::SlotDesc slot_descs[] = {
            {"c0", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT)},
            {"c1", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT)},
            {"c2", TypeDescriptor::from_logical_type(LogicalType::TYPE_VARCHAR)},
            {""},
    };
    ctx->tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, slot_descs);
    Utils::make_column_info_vector(ctx->tuple_desc, &ctx->materialized_columns);
    ctx->scan_range = (_create_scan_range(file_path));

    return ctx;
}

THdfsScanRange* PageIndexTest::_create_scan_range(const std::string& file_path, size_t scan_length) {
    auto* scan_range = _pool.add(new THdfsScanRange());

    scan_range->relative_path = file_path;
    scan_range->file_length = std::filesystem::file_size(file_path);
    scan_range->offset = 4;
    scan_range->length = scan_length > 0 ? scan_length : scan_range->file_length;

    return scan_range;
}

TEST_F(PageIndexTest, TestRandomReadWith2PageSize) {
    std::random_device rd;
    std::mt19937 rng(rd());

    TypeDescriptor type_array(LogicalType::TYPE_ARRAY);
    type_array.children.emplace_back(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT));

    auto chunk = std::make_shared<Chunk>();
    chunk->append_column(ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), true),
                         chunk->num_columns());
    chunk->append_column(ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), true),
                         chunk->num_columns());
    chunk->append_column(
            ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_VARCHAR), true),
            chunk->num_columns());
    chunk->append_column(ColumnHelper::create_column(type_array, true), chunk->num_columns());

    // c0 = np.arange(1, 20001)
    // c1 = np.arange(20000, 0, -1)
    // data = {
    //     'c0': c0,
    //     'c1': c1
    // }
    // df = pd.DataFrame(data)
    // df_with_dict = pd.DataFrame({
    //     "c0": df["c0"],
    //     "c1": df["c1"],
    //     "c2": df.apply(lambda x: pd.NA if x["c0"] % 10 == 0 else str(x["c0"] % 100), axis = 1),
    //     "c3": df.apply(lambda x: pd.NA if x["c0"] % 10 == 0 else [x["c0"] % 1000, pd.NA, x["c1"] % 1000], axis = 1)
    // })
    const std::string small_page_file = "./be/test/formats/parquet/test_data/page_index_small_page.parquet";

    // c0 = np.arange(1, 100001)
    // c1 = np.arange(100000, 0, -1)
    // data = {
    //     'c0': c0,
    //     'c1': c1
    // }
    // df = pd.DataFrame(data)
    // df_with_dict = pd.DataFrame({
    //     "c0": df["c0"],
    //     "c1": df["c1"],
    //     "c2": df.apply(lambda x: pd.NA if x["c0"] % 10 == 0 else str(x["c0"] % 100), axis = 1),
    //     "c3": df.apply(lambda x: pd.NA if x["c0"] % 10 == 0 else [x["c0"] % 1000, pd.NA, x["c1"] % 1000], axis = 1)
    // })
    const std::string big_page_file = "./be/test/formats/parquet/test_data/page_index_big_page.parquet";
    // same data with above but without dictionary
    const std::string repeated_no_dict_file = "./be/test//formats/parquet/test_data/page_index_repeated_nodict.parquet";

    std::vector<std::string> files = {small_page_file, big_page_file, repeated_no_dict_file};

    // for small page 1000 values / page
    // for big page 10000 values / page
    for (size_t index = 0; index < 3; index++) {
        const std::string& file_path = files[index];
        std::cout << "file_path: " << file_path << std::endl;

        std::uniform_int_distribution<int> dist_small(1, 20000);
        std::uniform_int_distribution<int> dist_big(1, 100000);

        std::vector<int> oprands;
        size_t expected_row = 0;
        auto _print_predicate = [&](bool single) {
            std::stringstream ss;
            ss << "expected_row: " << expected_row << " predicate: c0 > " << oprands[0] << " and c0 < " << oprands[1];
            if (single) {
                return ss.str();
            }
            ss << " and c1 > " << oprands[2] << " and c1 < " << oprands[3];
            return ss.str();
        };

        std::vector<bool> single_or_not{true, false};

        for (bool single_flag : single_or_not) {
            // use 2 to save ci's time, change bigger to test more case
            for (int32_t i = 0; i < 2; i++) {
                oprands.clear();
                for (int32_t j = 0; j < 4; j++) {
                    int num = index == 0 ? dist_small(rng) : dist_big(rng);
                    oprands.emplace_back(num);
                }
                for (int k : std::vector<int>{0, 2}) {
                    if (oprands[k] > oprands[k + 1]) {
                        int temp = oprands[k];
                        oprands[k] = oprands[k + 1];
                        oprands[k + 1] = temp;
                    }
                }

                auto ctx = _create_file_random_read_context(file_path);
                auto file = _create_file(file_path);
                ctx->conjunct_ctxs_by_slot[0].clear();
                ctx->min_max_conjunct_ctxs.clear();

                if (single_flag) {
                    Utils::SlotDesc min_max_slots[] = {
                            {"c0", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), 0},
                            {""},
                    };
                    ctx->min_max_tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, min_max_slots);

                    std::vector<TExpr> t_conjuncts;
                    ParquetUTBase::append_int_conjunct(TExprOpcode::GT, 0, oprands[0], &t_conjuncts);
                    ParquetUTBase::append_int_conjunct(TExprOpcode::LT, 0, oprands[1], &t_conjuncts);

                    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts,
                                                        &ctx->min_max_conjunct_ctxs);
                    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts,
                                                        &ctx->conjunct_ctxs_by_slot[0]);

                    expected_row = std::max(oprands[1] - oprands[0] - 1, 0);
                } else {
                    ctx->conjunct_ctxs_by_slot[1].clear();
                    Utils::SlotDesc min_max_slots[] = {
                            {"c0", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), 0},
                            {"c1", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), 1},
                            {""},
                    };
                    ctx->min_max_tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, min_max_slots);

                    std::vector<TExpr> t_conjuncts;
                    ParquetUTBase::append_int_conjunct(TExprOpcode::GT, 0, oprands[0], &t_conjuncts);
                    ParquetUTBase::append_int_conjunct(TExprOpcode::LT, 0, oprands[1], &t_conjuncts);
                    ParquetUTBase::append_int_conjunct(TExprOpcode::GT, 1, oprands[2], &t_conjuncts);
                    ParquetUTBase::append_int_conjunct(TExprOpcode::LT, 1, oprands[3], &t_conjuncts);

                    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts,
                                                        &ctx->min_max_conjunct_ctxs);

                    std::vector<TExpr> t_conjuncts_slot0{t_conjuncts[0], t_conjuncts[1]};
                    std::vector<TExpr> t_conjuncts_slot1{t_conjuncts[2], t_conjuncts[3]};

                    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts_slot0,
                                                        &ctx->conjunct_ctxs_by_slot[0]);
                    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts_slot1,
                                                        &ctx->conjunct_ctxs_by_slot[1]);

                    int low_bound = std::max(oprands[0], index == 0 ? 20001 - oprands[3] : 100001 - oprands[3]);
                    int up_bound = std::min(oprands[1], index == 0 ? 20001 - oprands[2] : 100001 - oprands[2]);
                    expected_row = std::max(up_bound - low_bound - 1, 0);
                }

                std::cout << "file path: " << file_path << ", " << _print_predicate(single_flag) << std::endl;

                auto file_reader = std::make_shared<FileReader>(config::vector_chunk_size, file.get(),
                                                                std::filesystem::file_size(file_path));

                Status status = file_reader->init(ctx);
                ASSERT_TRUE(status.ok());
                size_t total_row_nums = 0;
                while (!status.is_end_of_file()) {
                    chunk->reset();
                    status = file_reader->get_next(&chunk);
                    chunk->check_or_die();
                    total_row_nums += chunk->num_rows();
                    if (!status.ok() && !status.is_end_of_file()) {
                        std::cout << status.message() << std::endl;
                        DCHECK(false) << "file path: " << file_path << ", " << _print_predicate(single_flag);
                    }
                    // check row value
                    if (chunk->num_rows() > 0) {
                        ColumnPtr c0 = chunk->get_column_by_index(0);
                        ColumnPtr c1 = chunk->get_column_by_index(1);
                        ColumnPtr c2 = chunk->get_column_by_index(2);
                        ColumnPtr c3 = chunk->get_column_by_index(3);
                        for (size_t row_index = 0; row_index < chunk->num_rows(); row_index++) {
                            int32_t c0_value = c0->get(row_index).get_int32();
                            int32_t c1_value = c1->get(row_index).get_int32();
                            bool flag = index == 0 ? c0_value + c1_value == 20001 : c0_value + c1_value == 100001;
                            if (c0_value % 10 == 0) {
                                flag &= c2->is_null(row_index);
                                flag &= c3->is_null(row_index);
                            } else {
                                flag &= (!c2->is_null(row_index));
                                flag &= (!c3->is_null(row_index));
                                if (!flag) {
                                    std::cout << "file path: " << file_path << ", " << _print_predicate(single_flag);
                                }
                                EXPECT_TRUE(flag);
                                std::string expected_string = std::to_string(c0_value % 100);
                                Slice expected_value = Slice(expected_string);
                                Slice c2_value = c2->get(row_index).get_slice();
                                flag &= (c2_value == expected_value);
                                DatumArray c3_value = c3->get(row_index).get_array();
                                flag &= (c3_value.size() == 3) && (!c3_value[0].is_null()) &&
                                        (c3_value[0].get_int32() == (c0_value % 1000)) && (c3_value[1].is_null()) &&
                                        (!c3_value[2].is_null()) && (c3_value[2].get_int32() == (c1_value % 1000));
                            }
                            if (!flag) {
                                std::cout << "file path: " << file_path << ", " << _print_predicate(single_flag);
                            }
                            EXPECT_TRUE(flag);
                        }
                    }
                }
                EXPECT_EQ(total_row_nums, expected_row);
            }
        }
    }
}

TEST_F(PageIndexTest, TestCollectIORangeWithPageIndex) {
    auto chunk = std::make_shared<Chunk>();
    chunk->append_column(ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), true),
                         chunk->num_columns());

    const std::string small_page_file = "./be/test/formats/parquet/test_data/page_index_small_page.parquet";

    Utils::SlotDesc min_max_slots[] = {
            {"c0", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), 0},
            {""},
    };

    auto ctx = _create_file_only_c0_context(small_page_file);
    auto file = _create_file(small_page_file);
    ctx->conjunct_ctxs_by_slot[0].clear();
    ctx->min_max_conjunct_ctxs.clear();
    ctx->min_max_tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, min_max_slots);

    std::vector<TExpr> t_conjuncts;
    ParquetUTBase::append_int_conjunct(TExprOpcode::GT, 0, 5500, &t_conjuncts);
    ParquetUTBase::append_int_conjunct(TExprOpcode::LT, 0, 7500, &t_conjuncts);

    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts, &ctx->min_max_conjunct_ctxs);
    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts, &ctx->conjunct_ctxs_by_slot[0]);

    auto file_reader = std::make_shared<FileReader>(config::vector_chunk_size, file.get(),
                                                    std::filesystem::file_size(small_page_file));

    Status status = file_reader->init(ctx);
    ASSERT_TRUE(status.ok());

    // two row groups, but one is filtered.
    EXPECT_EQ(file_reader->_row_group_readers.size(), 1);
    std::vector<io::SharedBufferedInputStream::IORange> ranges;
    int64_t end_offset = 0;

    file_reader->_row_group_readers[0]->collect_io_ranges(&ranges, &end_offset, ColumnIOType::PAGE_INDEX);
    // collect io of column index and offset index for active column.
    EXPECT_EQ(ranges.size(), 2);
    // offset_index_offset = 293436, offset_index_length = 113, column_index_offset = 291196, column_index_length = 211
    EXPECT_EQ(ranges[1].offset, 293436);
    EXPECT_EQ(ranges[1].size, 113);

    ranges.clear();
    end_offset = 0;

    file_reader->_row_group_readers[0]->collect_io_ranges(&ranges, &end_offset);
    // 3 pages, 1 range 5000-8000
    EXPECT_EQ(file_reader->_row_group_readers[0]->_range.size(), 1);
    // only collect io of 3 pages, 5001-6000, 6001-7000, 7001-8000 and a dict page.
    EXPECT_EQ(ranges.size(), 4);
    // page 7001-8000: offset 50814, size 1660
    EXPECT_EQ(end_offset, 52474);

    size_t total_row_nums = 0;
    while (!status.is_end_of_file()) {
        chunk->reset();
        status = file_reader->get_next(&chunk);
        chunk->check_or_die();
        total_row_nums += chunk->num_rows();
    }
    EXPECT_EQ(total_row_nums, 1999);
}

TEST_F(PageIndexTest, TestTwoColumnIntersectPageIndex) {
    auto chunk = std::make_shared<Chunk>();
    chunk->append_column(ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), true),
                         chunk->num_columns());
    chunk->append_column(ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), true),
                         chunk->num_columns());
    chunk->append_column(
            ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_VARCHAR), true),
            chunk->num_columns());

    const std::string small_page_file = "./be/test/formats/parquet/test_data/page_index_small_page.parquet";

    Utils::SlotDesc min_max_slots[] = {
            {"c0", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), 0},
            {"c1", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), 1},
            {""},
    };

    auto ctx = _create_file_c0_c1_c2_context(small_page_file);
    auto file = _create_file(small_page_file);
    ctx->conjunct_ctxs_by_slot[0].clear();
    ctx->conjunct_ctxs_by_slot[1].clear();
    ctx->min_max_conjunct_ctxs.clear();
    ctx->min_max_tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, min_max_slots);

    std::vector<TExpr> t_conjuncts;
    // c0: 1->20000, c0 > 5000
    ParquetUTBase::append_int_conjunct(TExprOpcode::GT, 0, 5000, &t_conjuncts);
    // c1: 20000->1, c1 > 5000
    ParquetUTBase::append_int_conjunct(TExprOpcode::GT, 1, 5000, &t_conjuncts);

    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts, &ctx->min_max_conjunct_ctxs);

    std::vector<TExpr> t_conjuncts_slot0{t_conjuncts[0]};
    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts_slot0, &ctx->conjunct_ctxs_by_slot[0]);

    std::vector<TExpr> t_conjuncts_slot1{t_conjuncts[1]};
    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts_slot1, &ctx->conjunct_ctxs_by_slot[1]);

    auto shared_buffer = std::make_shared<io::SharedBufferedInputStream>(file->stream(), small_page_file,
                                                                         std::filesystem::file_size(small_page_file));
    auto file_reader = std::make_shared<FileReader>(config::vector_chunk_size, file.get(),
                                                    std::filesystem::file_size(small_page_file), DataCacheOptions(),
                                                    shared_buffer.get());

    Status status = file_reader->init(ctx);
    ASSERT_TRUE(status.ok());

    // two row groups.
    EXPECT_EQ(file_reader->_row_group_readers.size(), 2);
    std::vector<io::SharedBufferedInputStream::IORange> ranges;
    int64_t end_offset = 0;

    for (auto& r : file_reader->_row_group_readers) {
        r->collect_io_ranges(&ranges, &end_offset, ColumnIOType::PAGE_INDEX);
    }

    // collect io of column index and offset index for active column,
    // and offset index for lazy column
    // and two group collect together. (2 + 2 + 1) * 2 = 10
    EXPECT_EQ(ranges.size(), 10);

    ranges.clear();
    end_offset = 0;

    file_reader->_row_group_readers[0]->collect_io_ranges(&ranges, &end_offset);
    // only collect io of 5 pages, 5001-6000, 6001-7000, 7001-8000, 8001-9000, 9001-10000 and a dict page.
    // three columns, (5 + 1) * 3 = 18
    EXPECT_EQ(ranges.size(), 18);

    EXPECT_EQ(shared_buffer->current_range_ref_sum(), 28);

    // The second row group is not prepare yet

    size_t total_row_nums = 0;
    while (!status.is_end_of_file()) {
        chunk->reset();
        status = file_reader->get_next(&chunk);
        chunk->check_or_die();
        total_row_nums += chunk->num_rows();
    }
    EXPECT_EQ(total_row_nums, 10000);
}

TEST_F(PageIndexTest, TestPageIndexNoPageFiltered) {
    auto chunk = std::make_shared<Chunk>();
    chunk->append_column(ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), true),
                         chunk->num_columns());
    chunk->append_column(ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), true),
                         chunk->num_columns());
    chunk->append_column(
            ColumnHelper::create_column(TypeDescriptor::from_logical_type(LogicalType::TYPE_VARCHAR), true),
            chunk->num_columns());

    const std::string small_page_file = "./be/test/formats/parquet/test_data/page_index_small_page.parquet";

    Utils::SlotDesc min_max_slots[] = {
            {"c0", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), 0},
            {"c1", TypeDescriptor::from_logical_type(LogicalType::TYPE_INT), 1},
            {""},
    };

    auto ctx = _create_file_c0_c1_c2_context(small_page_file);
    auto file = _create_file(small_page_file);
    ctx->conjunct_ctxs_by_slot[0].clear();
    ctx->conjunct_ctxs_by_slot[1].clear();
    ctx->min_max_conjunct_ctxs.clear();
    ctx->min_max_tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, min_max_slots);

    std::vector<TExpr> t_conjuncts;
    // c0: 1->20000, c0 > 500
    ParquetUTBase::append_int_conjunct(TExprOpcode::GT, 0, 500, &t_conjuncts);
    // c1: 20000->1, c1 > 500
    ParquetUTBase::append_int_conjunct(TExprOpcode::GT, 1, 500, &t_conjuncts);

    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts, &ctx->min_max_conjunct_ctxs);

    std::vector<TExpr> t_conjuncts_slot0{t_conjuncts[0]};
    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts_slot0, &ctx->conjunct_ctxs_by_slot[0]);

    std::vector<TExpr> t_conjuncts_slot1{t_conjuncts[1]};
    ParquetUTBase::create_conjunct_ctxs(&_pool, _runtime_state, &t_conjuncts_slot1, &ctx->conjunct_ctxs_by_slot[1]);

    auto shared_buffer = std::make_shared<io::SharedBufferedInputStream>(file->stream(), small_page_file,
                                                                         std::filesystem::file_size(small_page_file));
    auto file_reader = std::make_shared<FileReader>(config::vector_chunk_size, file.get(),
                                                    std::filesystem::file_size(small_page_file), DataCacheOptions(),
                                                    shared_buffer.get());

    Status status = file_reader->init(ctx);
    ASSERT_TRUE(status.ok());

    // two row groups.
    EXPECT_EQ(file_reader->_row_group_readers.size(), 2);
    std::vector<io::SharedBufferedInputStream::IORange> ranges;
    int64_t end_offset = 0;

    for (auto& r : file_reader->_row_group_readers) {
        r->collect_io_ranges(&ranges, &end_offset, ColumnIOType::PAGE_INDEX);
    }

    // collect io of column index and offset index for active column,
    // and offset index for lazy column
    // and two group collect together. (2 + 2 + 1) * 2 = 10
    EXPECT_EQ(ranges.size(), 10);

    ranges.clear();
    end_offset = 0;

    file_reader->_row_group_readers[0]->collect_io_ranges(&ranges, &end_offset);
    // only collect io of 1 chunk / column.
    // three columns, 1 * 3 = 3
    EXPECT_EQ(ranges.size(), 3);

    EXPECT_EQ(shared_buffer->current_range_ref_sum(), 13);

    // The second row group is not prepare yet

    size_t total_row_nums = 0;
    while (!status.is_end_of_file()) {
        chunk->reset();
        status = file_reader->get_next(&chunk);
        chunk->check_or_die();
        total_row_nums += chunk->num_rows();
    }
    EXPECT_EQ(total_row_nums, 19000);
}


RuntimeFilterProbeDescriptor* PageIndexTest::gen_runtime_filter_desc(SlotId slot_id) {
    TRuntimeFilterDescription tRuntimeFilterDescription;
    tRuntimeFilterDescription.__set_filter_id(1);
    tRuntimeFilterDescription.__set_has_remote_targets(false);
    tRuntimeFilterDescription.__set_build_plan_node_id(1);
    tRuntimeFilterDescription.__set_build_join_mode(TRuntimeFilterBuildJoinMode::BORADCAST);
    tRuntimeFilterDescription.__set_filter_type(TRuntimeFilterBuildType::TOPN_FILTER);

    // Create TypeDescriptor with precision=30, scale=8 for DECIMAL128
    TypeDescriptor decimal_type(LogicalType::TYPE_DECIMAL128);
    decimal_type.precision = 30;
    decimal_type.scale = 8;

    TExpr expr;
    expr.nodes.emplace_back(TExprNode());
    expr.nodes[0].__set_type(decimal_type.to_thrift());
    expr.nodes[0].__set_node_type(TExprNodeType::SLOT_REF);
    expr.nodes[0].__set_is_nullable(true);
    expr.nodes[0].__set_slot_ref(TSlotRef());
    expr.nodes[0].slot_ref.__set_slot_id(slot_id);

    tRuntimeFilterDescription.__isset.plan_node_id_to_target_expr = true;
    tRuntimeFilterDescription.plan_node_id_to_target_expr.emplace(1, expr);

    auto* runtime_filter_desc = _pool.add(new RuntimeFilterProbeDescriptor());
    runtime_filter_desc->init(&_pool, tRuntimeFilterDescription, 1, _runtime_state);

    return runtime_filter_desc;
}

StatusOr<HdfsScannerContext*> PageIndexTest::create_context_for_rf_decimal128(SlotId slot_id, int128_t start, int128_t end, bool has_null) {
    const std::string decimal_file = "./be/test/formats/parquet/test_data/page_index_decimal128.parquet";
    if (!std::filesystem::exists(decimal_file)) {
        std::cout << "Skip create_context_for_rf_decimal128: test file not found: " << decimal_file << std::endl;
        return Status::InternalError("test file not found");
    }

    // Create TypeDescriptor with precision=30, scale=8 for DECIMAL128
    TypeDescriptor decimal_type(LogicalType::TYPE_DECIMAL128);
    decimal_type.precision = 30;
    decimal_type.scale = 8;

    Utils::SlotDesc slot_descs[] = {
            {"c_decimal", decimal_type},
            {""},
    };

    auto ctx = _create_scan_context();
    ctx->tuple_desc = Utils::create_tuple_descriptor(_runtime_state, &_pool, slot_descs);
    Utils::make_column_info_vector(ctx->tuple_desc, &ctx->materialized_columns);
    ctx->scan_range = _create_scan_range(decimal_file);

    // 1. Create RuntimeBloomFilter and set min/max values
    using CppType = RunTimeCppType<TYPE_DECIMAL128>;
    auto* rf = _pool.add(new RuntimeBloomFilter<TYPE_DECIMAL128>());
    rf->init(10);
    rf->insert(CppType(start));
    rf->insert(CppType(end));
    if (has_null) {
        rf->insert_null();
    }

    // 2. Create RuntimeFilterProbeDescriptor with probe expr (slot ref)
    auto* rf_desc = gen_runtime_filter_desc(slot_id);

    // 3. Set runtime filter to probe descriptor
    rf_desc->set_runtime_filter(rf);

    // 4. Create RuntimeFilterProbeCollector for RuntimeFilter mechanism
    auto* rf_collector = _pool.add(new RuntimeFilterProbeCollector());
    rf_collector->add_descriptor(rf_desc);

    // 5. Set collector to scanner context
    ctx->runtime_filter_collector = rf_collector;

    // 6. Create MinMaxPredicate from RuntimeBloomFilter for PageIndex filtering
    Expr* min_max_expr = MinMaxPredicateBuilder(&_pool, slot_id, rf, decimal_type).operator()<TYPE_DECIMAL128>();
    auto* min_max_ctx = _pool.add(new ExprContext(min_max_expr));
    min_max_ctx->prepare(_runtime_state);
    min_max_ctx->open(_runtime_state);

    // 7. Set conjunct_ctxs_by_slot for PageIndex filtering
    ctx->conjunct_ctxs_by_slot[slot_id].push_back(min_max_ctx);

    return ctx;
}


TEST_F(PageIndexTest, TestDecimal128MinMaxFilter_FilterAll) {
    // Test DECIMAL128(30,8) with RuntimeFilter MinMax - Filter ALL rows
    // This test verifies that when RF range does not overlap with data,
    // all rows are correctly filtered out.
    //
    // Test file: page_index_decimal128.parquet
    // Data: c_decimal values [333.3, 444.4, 555.5] repeating (501 rows)
    // Min: 333.30000000, Max: 555.50000000, No nulls
    // File has: ColumnIndex and OffsetIndex (PageIndex)
    //
    // RF range: [100, 200] - does not overlap with data [333.3, 555.5]
    // Expected: ALL rows filtered, 0 rows returned

    auto chunk = std::make_shared<Chunk>();
    TypeDescriptor decimal_type(LogicalType::TYPE_DECIMAL128);
    decimal_type.precision = 30;
    decimal_type.scale = 8;

    chunk->append_column(ColumnHelper::create_column(decimal_type, true), chunk->num_columns());

    const std::string decimal_file = "./be/test/formats/parquet/test_data/page_index_decimal128.parquet";
    auto file = _create_file(decimal_file);
    auto shared_buffer = std::make_shared<io::SharedBufferedInputStream>(
        file->stream(), decimal_file, std::filesystem::file_size(decimal_file));
    auto file_reader = std::make_shared<FileReader>(config::vector_chunk_size, file.get(),
                                                    std::filesystem::file_size(decimal_file),
                                                    DataCacheOptions(), shared_buffer.get());

    // Use slot_id = 0 (must match the slot id in tuple descriptor)
    SlotId slot_id = 0;
    auto ret = create_context_for_rf_decimal128(slot_id, 10000000000LL, 20000000000LL, false);
    ASSERT_TRUE(ret.ok());
    HdfsScannerContext* ctx = ret.value();
    Status status = file_reader->init(ctx);
    ASSERT_TRUE(status.ok()) << "Failed to init file reader: " << status.message();

    size_t total_row_nums = 0;
    while (!status.is_end_of_file()) {
        chunk->reset();
        status = file_reader->get_next(&chunk);
        if (!status.ok() && !status.is_end_of_file()) {
            std::cout << "Error reading file: " << status.message() << std::endl;
            break;
        }
        chunk->check_or_die();
        total_row_nums += chunk->num_rows();
    }

    std::cout << "TestDecimal128MinMaxFilter_FilterAll [100,200]: total rows read = " << total_row_nums << std::endl;
    // All rows should be filtered (0 rows returned)
    EXPECT_EQ(total_row_nums, 0) << "Expected 0 rows, but got " << total_row_nums;
}

TEST_F(PageIndexTest, TestDecimal128MinMaxFilter_FilterPartial) {
    // Test DECIMAL128(30,8) with RuntimeFilter MinMax - Filter PARTIAL rows
    // This test verifies that when RF range partially overlaps with data,
    // only matching rows are returned.
    //
    // Test file: page_index_decimal128.parquet
    // Data: c_decimal values [333.3, 444.4, 555.5] repeating (501 rows)
    // Min: 333.30000000, Max: 555.50000000, No nulls
    // File has: ColumnIndex and OffsetIndex (PageIndex)
    //
    // RF range: [350, 500] - only overlaps with 444.4
    // Expected: ~167 rows (only 444.4 values pass)

    auto chunk = std::make_shared<Chunk>();
    TypeDescriptor decimal_type(LogicalType::TYPE_DECIMAL128);
    decimal_type.precision = 30;
    decimal_type.scale = 8;

    chunk->append_column(ColumnHelper::create_column(decimal_type, true), chunk->num_columns());

    const std::string decimal_file = "./be/test/formats/parquet/test_data/page_index_decimal128.parquet";
    auto file = _create_file(decimal_file);
    auto shared_buffer = std::make_shared<io::SharedBufferedInputStream>(
        file->stream(), decimal_file, std::filesystem::file_size(decimal_file));
    auto file_reader = std::make_shared<FileReader>(config::vector_chunk_size, file.get(),
                                                    std::filesystem::file_size(decimal_file),
                                                    DataCacheOptions(), shared_buffer.get());

    // Use slot_id = 0 (must match the slot id in tuple descriptor)
    SlotId slot_id = 0;
    auto ret = create_context_for_rf_decimal128(slot_id, 35000000000LL, 50000000000LL, false); 
    ASSERT_TRUE(ret.ok());
    HdfsScannerContext* ctx = ret.value();
    Status status = file_reader->init(ctx);
    ASSERT_TRUE(status.ok()) << "Failed to init file reader: " << status.message();

    size_t total_row_nums = 0;
    while (!status.is_end_of_file()) {
        chunk->reset();
        status = file_reader->get_next(&chunk);
        if (!status.ok() && !status.is_end_of_file()) {
            std::cout << "Error reading file: " << status.message() << std::endl;
            break;
        }
        chunk->check_or_die();
        total_row_nums += chunk->num_rows();
    }

    std::cout << "TestDecimal128MinMaxFilter_FilterPartial [350,500]: total rows read = " << total_row_nums << std::endl;
    // Should read ~167 rows (only 444.4 values pass the filter)
    EXPECT_GT(total_row_nums, 0) << "All rows were incorrectly filtered by PageIndex";
    EXPECT_LT(total_row_nums, 250) << "Too many rows, filter may not be working correctly";
}

TEST_F(PageIndexTest, TestDecimal128MinMaxFilter_FilterNone) {
    // Test DECIMAL128(30,8) with RuntimeFilter MinMax - Filter NO rows
    // This test verifies that when RF range covers all data,
    // all rows are returned (no filtering).
    //
    // Test file: page_index_decimal128.parquet
    // Data: c_decimal values [333.3, 444.4, 555.5] repeating (501 rows)
    // Min: 333.30000000, Max: 555.50000000, No nulls
    // File has: ColumnIndex and OffsetIndex (PageIndex)
    //
    // RF range: [300, 600] - covers all data [333.3, 555.5]
    // Expected: 501 rows (all rows pass)

    auto chunk = std::make_shared<Chunk>();
    TypeDescriptor decimal_type(LogicalType::TYPE_DECIMAL128);
    decimal_type.precision = 30;
    decimal_type.scale = 8;
    chunk->append_column(ColumnHelper::create_column(decimal_type, true), chunk->num_columns());

    const std::string decimal_file = "./be/test/formats/parquet/test_data/page_index_decimal128.parquet";
    auto file = _create_file(decimal_file);
    auto shared_buffer = std::make_shared<io::SharedBufferedInputStream>(
        file->stream(), decimal_file, std::filesystem::file_size(decimal_file));
    auto file_reader = std::make_shared<FileReader>(config::vector_chunk_size, file.get(),
                                                    std::filesystem::file_size(decimal_file),
                                                    DataCacheOptions(), shared_buffer.get());
    
    // Use slot_id = 0 (must match the slot id in tuple descriptor)
    SlotId slot_id = 0;
    auto ret = create_context_for_rf_decimal128(slot_id, 30000000000LL, 60000000000LL, false); 
    ASSERT_TRUE(ret.ok());
    HdfsScannerContext* ctx = ret.value();
    Status status = file_reader->init(ctx);
    ASSERT_TRUE(status.ok()) << "Failed to init file reader: " << status.message();

    size_t total_row_nums = 0;
    while (!status.is_end_of_file()) {
        chunk->reset();
        status = file_reader->get_next(&chunk);
        if (!status.ok() && !status.is_end_of_file()) {
            std::cout << "Error reading file: " << status.message() << std::endl;
            break;
        }
        chunk->check_or_die();
        total_row_nums += chunk->num_rows();
    }

    std::cout << "TestDecimal128MinMaxFilter_FilterNone [300,600]: total rows read = " << total_row_nums << std::endl;
    // All 501 rows should be returned
    EXPECT_EQ(total_row_nums, 501) << "Expected 501 rows, but got " << total_row_nums;
}

} // namespace starrocks::parquet
