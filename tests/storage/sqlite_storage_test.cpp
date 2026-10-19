#include "sqlite/sqlite_async_executor.h"
#include "sqlite/sqlite_connection.h"
#include "sqlite/sqlite_connection_pool.h"
#include "sqlite/sqlite_statement.h"
#include "sqlite/sqlite_transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using storage::sqlite::SqliteConnection;
using storage::sqlite::SqliteConnectionPool;
using storage::sqlite::SqliteConnectionPoolOptions;
using storage::sqlite::SqliteStepResult;
using storage::sqlite::SqliteTransaction;

std::filesystem::path TestDatabasePath(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return path;
}

void InitializeFileDatabase(const std::filesystem::path& path) {
    auto connection_result = SqliteConnection::Open(path.string());
    ASSERT_TRUE(connection_result.ok()) << connection_result.status().message();
    auto connection = std::move(connection_result).value();
    ASSERT_TRUE(connection.EnableWal().ok());
    ASSERT_TRUE(connection.Execute("CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT NOT NULL)").ok());
}

SqliteConnection OpenMemoryDatabase() {
    auto connection_result = SqliteConnection::OpenMemory();
    EXPECT_TRUE(connection_result.ok()) << connection_result.status().message();
    return std::move(connection_result).value();
}

int CountRows(SqliteConnection& connection) {
    auto statement_result = connection.Prepare("SELECT COUNT(*) FROM items");
    EXPECT_TRUE(statement_result.ok()) << statement_result.status().message();
    auto statement = std::move(statement_result).value();

    auto step_result = statement.Step();
    EXPECT_TRUE(step_result.ok()) << step_result.status().message();
    EXPECT_EQ(std::move(step_result).value(), SqliteStepResult::Row);
    return statement.ColumnInt(0);
}

std::uint64_t SimulateCpuWork(int seed) {
    std::uint64_t value = static_cast<std::uint64_t>(seed + 1);
    for (int i = 0; i < 2048; ++i) {
        value = value * 1664525u + 1013904223u;
        value ^= value >> 17;
    }
    return value;
}

TEST(SqliteStorageTest, OpensInMemoryDatabaseAndExecutesSchema) {
    auto connection = OpenMemoryDatabase();
    ASSERT_TRUE(connection);

    auto status = connection.Execute(
        "CREATE TABLE items ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "score REAL NOT NULL,"
        "payload BLOB,"
        "optional TEXT"
        ")");
    ASSERT_TRUE(status.ok()) << status.message();

    EXPECT_EQ(connection.Changes(), 0);
}

TEST(SqliteStorageTest, PreparesBindsStepsAndQueriesValues) {
    auto connection = OpenMemoryDatabase();
    ASSERT_TRUE(connection.Execute(
                    "CREATE TABLE items ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "name TEXT NOT NULL,"
                    "score REAL NOT NULL,"
                    "payload BLOB,"
                    "optional TEXT"
                    ")")
                    .ok());

    auto insert_result = connection.Prepare("INSERT INTO items(name, score, payload, optional) VALUES(?1, ?2, ?3, ?4)");
    ASSERT_TRUE(insert_result.ok()) << insert_result.status().message();
    auto insert = std::move(insert_result).value();

    constexpr std::array<std::byte, 3> payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x7F}};
    ASSERT_TRUE(insert.BindText(1, "alpha").ok());
    ASSERT_TRUE(insert.BindDouble(2, 9.5).ok());
    ASSERT_TRUE(insert.BindBlob(3, payload).ok());
    ASSERT_TRUE(insert.BindNull(4).ok());

    auto insert_step = insert.Step();
    ASSERT_TRUE(insert_step.ok()) << insert_step.status().message();
    EXPECT_EQ(std::move(insert_step).value(), SqliteStepResult::Done);
    EXPECT_EQ(connection.Changes(), 1);
    EXPECT_EQ(connection.LastInsertRowId(), 1);

    auto query_result = connection.Prepare("SELECT id, name, score, payload, optional FROM items WHERE id = ?1");
    ASSERT_TRUE(query_result.ok()) << query_result.status().message();
    auto query = std::move(query_result).value();
    ASSERT_TRUE(query.BindInt64(1, 1).ok());

    auto row = query.Step();
    ASSERT_TRUE(row.ok()) << row.status().message();
    ASSERT_EQ(std::move(row).value(), SqliteStepResult::Row);
    ASSERT_EQ(query.ColumnCount(), 5);
    EXPECT_EQ(query.ColumnInt64(0), 1);
    EXPECT_EQ(query.ColumnText(1), "alpha");
    EXPECT_DOUBLE_EQ(query.ColumnDouble(2), 9.5);
    EXPECT_TRUE(query.ColumnIsNull(4));

    const auto observed_payload = query.ColumnBlob(3);
    ASSERT_EQ(observed_payload.size(), payload.size());
    EXPECT_EQ(observed_payload[0], payload[0]);
    EXPECT_EQ(observed_payload[1], payload[1]);
    EXPECT_EQ(observed_payload[2], payload[2]);

    auto done = query.Step();
    ASSERT_TRUE(done.ok()) << done.status().message();
    EXPECT_EQ(std::move(done).value(), SqliteStepResult::Done);
}

TEST(SqliteStorageTest, ResetAndClearBindingsAllowStatementReuse) {
    auto connection = OpenMemoryDatabase();
    ASSERT_TRUE(connection.Execute("CREATE TABLE items (id INTEGER PRIMARY KEY, value INTEGER)").ok());

    auto insert_result = connection.Prepare("INSERT INTO items(id, value) VALUES(?1, ?2)");
    ASSERT_TRUE(insert_result.ok()) << insert_result.status().message();
    auto insert = std::move(insert_result).value();

    ASSERT_TRUE(insert.BindInt(1, 1).ok());
    ASSERT_TRUE(insert.BindInt(2, 10).ok());
    ASSERT_TRUE(insert.Step().ok());
    ASSERT_TRUE(insert.Reset().ok());
    ASSERT_TRUE(insert.ClearBindings().ok());

    ASSERT_TRUE(insert.BindInt(1, 2).ok());
    ASSERT_TRUE(insert.BindInt(2, 20).ok());
    ASSERT_TRUE(insert.Step().ok());

    auto query_result = connection.Prepare("SELECT SUM(value) FROM items");
    ASSERT_TRUE(query_result.ok()) << query_result.status().message();
    auto query = std::move(query_result).value();
    auto step = query.Step();
    ASSERT_TRUE(step.ok()) << step.status().message();
    EXPECT_EQ(std::move(step).value(), SqliteStepResult::Row);
    EXPECT_EQ(query.ColumnInt(0), 30);
}

TEST(SqliteStorageTest, CommitTransactionPersistsChanges) {
    auto connection = OpenMemoryDatabase();
    ASSERT_TRUE(connection.Execute("CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT)").ok());

    SqliteTransaction transaction(connection);
    ASSERT_TRUE(transaction.Begin().ok());
    ASSERT_TRUE(connection.Execute("INSERT INTO items(value) VALUES('committed')").ok());
    ASSERT_TRUE(transaction.Commit().ok());
    EXPECT_FALSE(transaction.active());

    EXPECT_EQ(CountRows(connection), 1);
}

TEST(SqliteStorageTest, DestructorRollsBackActiveTransaction) {
    auto connection = OpenMemoryDatabase();
    ASSERT_TRUE(connection.Execute("CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT)").ok());

    {
        SqliteTransaction transaction(connection);
        ASSERT_TRUE(transaction.Begin().ok());
        ASSERT_TRUE(connection.Execute("INSERT INTO items(value) VALUES('rolled back')").ok());
    }

    EXPECT_EQ(CountRows(connection), 0);
}

TEST(SqliteStorageTest, InvalidSqlMapsToStatus) {
    auto connection = OpenMemoryDatabase();

    auto prepare_result = connection.Prepare("SELECT FROM broken");
    ASSERT_FALSE(prepare_result.ok());
    EXPECT_NE(prepare_result.status().code(), core::ErrorCode::Ok);
    EXPECT_NE(prepare_result.status().message().find("prepare SQLite statement failed"), std::string::npos);

    auto execute_status = connection.Execute("CREATE TABLE");
    ASSERT_FALSE(execute_status.ok());
    EXPECT_EQ(execute_status.code(), core::ErrorCode::InvalidArgument);
}

TEST(SqliteStorageTest, MoveSemanticsDoNotDoubleCloseOrFinalize) {
    auto connection = OpenMemoryDatabase();
    ASSERT_TRUE(connection.Execute("CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT)").ok());

    SqliteConnection moved_connection = std::move(connection);
    EXPECT_FALSE(connection);
    ASSERT_TRUE(moved_connection);

    auto statement_result = moved_connection.Prepare("INSERT INTO items(value) VALUES(?1)");
    ASSERT_TRUE(statement_result.ok()) << statement_result.status().message();
    auto statement = std::move(statement_result).value();

    decltype(statement) moved_statement = std::move(statement);
    EXPECT_FALSE(statement);
    ASSERT_TRUE(moved_statement);

    ASSERT_TRUE(moved_statement.BindText(1, "moved").ok());
    auto step = moved_statement.Step();
    ASSERT_TRUE(step.ok()) << step.status().message();
    EXPECT_EQ(std::move(step).value(), SqliteStepResult::Done);
    EXPECT_EQ(CountRows(moved_connection), 1);
}

TEST(SqliteStorageTest, BusyTimeoutRejectsNegativeValuesAndAcceptsNonNegativeValues) {
    auto connection = OpenMemoryDatabase();

    auto invalid = connection.SetBusyTimeoutMs(-1);
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.code(), core::ErrorCode::InvalidArgument);

    auto valid = connection.SetBusyTimeoutMs(250);
    EXPECT_TRUE(valid.ok()) << valid.message();
}

TEST(SqliteConnectionPoolTest, AcquiresAndReturnsReadWriteLeases) {
    const auto path = TestDatabasePath("agent_sqlite_pool_lease.db");
    InitializeFileDatabase(path);

    SqliteConnectionPool pool({
        path.string(),
        2,
        1,
        250,
        true,
        false});
    ASSERT_TRUE(pool.Start().ok());

    auto read_result = pool.AcquireRead();
    ASSERT_TRUE(read_result.ok()) << read_result.status().message();
    auto read = std::move(read_result).value();
    EXPECT_TRUE(read);
    EXPECT_EQ(read.kind(), storage::sqlite::SqliteConnectionKind::Read);

    auto write_result = pool.AcquireWrite();
    ASSERT_TRUE(write_result.ok()) << write_result.status().message();
    auto write = std::move(write_result).value();
    EXPECT_TRUE(write);
    EXPECT_EQ(write.kind(), storage::sqlite::SqliteConnectionKind::Write);

    auto stats = pool.Stats();
    EXPECT_EQ(stats.total_read_connections, 2u);
    EXPECT_EQ(stats.idle_read_connections, 1u);
    EXPECT_EQ(stats.leased_read_connections, 1u);
    EXPECT_EQ(stats.total_write_connections, 1u);
    EXPECT_EQ(stats.idle_write_connections, 0u);
    EXPECT_EQ(stats.leased_write_connections, 1u);

    read.Release();
    write.Release();

    stats = pool.Stats();
    EXPECT_EQ(stats.idle_read_connections, 2u);
    EXPECT_EQ(stats.leased_read_connections, 0u);
    EXPECT_EQ(stats.idle_write_connections, 1u);
    EXPECT_EQ(stats.leased_write_connections, 0u);
}

TEST(SqliteConnectionPoolTest, RejectsMoreThanOneWriteConnection) {
    const auto path = TestDatabasePath("agent_sqlite_pool_multiple_writers.db");

    SqliteConnectionPool pool({
        path.string(),
        2,
        2,
        250,
        true,
        false});
    auto status = pool.Start();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.code(), core::ErrorCode::InvalidArgument);
    EXPECT_NE(status.message().find("at most one write connection"), std::string::npos);
}

TEST(SqliteConnectionPoolTest, ReportsResourceExhaustedWhenPoolIsEmpty) {
    const auto path = TestDatabasePath("agent_sqlite_pool_exhausted.db");
    InitializeFileDatabase(path);

    SqliteConnectionPool pool({
        path.string(),
        1,
        1,
        250,
        true,
        false});
    ASSERT_TRUE(pool.Start().ok());

    auto first = pool.AcquireRead();
    ASSERT_TRUE(first.ok()) << first.status().message();

    auto second = pool.AcquireRead();
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.status().code(), core::ErrorCode::ResourceExhausted);
}

TEST(SqliteConnectionPoolTest, WaitAcquireBlocksUntilLeaseIsReturned) {
    const auto path = TestDatabasePath("agent_sqlite_pool_wait.db");
    InitializeFileDatabase(path);

    SqliteConnectionPool pool({
        path.string(),
        1,
        1,
        250,
        true,
        false});
    ASSERT_TRUE(pool.Start().ok());

    auto first_result = pool.AcquireRead();
    ASSERT_TRUE(first_result.ok()) << first_result.status().message();
    auto first = std::move(first_result).value();

    std::atomic<bool> acquired{false};
    auto waiter = std::async(std::launch::async, [&] {
        auto second_result = pool.WaitAcquireRead();
        if (!second_result.ok()) {
            return false;
        }
        auto second = std::move(second_result).value();
        acquired.store(true, std::memory_order_release);
        return second.valid();
    });

    EXPECT_EQ(waiter.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
    EXPECT_FALSE(acquired.load(std::memory_order_acquire));

    first.Release();
    EXPECT_TRUE(waiter.get());
    EXPECT_TRUE(acquired.load(std::memory_order_acquire));
}

TEST(SqliteConnectionPoolTest, ReportsWriteQueueWait) {
    const auto path = TestDatabasePath("agent_sqlite_pool_write_wait.db");
    InitializeFileDatabase(path);

    SqliteConnectionPool pool({
        path.string(),
        1,
        1,
        250,
        true,
        false});
    ASSERT_TRUE(pool.Start().ok());

    auto first_result = pool.AcquireWrite();
    ASSERT_TRUE(first_result.ok()) << first_result.status().message();
    auto first = std::move(first_result).value();

    auto waiter = std::async(std::launch::async, [&] {
        return pool.WaitAcquireWrite();
    });
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (pool.Stats().waiting_write_acquires == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(pool.Stats().waiting_write_acquires, 1u);

    first.Release();
    auto second_result = waiter.get();
    ASSERT_TRUE(second_result.ok()) << second_result.status().message();
    auto second = std::move(second_result).value();
    second.Release();

    const auto stats = pool.Stats();
    EXPECT_EQ(stats.waiting_write_acquires, 0u);
    EXPECT_EQ(stats.write_wait_count, 1u);
    EXPECT_GT(stats.total_write_wait_ns, 0u);
}

TEST(SqliteConnectionPoolTest, ImmediateAcquireDoesNotReportQueueWait) {
    const auto path = TestDatabasePath("agent_sqlite_pool_no_wait.db");
    InitializeFileDatabase(path);

    SqliteConnectionPool pool({
        path.string(),
        1,
        1,
        250,
        true,
        false});
    ASSERT_TRUE(pool.Start().ok());

    auto read_result = pool.WaitAcquireRead();
    ASSERT_TRUE(read_result.ok()) << read_result.status().message();
    auto read = std::move(read_result).value();
    read.Release();

    auto write_result = pool.WaitAcquireWrite();
    ASSERT_TRUE(write_result.ok()) << write_result.status().message();
    auto write = std::move(write_result).value();
    write.Release();

    const auto stats = pool.Stats();
    EXPECT_EQ(stats.read_wait_count, 0u);
    EXPECT_EQ(stats.write_wait_count, 0u);
    EXPECT_EQ(stats.total_read_wait_ns, 0u);
    EXPECT_EQ(stats.total_write_wait_ns, 0u);
}

TEST(SqliteConnectionPoolTest, TimedAcquireReturnsTimeoutWhenLeaseIsNotAvailable) {
    const auto path = TestDatabasePath("agent_sqlite_pool_timeout.db");
    InitializeFileDatabase(path);

    SqliteConnectionPool pool({
        path.string(),
        1,
        1,
        250,
        true,
        false});
    ASSERT_TRUE(pool.Start().ok());

    auto first_result = pool.AcquireRead();
    ASSERT_TRUE(first_result.ok()) << first_result.status().message();

    auto timeout_result = pool.WaitAcquireReadFor(std::chrono::milliseconds(20));
    ASSERT_FALSE(timeout_result.ok());
    EXPECT_EQ(timeout_result.status().code(), core::ErrorCode::Timeout);
}

TEST(SqliteConnectionPoolTest, CloseWakesWaitingAcquireWithCancelledStatus) {
    const auto path = TestDatabasePath("agent_sqlite_pool_close_wait.db");
    InitializeFileDatabase(path);

    SqliteConnectionPool pool({
        path.string(),
        1,
        1,
        250,
        true,
        false});
    ASSERT_TRUE(pool.Start().ok());

    auto first_result = pool.AcquireRead();
    ASSERT_TRUE(first_result.ok()) << first_result.status().message();

    auto waiter = std::async(std::launch::async, [&] {
        return pool.WaitAcquireRead();
    });

    EXPECT_EQ(waiter.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);

    pool.Close();

    auto result = waiter.get();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), core::ErrorCode::Cancelled);
}

TEST(SqliteAsyncExecutorTest, RunsAsyncWriteTransactionAndRead) {
    const auto path = TestDatabasePath("agent_sqlite_async_basic.db");
    InitializeFileDatabase(path);

    storage::sqlite::SqliteAsyncExecutorOptions executor_options;
    executor_options.pool = SqliteConnectionPoolOptions{
        path.string(),
        2,
        1,
        500,
        true,
        false};
    executor_options.worker_count = 2;
    executor_options.queue_capacity = 16;
    executor_options.acquire_timeout = std::chrono::milliseconds(5000);
    executor_options.thread_pool_name = "sqlite-async-test";

    storage::sqlite::SqliteAsyncExecutor executor(std::move(executor_options));
    ASSERT_TRUE(executor.Start().ok());

    auto write_future = executor.SubmitTransaction([](SqliteConnection& connection, SqliteTransaction&) {
        auto status = connection.Execute("INSERT INTO items(value) VALUES('async')");
        if (!status.ok()) {
            return -1;
        }
        return static_cast<int>(connection.LastInsertRowId());
    });

    auto write_result = write_future.get();
    ASSERT_TRUE(write_result.ok()) << write_result.status().message();
    EXPECT_EQ(std::move(write_result).value(), 1);

    auto read_future = executor.SubmitRead([](SqliteConnection& connection) {
        auto statement_result = connection.Prepare("SELECT COUNT(*) FROM items WHERE value = 'async'");
        if (!statement_result.ok()) {
            return -1;
        }
        auto statement = std::move(statement_result).value();
        auto step = statement.Step();
        if (!step.ok() || std::move(step).value() != SqliteStepResult::Row) {
            return -1;
        }
        return statement.ColumnInt(0);
    });

    auto read_result = read_future.get();
    ASSERT_TRUE(read_result.ok()) << read_result.status().message();
    EXPECT_EQ(std::move(read_result).value(), 1);
}

TEST(SqliteAsyncExecutorTest, HandlesMixedReadWriteStressWithCpuWork) {
    const auto path = TestDatabasePath("agent_sqlite_async_stress.db");
    InitializeFileDatabase(path);

    constexpr int kWrites = 96;
    constexpr int kReads = 192;

    storage::sqlite::SqliteAsyncExecutorOptions executor_options;
    executor_options.pool = SqliteConnectionPoolOptions{
        path.string(),
        3,
        1,
        2000,
        true,
        false};
    executor_options.worker_count = 8;
    executor_options.queue_capacity = 512;
    executor_options.acquire_timeout = std::chrono::milliseconds(5000);
    executor_options.thread_pool_name = "sqlite-async-stress-test";

    storage::sqlite::SqliteAsyncExecutor executor(std::move(executor_options));
    ASSERT_TRUE(executor.Start().ok());

    std::vector<std::future<core::Result<int>>> write_futures;
    write_futures.reserve(kWrites);
    for (int i = 0; i < kWrites; ++i) {
        write_futures.push_back(executor.SubmitTransaction([i](SqliteConnection& connection, SqliteTransaction&) {
            const auto cpu_value = SimulateCpuWork(i);
            auto statement_result = connection.Prepare("INSERT INTO items(value) VALUES(?1)");
            if (!statement_result.ok()) {
                return -1;
            }
            auto statement = std::move(statement_result).value();
            if (!statement.BindText(1, std::to_string(cpu_value)).ok()) {
                return -1;
            }
            auto step = statement.Step();
            if (!step.ok() || std::move(step).value() != SqliteStepResult::Done) {
                return -1;
            }
            return 1;
        }));
    }

    std::vector<std::future<core::Result<int>>> read_futures;
    read_futures.reserve(kReads);
    for (int i = 0; i < kReads; ++i) {
        read_futures.push_back(executor.SubmitRead([i](SqliteConnection& connection) {
            (void)SimulateCpuWork(i + 1000);
            auto statement_result = connection.Prepare("SELECT COUNT(*) FROM items");
            if (!statement_result.ok()) {
                return -1;
            }
            auto statement = std::move(statement_result).value();
            auto step = statement.Step();
            if (!step.ok() || std::move(step).value() != SqliteStepResult::Row) {
                return -1;
            }
            return statement.ColumnInt(0);
        }));
    }

    int completed_writes = 0;
    for (auto& future : write_futures) {
        auto result = future.get();
        ASSERT_TRUE(result.ok()) << result.status().message();
        completed_writes += std::move(result).value();
    }
    EXPECT_EQ(completed_writes, kWrites);

    for (auto& future : read_futures) {
        auto result = future.get();
        ASSERT_TRUE(result.ok()) << result.status().message();
        EXPECT_GE(std::move(result).value(), 0);
    }

    auto final_read = executor.SubmitRead([](SqliteConnection& connection) {
        auto statement_result = connection.Prepare("SELECT COUNT(*) FROM items");
        if (!statement_result.ok()) {
            return -1;
        }
        auto statement = std::move(statement_result).value();
        auto step = statement.Step();
        if (!step.ok() || std::move(step).value() != SqliteStepResult::Row) {
            return -1;
        }
        return statement.ColumnInt(0);
    });

    auto final_count = final_read.get();
    ASSERT_TRUE(final_count.ok()) << final_count.status().message();
    EXPECT_EQ(std::move(final_count).value(), kWrites);

    // future completion is published just before the worker releases its RAII lease.
    // Wait for that cleanup instead of racing the worker at the final pool snapshot.
    const auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto stats = executor.pool().Stats();
    while ((stats.idle_read_connections != stats.total_read_connections ||
            stats.idle_write_connections != stats.total_write_connections) &&
           std::chrono::steady_clock::now() < idle_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        stats = executor.pool().Stats();
    }
    EXPECT_EQ(stats.idle_read_connections, stats.total_read_connections);
    EXPECT_EQ(stats.idle_write_connections, stats.total_write_connections);
    EXPECT_EQ(stats.leased_read_connections, 0u);
    EXPECT_EQ(stats.leased_write_connections, 0u);
}

TEST(SqliteAsyncExecutorTest, TimesOutAcquireWhenPoolIsExhausted) {
    const auto path = TestDatabasePath("agent_sqlite_async_timeout.db");
    InitializeFileDatabase(path);

    storage::sqlite::SqliteAsyncExecutorOptions executor_options;
    executor_options.pool = SqliteConnectionPoolOptions{
        path.string(),
        1,
        1,
        2000,
        true,
        false};
    executor_options.worker_count = 2;
    executor_options.queue_capacity = 16;
    executor_options.acquire_timeout = std::chrono::milliseconds(20);
    executor_options.thread_pool_name = "sqlite-async-timeout-test";

    storage::sqlite::SqliteAsyncExecutor executor(std::move(executor_options));
    ASSERT_TRUE(executor.Start().ok());

    std::promise<void> blocker_ready;
    auto blocker_ready_future = blocker_ready.get_future();
    std::promise<void> blocker_release;
    auto blocker_release_future = blocker_release.get_future().share();

    auto blocker = executor.SubmitRead([&](SqliteConnection& connection) {
        blocker_ready.set_value();
        blocker_release_future.wait();
        auto statement_result = connection.Prepare("SELECT COUNT(*) FROM items");
        if (!statement_result.ok()) {
            return -1;
        }
        auto statement = std::move(statement_result).value();
        auto step = statement.Step();
        if (!step.ok() || std::move(step).value() != SqliteStepResult::Row) {
            return -1;
        }
        return statement.ColumnInt(0);
    });

    ASSERT_EQ(blocker_ready_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    auto timeout_task = executor.SubmitRead([](SqliteConnection& connection) {
        auto statement_result = connection.Prepare("SELECT COUNT(*) FROM items");
        if (!statement_result.ok()) {
            return -1;
        }
        auto statement = std::move(statement_result).value();
        auto step = statement.Step();
        if (!step.ok() || std::move(step).value() != SqliteStepResult::Row) {
            return -1;
        }
        return statement.ColumnInt(0);
    });

    auto timeout_result = timeout_task.get();
    ASSERT_FALSE(timeout_result.ok());
    EXPECT_EQ(timeout_result.status().code(), core::ErrorCode::Timeout);

    blocker_release.set_value();
    auto blocker_result = blocker.get();
    ASSERT_TRUE(blocker_result.ok()) << blocker_result.status().message();
}

} // namespace
