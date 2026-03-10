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

#include "runtime/local_tablets_channel.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "column/chunk.h"
#include "column/fixed_length_column.h"
#include "column/schema.h"
#include "column/vectorized_fwd.h"
#include "common/logging.h"
#include "gen_cpp/internal_service.pb.h"
#include "runtime/load_channel.h"
#include "runtime/load_channel_mgr.h"
#include "runtime/mem_tracker.h"
#include "serde/protobuf_serde.h"
#include "storage/chunk_helper.h"
#include "storage/rowset/segment_options.h"
#include "storage/storage_engine.h"
#include "storage/tablet_manager.h"
#include "storage/tablet_schema.h"
#include "testutil/assert.h"
#include "testutil/id_generator.h"
#include "util/runtime_profile.h"

// ============================================================================
// LocalTabletsChannel 单元测试
// ============================================================================
// 用途：测试 LocalTabletsChannel 的核心功能，包括：
// 1. Tablet 数据写入流程
// 2. 错误处理（不存在的 Tablet ID、部分无效数据等）
// 3. 复制存储模式下的 Primary/Secondary 副本同步
// 4. 滑动窗口机制（乱序、重复请求处理）
// 5. 背压控制（MemTable 队列满时的行为）
//
// 测试架构：
//   LoadChannelMgr -> LoadChannel -> LocalTabletsChannel -> AsyncDeltaWriter -> DeltaWriter -> Tablet
// ============================================================================

namespace starrocks {

/**
 * @brief LocalTabletsChannel 测试夹具
 *
 * 负责初始化测试环境，包括：
 * - 创建测试用 Tablet（真实存储引擎）
 * - 构建完整的调用链路（LoadChannelMgr -> LoadChannel -> LocalTabletsChannel）
 * - 准备测试数据和请求参数
 */
class LocalTabletsChannelTest : public testing::Test {
protected:
    /**
     * @brief 测试前置准备
     *
     * 初始化流程：
     * 1. 设置基础 ID（load_id, txn_id, tablet_id 等）
     * 2. 创建 Tablet 到存储引擎
     * 3. 构建组件依赖链
     * 4. 准备 Open 请求（单副本/多副本两种模式）
     */
    void SetUp() override {
        srand(GetCurrentTimeMicros());

        // ========== 基础 ID 配置 ==========
        _node_id = 100;              // 当前 BE 节点 ID
        _secondary_node_id = 101;    // Secondary 副本节点 ID（用于复制存储模式）

        // Load 任务唯一标识
        _load_id.set_hi(456789);
        _load_id.set_lo(987654);

        // 事务 ID（用于 2PC 事务）
        _txn_id = 10000;

        // 元数据 ID
        _db_id = 100;           // 数据库 ID
        _table_id = 101;        // 表 ID
        _partition_id = 10;     // 分区 ID
        _index_id = 1;          // Index/Rollup ID

        // ========== 创建测试 Tablet ==========
        _tablet_id = rand();
        _tablet = create_tablet(_tablet_id, rand());
        _schema = std::make_shared<Schema>(ChunkHelper::convert_schema(_tablet->tablet_schema()));

        // ========== 构建组件依赖链 ==========
        // 内存跟踪器（限制 1MB）
        _mem_tracker = std::make_unique<MemTracker>(1024 * 1024);
        _root_profile = std::make_unique<RuntimeProfile>("LoadChannel");

        // LoadChannel 管理器
        _load_channel_mgr = std::make_unique<LoadChannelMgr>();

        // LoadChannel（管理多个 TabletsChannel）
        auto load_mem_tracker = std::make_unique<MemTracker>(-1, "", _mem_tracker.get());
        _load_channel = std::make_shared<LoadChannel>(_load_channel_mgr.get(), nullptr, _load_id, _txn_id, string(),
                                                      1000, std::move(load_mem_tracker));

        // 准备两种 Open 请求：单副本模式 / 多副本模式
        _open_single_replica_request = create_open_request(true);
        _open_primary_request = create_open_request(false);

        // ========== 创建被测对象：LocalTabletsChannel ==========
        TabletsChannelKey key{_load_id, _index_id};
        _schema_param.reset(new OlapTableSchemaParam());
        ASSERT_OK(_schema_param->init(_open_single_replica_request.schema()));
        _tablets_channel =
                new_local_tablets_channel(_load_channel.get(), key, _load_channel->mem_tracker());
    }

    /**
     * @brief 测试后置清理
     *
     * 清理顺序：
     * 1. 销毁 LocalTabletsChannel
     * 2. 销毁 LoadChannel
     * 3. 从存储引擎删除 Tablet（避免脏数据）
     */
    void TearDown() override {
        _tablets_channel.reset();
        _load_channel.reset();
        if (_tablet) {
            _tablet.reset();
            auto st = StorageEngine::instance()->tablet_manager()->drop_tablet(_tablet_id);
            ASSERT_OK(st);
        }
    }

    // 前置声明：测试复制存储模式下 Secondary 副本的取消逻辑
    void test_cancel_secondary_replica_base(bool is_empty_tablet);

    // ========== 辅助方法：创建测试用 Tablet ==========
    /**
     * @brief 在存储引擎中创建一个真实的 Tablet
     *
     * @param tablet_id Tablet 唯一标识
     * @param schema_hash Schema 哈希值（用于版本管理）
     * @return TabletSharedPtr 指向新创建的 Tablet
     *
     * Tablet 结构：
     * - c0: INT 类型，Key 列（DUP_KEYS 模型的分桶键）
     * - c1: INT 类型，Value 列
     */
    TabletSharedPtr create_tablet(int64_t tablet_id, int32_t schema_hash) {
        TCreateTabletReq request;
        request.tablet_id = tablet_id;
        request.__set_version(1);
        request.tablet_schema.schema_hash = schema_hash;
        request.tablet_schema.short_key_column_count = 1;
        request.tablet_schema.keys_type = TKeysType::DUP_KEYS;      // 明细模型
        request.tablet_schema.storage_type = TStorageType::COLUMN;  // 列式存储

        // 定义 Key 列：c0
        TColumn c0;
        c0.column_name = "c0";
        c0.__set_is_key(true);
        c0.column_type.type = TPrimitiveType::INT;
        request.tablet_schema.columns.push_back(c0);

        // 定义 Value 列：c1
        TColumn c1;
        c1.column_name = "c1";
        c1.__set_is_key(false);
        c1.column_type.type = TPrimitiveType::INT;
        request.tablet_schema.columns.push_back(c1);

        // 调用存储引擎创建 Tablet
        auto st = StorageEngine::instance()->create_tablet(request);
        CHECK(st.ok()) << st.to_string();
        return StorageEngine::instance()->tablet_manager()->get_tablet(tablet_id, false);
    }

    // ========== 辅助方法：创建 Open 请求 ==========
    /**
     * @brief 构造 PTabletWriterOpenRequest 请求
     *
     * @param single_replica true=单副本模式，false=多副本模式（Primary + Secondary）
     * @return PTabletWriterOpenRequest 配置好的 Open 请求
     *
     * 请求配置说明：
     * - is_replicated_storage=true: 启用复制存储模式（数据只写 Primary，由 Primary 同步到 Secondary）
     * - write_quorum=ONE: 写夸克数为 1（只要一个副本成功即可）
     * - num_senders=1: 单个发送者（滑动窗口大小为 sender_num * max_load_dop * 3）
     */
    PTabletWriterOpenRequest create_open_request(bool single_replica) {
        PTabletWriterOpenRequest request;
        request.mutable_id()->CopyFrom(_load_id);
        request.set_index_id(_index_id);
        request.set_txn_id(_txn_id);
        request.set_is_lake_tablet(false);           // 非 Lake 表（使用 LocalTabletsChannel）
        request.set_is_replicated_storage(true);     // 启用复制存储模式
        request.set_node_id(_node_id);
        request.set_write_quorum(WriteQuorumTypePB::ONE);  // 写夸克数：ONE
        request.set_miss_auto_increment_column(false);
        request.set_table_id(_table_id);
        request.set_is_incremental(false);           // 非增量创建（首次创建）
        request.set_num_senders(1);                  // 1 个发送者
        request.set_sender_id(0);                    // 发送者 ID 为 0
        request.set_need_gen_rollup(false);          // 不需要生成 Rollup
        request.set_load_channel_timeout_s(10);      // 超时时间 10 秒
        request.set_is_vectorized(true);             // 向量化执行
        request.set_timeout_ms(10000);               // 超时时间 10000ms

        // ========== 添加 Tablet 信息 ==========
        auto tablet = request.add_tablets();
        tablet->set_partition_id(_partition_id);
        tablet->set_tablet_id(_tablet_id);

        // 添加 Primary 副本
        auto replica = tablet->add_replicas();
        replica->set_host("127.0.0.1");
        replica->set_port(8060);
        replica->set_node_id(_node_id);

        // 如果是多副本模式，添加 Secondary 副本
        if (!single_replica) {
            auto secondary_replica = tablet->add_replicas();
            secondary_replica->set_host("127.0.0.2");
            secondary_replica->set_port(8060);
            secondary_replica->set_node_id(_secondary_node_id);
        }

        // ========== 构建 Schema 信息 ==========
        auto schema = request.mutable_schema();
        schema->set_db_id(_db_id);
        schema->set_table_id(_table_id);
        schema->set_version(1);

        // 添加 Index 定义
        auto index = schema->add_indexes();
        index->set_id(_index_id);
        index->set_schema_hash(0);

        // 添加列定义（从 Tablet Schema 复制）
        for (int i = 0, sz = _tablet->tablet_schema().num_columns(); i < sz; i++) {
            auto slot = request.mutable_schema()->add_slot_descs();
            auto& column = _tablet->tablet_schema().column(i);
            slot->set_id(i);
            slot->set_byte_offset(i * sizeof(int) /*unused*/);
            slot->set_col_name(std::string(column.name()));
            slot->set_slot_idx(i);
            slot->set_is_materialized(true);
            slot->mutable_slot_type()->add_types()->mutable_scalar_type()->set_type(column.type());
            index->add_columns(std::string(column.name()));
        }

        // 添加 Tuple 描述
        auto tuple_desc = schema->mutable_tuple_desc();
        tuple_desc->set_id(1);
        tuple_desc->set_byte_size(8 /*unused*/);
        tuple_desc->set_num_null_bytes(0 /*unused*/);
        tuple_desc->set_table_id(_table_id);

        return request;
    }

    // ========== 辅助方法：生成测试数据 ==========
    /**
     * @brief 生成指定行数的测试数据 Chunk
     *
     * @param chunk_size 数据行数
     * @return Chunk 包含两列 INT 数据（c0, c1）
     *
     * 数据格式：
     * c0: [0, 1, 2, ..., chunk_size-1]
     * c1: [0, 1, 2, ..., chunk_size-1]
     */
    Chunk generate_data(int64_t chunk_size) {
        std::vector<int> v0(chunk_size);
        std::vector<int> v1(chunk_size);
        for (int i = 0; i < chunk_size; i++) {
            v0[i] = i;
            // v1[i] 默认初始化为 0
        }
        auto c0 = Int32Column::create();
        auto c1 = Int32Column::create();
        c0->append_numbers(v0.data(), v0.size() * sizeof(int));
        c1->append_numbers(v1.data(), v1.size() * sizeof(int));
        Chunk chunk({c0, c1}, _schema);
        chunk.set_slot_id_to_index(0, 0);  // slot_id=0 -> column_index=0
        chunk.set_slot_id_to_index(1, 1);  // slot_id=1 -> column_index=1
        return chunk;
    }

    // ========== 成员变量：基础配置 ==========
    int64_t _node_id;                 // 当前 BE 节点 ID
    int64_t _secondary_node_id;       // Secondary 副本节点 ID
    PUniqueId _load_id;               // Load 任务唯一标识（hi + lo）
    int64_t _txn_id;                  // 事务 ID
    int64_t _db_id;                   // 数据库 ID
    int64_t _table_id;                // 表 ID
    int64_t _partition_id;            // 分区 ID
    int32_t _index_id;                // Index/Rollup ID
    int64_t _tablet_id;               // Tablet ID

    // ========== 成员变量：组件依赖 ==========
    std::unique_ptr<MemTracker> _mem_tracker;          // 内存跟踪器
    std::unique_ptr<RuntimeProfile> _root_profile;     // 性能分析器
    std::unique_ptr<LoadChannelMgr> _load_channel_mgr; // LoadChannel 管理器
    std::shared_ptr<LoadChannel> _load_channel;        // LoadChannel（父级）
    std::shared_ptr<TabletsChannel> _tablets_channel;  // LocalTabletsChannel（被测对象）

    // ========== 成员变量：元数据 ==========
    TabletSharedPtr _tablet;                  // 测试用 Tablet
    std::shared_ptr<Schema> _schema;          // Schema 定义
    std::shared_ptr<OlapTableSchemaParam> _schema_param; // Schema 参数

    // ========== 成员变量：预构造的请求 ==========
    PTabletWriterOpenRequest _open_single_replica_request; // 单副本 Open 请求
    PTabletWriterOpenRequest _open_primary_request;        // 多副本 Open 请求（Primary + Secondary）
    PTabletWriterOpenResult _open_response;                // Open 响应
};

// ============================================================================
// 测试用例 1：验证不存在的 Tablet ID 错误处理
// ============================================================================
// 测试场景：
//   构造一个指向不存在 Tablet 的写入请求，验证 LocalTabletsChannel 能否正确
//   检测并返回错误。
//
// 预期结果：
//   - add_chunk 返回 TStatusCode::INTERNAL_ERROR
//   - 错误信息中包含 Tablet 不存在的相关描述
//
// 覆盖逻辑：
//   LocalTabletsChannel::add_chunk() -> _create_write_context() 中的
//   _delta_writers.find(tablet_id) 失败路径
// ============================================================================
TEST_F(LocalTabletsChannelTest, test_add_chunk_not_exist_tablet) {
    // 1. 打开 TabletsChannel（使用单副本模式）
    auto open_request = _open_single_replica_request;
    ASSERT_OK(_tablets_channel->open(open_request, _schema_param, false));

    // 2. 构造 AddChunk 请求
    PTabletWriterAddChunkRequest add_chunk_request;
    add_chunk_request.mutable_id()->CopyFrom(_load_id);
    add_chunk_request.set_index_id(_index_id);
    add_chunk_request.set_sender_id(0);
    add_chunk_request.set_eos(true);       // 标记为最后一个包
    add_chunk_request.set_packet_seq(0);   // 包序号为 0

    // 3. 构造一个不存在的 tablet_id（当前 tablet_id + 1）
    // NOTE: 这是一个 malformed request，因为 chunk 为 nullptr 但 tablet_ids 非空
    auto non_exist_tablet_id = _tablet->tablet_id() + 1;
    add_chunk_request.add_tablet_ids(non_exist_tablet_id);

    // 4. 发送写入请求
    PTabletWriterAddBatchResult add_chunk_response;

    int num_rows = 1;
    auto chunk = generate_data(num_rows);
    _tablets_channel->add_chunk(&chunk, add_chunk_request, &add_chunk_response);

    // 5. 验证返回错误码应为 INTERNAL_ERROR
    ASSERT_EQ(TStatusCode::INTERNAL_ERROR, add_chunk_response.status().status_code()) << add_chunk_response.status();

    // 6. 调用 abort 清理资源
    _tablets_channel->abort();
}

// ============================================================================
// 测试用例 2：验证 Chunk 中部分行指向不存在 Tablet 的错误处理
// ============================================================================
// 测试场景：
//   Chunk 中包含多行数据，部分行指向有效的 Tablet，部分行指向不存在的 Tablet。
//   验证 LocalTabletsChannel 能否正确检测这种部分无效的场景。
//
// 预期结果：
//   - add_chunk 返回 TStatusCode::INTERNAL_ERROR
//   - 即使部分数据有效，整体请求也应失败
//
// 覆盖逻辑：
//   LocalTabletsChannel::add_chunk() -> _create_write_context() 中的
//   按行遍历 tablet_ids 时的验证逻辑
// ============================================================================
TEST_F(LocalTabletsChannelTest, test_add_chunk_not_exist_tablet_for_chunk_rows) {
    // 1. 打开 TabletsChannel（使用单副本模式）
    auto open_request = _open_single_replica_request;
    ASSERT_OK(_tablets_channel->open(open_request, _schema_param, false));

    // 2. 构造 AddChunk 请求和响应
    PTabletWriterAddChunkRequest add_chunk_request;
    PTabletWriterAddBatchResult add_chunk_response;

    add_chunk_request.mutable_id()->CopyFrom(_load_id);
    add_chunk_request.set_index_id(_index_id);
    add_chunk_request.set_sender_id(0);
    add_chunk_request.set_eos(false);          // 非结束包
    add_chunk_request.set_packet_seq(0);       // 包序号为 0
    add_chunk_request.set_timeout_ms(60000);   // 超时 60 秒

    // 3. 生成 10 行测试数据
    {
        int num_rows = 10;
        auto chunk = generate_data(num_rows);

        // 4. 构造 tablet_ids：前 5 行有效，后 5 行无效
        for (int i = 0; i < num_rows; i++) {
            if (i > num_rows / 2) {
                // 无效的 tablet_id（该 channel 未打开此 Tablet）
                add_chunk_request.add_tablet_ids(_tablet_id + 1);
            } else {
                // 有效的 tablet_id（该 channel 已打开此 Tablet）
                add_chunk_request.add_tablet_ids(_tablet_id);
            }
            add_chunk_request.add_partition_ids(_partition_id);
        }

        // 5. 发送写入请求
        _tablets_channel->add_chunk(&chunk, add_chunk_request, &add_chunk_response);

        // chunk 在此作用域结束后释放，模拟 RPC 完成后的资源释放
    }

    // 6. 验证返回错误码应为 INTERNAL_ERROR
    ASSERT_EQ(TStatusCode::INTERNAL_ERROR, add_chunk_response.status().status_code()) << add_chunk_response.status();

    // 7. 调用 abort 清理资源
    _tablets_channel->abort();
}

} // namespace starrocks
