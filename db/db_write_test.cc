//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <atomic>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "db/db_test_util.h"
#include "db/write_batch_internal.h"
#include "db/write_thread.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "test_util/sync_point.h"
#include "util/random.h"
#include "util/string_util.h"
#include "utilities/fault_injection_env.h"

namespace ROCKSDB_NAMESPACE {

// Test variations of WriteImpl.
class DBWriteTest : public DBTestBase, public testing::WithParamInterface<int> {
 public:
  DBWriteTest() : DBTestBase("db_write_test", /*env_do_fsync=*/true) {}

  Options GetOptions() { return DBTestBase::GetOptions(GetParam()); }

  void Open() { DBTestBase::Reopen(GetOptions()); }
};

TEST_P(DBWriteTest, WriteEmptyBatch) {
  Options options = GetOptions();
  options.write_buffer_size = 65536;
  Reopen(options);
  WriteOptions write_options;
  WriteBatch batch;
  Random rnd(301);
  // Trigger a flush so that we will enter `WaitForPendingWrites`.
  for (auto i = 0; i < 10; i++) {
    batch.Clear();
    ASSERT_OK(dbfull()->Write(write_options, &batch));
    ASSERT_OK(batch.Put(std::to_string(i), rnd.RandomString(10240)));
    ASSERT_OK(dbfull()->Write(write_options, &batch));
  }
}
class DBWriteTestUnparameterized : public DBTestBase {
 public:
  explicit DBWriteTestUnparameterized()
      : DBTestBase("pipelined_write_test", /*env_do_fsync=*/false) {}
};

// It is invalid to do sync write while disabling WAL.
TEST_P(DBWriteTest, SyncAndDisableWAL) {
  Reopen(GetOptions());
  WriteOptions write_options;
  write_options.sync = true;
  write_options.disableWAL = true;
  ASSERT_TRUE(dbfull()->Put(write_options, "foo", "bar").IsInvalidArgument());
  WriteBatch batch;
  ASSERT_OK(batch.Put("foo", "bar"));
  ASSERT_TRUE(dbfull()->Write(write_options, &batch).IsInvalidArgument());
}

TEST_P(DBWriteTest, WriteStallRemoveNoSlowdownWrite) {
  Options options = GetOptions();
  options.level0_stop_writes_trigger = options.level0_slowdown_writes_trigger =
      4;
  std::vector<port::Thread> threads;
  std::atomic<int> thread_num(0);
  port::Mutex mutex;
  port::CondVar cv(&mutex);
  // Guarded by mutex
  int writers = 0;

  Reopen(options);

  std::function<void()> write_slowdown_func = [&]() {
    int a = thread_num.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    wo.no_slowdown = false;
    ASSERT_OK(dbfull()->Put(wo, key, "bar"));
  };
  std::function<void()> write_no_slowdown_func = [&]() {
    int a = thread_num.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    wo.no_slowdown = true;
    Status s = dbfull()->Put(wo, key, "bar");
    ASSERT_TRUE(s.ok() || s.IsIncomplete());
  };
  std::function<void(void*)> unblock_main_thread_func = [&](void*) {
    mutex.Lock();
    ++writers;
    cv.SignalAll();
    mutex.Unlock();
  };

  // Create 3 L0 files and schedule 4th without waiting
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::JoinBatchGroup:Start", unblock_main_thread_func);
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->LoadDependency(
      {{"DBWriteTest::WriteStallRemoveNoSlowdownWrite:1",
        "DBImpl::BackgroundCallFlush:start"},
       {"DBWriteTest::WriteStallRemoveNoSlowdownWrite:2",
        "DBImplWrite::PipelinedWriteImpl:AfterJoinBatchGroup"},
       // Make compaction start wait for the write stall to be detected and
       // implemented by a write group leader
       {"DBWriteTest::WriteStallRemoveNoSlowdownWrite:3",
        "BackgroundCallCompaction:0"}});
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // Schedule creation of 4th L0 file without waiting. This will seal the
  // memtable and then wait for a sync point before writing the file. We need
  // to do it this way because SwitchMemtable() needs to enter the
  // write_thread
  FlushOptions fopt;
  fopt.wait = false;
  ASSERT_OK(dbfull()->Flush(fopt));

  // Create a mix of slowdown/no_slowdown write threads
  mutex.Lock();
  // First leader
  threads.emplace_back(write_slowdown_func);
  while (writers != 1) {
    cv.Wait();
  }

  // Second leader. Will stall writes
  // Build a writers list with no slowdown in the middle:
  //  +-------------+
  //  | slowdown    +<----+ newest
  //  +--+----------+
  //     |
  //     v
  //  +--+----------+
  //  | no slowdown |
  //  +--+----------+
  //     |
  //     v
  //  +--+----------+
  //  | slowdown    +
  //  +-------------+
  threads.emplace_back(write_slowdown_func);
  while (writers != 2) {
    cv.Wait();
  }
  threads.emplace_back(write_no_slowdown_func);
  while (writers != 3) {
    cv.Wait();
  }
  threads.emplace_back(write_slowdown_func);
  while (writers != 4) {
    cv.Wait();
  }

  mutex.Unlock();

  TEST_SYNC_POINT("DBWriteTest::WriteStallRemoveNoSlowdownWrite:1");
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable(nullptr));
  // This would have triggered a write stall. Unblock the write group leader
  TEST_SYNC_POINT("DBWriteTest::WriteStallRemoveNoSlowdownWrite:2");
  // The leader is going to create missing newer links. When the leader
  // finishes, the next leader is going to delay writes and fail writers with
  // no_slowdown

  TEST_SYNC_POINT("DBWriteTest::WriteStallRemoveNoSlowdownWrite:3");
  for (auto& t : threads) {
    t.join();
  }

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();
}

TEST_P(DBWriteTest, WriteThreadHangOnWriteStall) {
  Options options = GetOptions();
  options.level0_stop_writes_trigger = options.level0_slowdown_writes_trigger = 4;
  std::vector<port::Thread> threads;
  std::atomic<int> thread_num(0);
  port::Mutex mutex;
  port::CondVar cv(&mutex);
  // Guarded by mutex
  int writers = 0;

  Reopen(options);

  std::function<void()> write_slowdown_func = [&]() {
    int a = thread_num.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    wo.no_slowdown = false;
    ASSERT_OK(dbfull()->Put(wo, key, "bar"));
  };
  std::function<void()> write_no_slowdown_func = [&]() {
    int a = thread_num.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    wo.no_slowdown = true;
    Status s = dbfull()->Put(wo, key, "bar");
    ASSERT_TRUE(s.ok() || s.IsIncomplete());
  };
  std::function<void(void *)> unblock_main_thread_func = [&](void *) {
    mutex.Lock();
    ++writers;
    cv.SignalAll();
    mutex.Unlock();
  };

  // Create 3 L0 files and schedule 4th without waiting
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::JoinBatchGroup:Start", unblock_main_thread_func);
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->LoadDependency(
      {{"DBWriteTest::WriteThreadHangOnWriteStall:1",
        "DBImpl::BackgroundCallFlush:start"},
       {"DBWriteTest::WriteThreadHangOnWriteStall:2",
        "DBImpl::WriteImpl:BeforeLeaderEnters"},
       // Make compaction start wait for the write stall to be detected and
       // implemented by a write group leader
       {"DBWriteTest::WriteThreadHangOnWriteStall:3",
        "BackgroundCallCompaction:0"}});
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // Schedule creation of 4th L0 file without waiting. This will seal the
  // memtable and then wait for a sync point before writing the file. We need
  // to do it this way because SwitchMemtable() needs to enter the
  // write_thread
  FlushOptions fopt;
  fopt.wait = false;
  ASSERT_OK(dbfull()->Flush(fopt));

  // Create a mix of slowdown/no_slowdown write threads
  mutex.Lock();
  // First leader
  threads.emplace_back(write_slowdown_func);
  while (writers != 1) {
    cv.Wait();
  }
  // Second leader. Will stall writes
  threads.emplace_back(write_slowdown_func);
  threads.emplace_back(write_no_slowdown_func);
  threads.emplace_back(write_slowdown_func);
  threads.emplace_back(write_no_slowdown_func);
  threads.emplace_back(write_slowdown_func);
  while (writers != 6) {
    cv.Wait();
  }
  mutex.Unlock();

  TEST_SYNC_POINT("DBWriteTest::WriteThreadHangOnWriteStall:1");
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable(nullptr));
  // This would have triggered a write stall. Unblock the write group leader
  TEST_SYNC_POINT("DBWriteTest::WriteThreadHangOnWriteStall:2");
  // The leader is going to create missing newer links. When the leader finishes,
  // the next leader is going to delay writes and fail writers with no_slowdown

  TEST_SYNC_POINT("DBWriteTest::WriteThreadHangOnWriteStall:3");
  for (auto& t : threads) {
    t.join();
  }
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();
}

TEST_P(DBWriteTest, IOErrorOnWALWritePropagateToWriteThreadFollower) {
  constexpr int kNumThreads = 5;
  std::unique_ptr<FaultInjectionTestEnv> mock_env(
      new FaultInjectionTestEnv(env_));
  Options options = GetOptions();
  options.env = mock_env.get();
  Reopen(options);
  std::atomic<int> ready_count{0};
  std::atomic<int> leader_count{0};
  std::vector<port::Thread> threads;
  mock_env->SetFilesystemActive(false);

  // Wait until all threads linked to write threads, to make sure
  // all threads join the same batch group.
  SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::JoinBatchGroup:Wait", [&](void* arg) {
        ready_count++;
        auto* w = reinterpret_cast<WriteThread::Writer*>(arg);
        if (w->state == WriteThread::STATE_GROUP_LEADER) {
          leader_count++;
          while (ready_count < kNumThreads) {
            // busy waiting
          }
        }
      });
  SyncPoint::GetInstance()->EnableProcessing();
  for (int i = 0; i < kNumThreads; i++) {
    threads.push_back(port::Thread(
        [&](int index) {
          // All threads should fail.
          auto res = Put("key" + std::to_string(index), "value");
          if (options.manual_wal_flush) {
            ASSERT_TRUE(res.ok());
            // we should see fs error when we do the flush

            // TSAN reports a false alarm for lock-order-inversion but Open and
            // FlushWAL are not run concurrently. Disabling this until TSAN is
            // fixed.
            // res = dbfull()->FlushWAL(false);
            // ASSERT_FALSE(res.ok());
          } else {
            ASSERT_FALSE(res.ok());
          }
        },
        i));
  }
  for (int i = 0; i < kNumThreads; i++) {
    threads[i].join();
  }
  ASSERT_EQ(1, leader_count);

  // The Failed PUT operations can cause a BG error to be set.
  // Mark it as Checked for the ASSERT_STATUS_CHECKED
  dbfull()->Resume().PermitUncheckedError();

  // Close before mock_env destruct.
  Close();
}

TEST_F(DBWriteTestUnparameterized, PipelinedWriteRace) {
  // This test was written to trigger a race in ExitAsBatchGroupLeader in case
  // enable_pipelined_write_ was true.
  // Writers for which ShouldWriteToMemtable() evaluates to false are removed
  // from the write_group via CompleteFollower/ CompleteLeader. Writers in the
  // middle of the group are fully unlinked, but if that writers is the
  // last_writer, then we did not update the predecessor's link_older, i.e.,
  // this writer was still reachable via newest_writer_.
  //
  // But the problem was, that CompleteFollower already wakes up the thread
  // owning that writer before the writer has been removed. This resulted in a
  // race - if the leader thread was fast enough, then everything was fine.
  // However, if the woken up thread finished the current write operation and
  // then performed yet another write, then a new writer instance was added
  // to newest_writer_. It is possible that the new writer is located on the
  // same address on stack, and if this happened, then we had a problem,
  // because the old code tried to find the last_writer in the list to unlink
  // it, which in this case produced a cycle in the list.
  // Whether two invocations of PipelinedWriteImpl() by the same thread actually
  // allocate the writer on the same address depends on the OS and/or compiler,
  // so it is rather hard to create a deterministic test for this.

  Options options = GetDefaultOptions();
  options.create_if_missing = true;
  options.enable_pipelined_write = true;
  std::vector<port::Thread> threads;

  std::atomic<int> write_counter{0};
  std::atomic<int> active_writers{0};
  std::atomic<bool> second_write_starting{false};
  std::atomic<bool> second_write_in_progress{false};
  std::atomic<WriteThread::Writer*> leader{nullptr};
  std::atomic<bool> finished_WAL_write{false};

  DestroyAndReopen(options);

  auto write_one_doc = [&]() {
    int a = write_counter.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    ASSERT_OK(dbfull()->Put(wo, key, "bar"));
    --active_writers;
  };

  auto write_two_docs = [&]() {
    write_one_doc();
    second_write_starting = true;
    write_one_doc();
  };

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::JoinBatchGroup:Wait", [&](void* arg) {
        if (second_write_starting.load()) {
          second_write_in_progress = true;
          return;
        }
        auto* w = reinterpret_cast<WriteThread::Writer*>(arg);
        if (w->state == WriteThread::STATE_GROUP_LEADER) {
          active_writers++;
          if (leader.load() == nullptr) {
            leader.store(w);
            while (active_writers.load() < 2) {
              // wait for another thread to join the write_group
            }
          }
        } else {
          // we disable the memtable for all followers so that they they are
          // removed from the write_group before enqueuing it for the memtable
          // write
          w->disable_memtable = true;
          active_writers++;
        }
      });

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::ExitAsBatchGroupLeader:Start", [&](void* arg) {
        auto* wg = reinterpret_cast<WriteThread::WriteGroup*>(arg);
        if (wg->leader == leader && !finished_WAL_write) {
          finished_WAL_write = true;
          while (active_writers.load() < 3) {
            // wait for the new writer to be enqueued
          }
        }
      });

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::ExitAsBatchGroupLeader:AfterCompleteWriters",
      [&](void* arg) {
        auto* wg = reinterpret_cast<WriteThread::WriteGroup*>(arg);
        if (wg->leader == leader) {
          while (!second_write_in_progress.load()) {
            // wait for the old follower thread to start the next write
          }
        }
      });

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // start leader + one follower
  threads.emplace_back(write_one_doc);
  while (leader.load() == nullptr) {
    // wait for leader
  }

  // we perform two writes in the follower, so that for the second write
  // the thread reinserts a Writer with the same address
  threads.emplace_back(write_two_docs);

  // wait for the leader to enter ExitAsBatchGroupLeader
  while (!finished_WAL_write.load()) {
    // wait for write_group to have finished the WAL writes
  }

  // start another writer thread to be enqueued before the leader can
  // complete the writers from its write_group
  threads.emplace_back(write_one_doc);

  for (auto& t : threads) {
    t.join();
  }
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();
}

TEST_P(DBWriteTest, ManualWalFlushInEffect) {
  Options options = GetOptions();
  Reopen(options);
  // try the 1st WAL created during open
  ASSERT_TRUE(Put("key" + std::to_string(0), "value").ok());
  ASSERT_TRUE(options.manual_wal_flush != dbfull()->WALBufferIsEmpty());
  ASSERT_TRUE(dbfull()->FlushWAL(false).ok());
  ASSERT_TRUE(dbfull()->WALBufferIsEmpty());
  // try the 2nd wal created during SwitchWAL
  ASSERT_OK(dbfull()->TEST_SwitchWAL());
  ASSERT_TRUE(Put("key" + std::to_string(0), "value").ok());
  ASSERT_TRUE(options.manual_wal_flush != dbfull()->WALBufferIsEmpty());
  ASSERT_TRUE(dbfull()->FlushWAL(false).ok());
  ASSERT_TRUE(dbfull()->WALBufferIsEmpty());
}

TEST_P(DBWriteTest, UnflushedPutRaceWithTrackedWalSync) {
  // Repro race condition bug where unflushed WAL data extended the synced size
  // recorded to MANIFEST despite being unrecoverable.
  Options options = GetOptions();
  std::unique_ptr<FaultInjectionTestEnv> fault_env(
      new FaultInjectionTestEnv(env_));
  options.env = fault_env.get();
  options.manual_wal_flush = true;
  options.track_and_verify_wals_in_manifest = true;
  Reopen(options);

  ASSERT_OK(Put("key1", "val1"));

  SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::SyncWAL:Begin",
      [this](void* /* arg */) { ASSERT_OK(Put("key2", "val2")); });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  ASSERT_OK(db_->FlushWAL(true /* sync */));

  // Ensure callback ran.
  ASSERT_EQ("val2", Get("key2"));

  Close();

  // Simulate full loss of unsynced data. This drops "key2" -> "val2" from the
  // DB WAL.
  fault_env->DropUnsyncedFileData();

  Reopen(options);

  // Need to close before `fault_env` goes out of scope.
  Close();
}

TEST_P(DBWriteTest, InactiveWalFullySyncedBeforeUntracked) {
  // Repro bug where a WAL is appended and switched after
  // `FlushWAL(true /* sync */)`'s sync finishes and before it untracks fully
  // synced inactive logs. Previously such a WAL would be wrongly untracked
  // so the final append would never be synced.
  Options options = GetOptions();
  std::unique_ptr<FaultInjectionTestEnv> fault_env(
      new FaultInjectionTestEnv(env_));
  options.env = fault_env.get();
  Reopen(options);

  ASSERT_OK(Put("key1", "val1"));

  SyncPoint::GetInstance()->SetCallBack(
      "DBImpl::SyncWAL:BeforeMarkLogsSynced:1", [this](void* /* arg */) {
        ASSERT_OK(Put("key2", "val2"));
        ASSERT_OK(dbfull()->TEST_SwitchMemtable());
      });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  ASSERT_OK(db_->FlushWAL(true /* sync */));

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();

  ASSERT_OK(Put("key3", "val3"));

  ASSERT_OK(db_->FlushWAL(true /* sync */));

  Close();

  // Simulate full loss of unsynced data. This should drop nothing since we did
  // `FlushWAL(true /* sync */)` before `Close()`.
  fault_env->DropUnsyncedFileData();

  Reopen(options);

  ASSERT_EQ("val1", Get("key1"));
  ASSERT_EQ("val2", Get("key2"));
  ASSERT_EQ("val3", Get("key3"));

  // Need to close before `fault_env` goes out of scope.
  Close();
}

TEST_P(DBWriteTest, IOErrorOnWALWriteTriggersReadOnlyMode) {
  std::unique_ptr<FaultInjectionTestEnv> mock_env(
      new FaultInjectionTestEnv(env_));
  Options options = GetOptions();
  options.env = mock_env.get();
  Reopen(options);
  for (int i = 0; i < 2; i++) {
    // Forcibly fail WAL write for the first Put only. Subsequent Puts should
    // fail due to read-only mode
    mock_env->SetFilesystemActive(i != 0);
    auto res = Put("key" + std::to_string(i), "value");
    // TSAN reports a false alarm for lock-order-inversion but Open and
    // FlushWAL are not run concurrently. Disabling this until TSAN is
    // fixed.
    /*
    if (options.manual_wal_flush && i == 0) {
      // even with manual_wal_flush the 2nd Put should return error because of
      // the read-only mode
      ASSERT_TRUE(res.ok());
      // we should see fs error when we do the flush
      res = dbfull()->FlushWAL(false);
    }
    */
    if (!options.manual_wal_flush) {
      ASSERT_NOK(res);
    } else {
      ASSERT_OK(res);
    }
  }
  // Close before mock_env destruct.
  Close();
}

TEST_P(DBWriteTest, IOErrorOnSwitchMemtable) {
  Random rnd(301);
  std::unique_ptr<FaultInjectionTestEnv> mock_env(
      new FaultInjectionTestEnv(env_));
  Options options = GetOptions();
  options.env = mock_env.get();
  options.writable_file_max_buffer_size = 4 * 1024 * 1024;
  options.write_buffer_size = 3 * 512 * 1024;
  options.wal_bytes_per_sync = 256 * 1024;
  options.manual_wal_flush = true;
  Reopen(options);
  mock_env->SetFilesystemActive(false, Status::IOError("Not active"));
  Status s;
  for (int i = 0; i < 4 * 512; ++i) {
    s = Put(Key(i), rnd.RandomString(1024));
    if (!s.ok()) {
      break;
    }
  }
  ASSERT_EQ(s.severity(), Status::Severity::kFatalError);

  mock_env->SetFilesystemActive(true);
  // Close before mock_env destruct.
  Close();
}

// Test that db->LockWAL() flushes the WAL after locking.
TEST_P(DBWriteTest, LockWalInEffect) {
  Options options = GetOptions();
  Reopen(options);
  // try the 1st WAL created during open
  ASSERT_OK(Put("key" + std::to_string(0), "value"));
  ASSERT_TRUE(options.manual_wal_flush != dbfull()->WALBufferIsEmpty());
  ASSERT_OK(dbfull()->LockWAL());
  ASSERT_TRUE(dbfull()->WALBufferIsEmpty(false));
  ASSERT_OK(dbfull()->UnlockWAL());
  // try the 2nd wal created during SwitchWAL
  ASSERT_OK(dbfull()->TEST_SwitchWAL());
  ASSERT_OK(Put("key" + std::to_string(0), "value"));
  ASSERT_TRUE(options.manual_wal_flush != dbfull()->WALBufferIsEmpty());
  ASSERT_OK(dbfull()->LockWAL());
  ASSERT_TRUE(dbfull()->WALBufferIsEmpty(false));
  ASSERT_OK(dbfull()->UnlockWAL());
}

TEST_P(DBWriteTest, ConcurrentlyDisabledWAL) {
    Options options = GetOptions();
    options.statistics = ROCKSDB_NAMESPACE::CreateDBStatistics();
    options.statistics->set_stats_level(StatsLevel::kAll);
    Reopen(options);
    std::string wal_key_prefix = "WAL_KEY_";
    std::string no_wal_key_prefix = "K_";
    // 100 KB value each for NO-WAL operation
    std::string no_wal_value(1024 * 100, 'X');
    // 1B value each for WAL operation
    std::string wal_value = "0";
    std::thread threads[10];
    for (int t = 0; t < 10; t++) {
        threads[t] = std::thread([t, wal_key_prefix, wal_value, no_wal_key_prefix, no_wal_value, this] {
            for(int i = 0; i < 10; i++) {
              ROCKSDB_NAMESPACE::WriteOptions write_option_disable;
              write_option_disable.disableWAL = true;
              ROCKSDB_NAMESPACE::WriteOptions write_option_default;
              std::string no_wal_key = no_wal_key_prefix + std::to_string(t) +
                                       "_" + std::to_string(i);
              ASSERT_OK(
                  this->Put(no_wal_key, no_wal_value, write_option_disable));
              std::string wal_key =
                  wal_key_prefix + std::to_string(i) + "_" + std::to_string(i);
              ASSERT_OK(this->Put(wal_key, wal_value, write_option_default));
              ASSERT_OK(dbfull()->SyncWAL());
            }
            return;
        });
    }
    for (auto& t: threads) {
        t.join();
    }
    uint64_t bytes_num = options.statistics->getTickerCount(
        ROCKSDB_NAMESPACE::Tickers::WAL_FILE_BYTES);
    // written WAL size should less than 100KB (even included HEADER & FOOTER overhead)
    ASSERT_LE(bytes_num, 1024 * 100);
}

TEST_P(DBWriteTest, MultiThreadWrite) {
  Options options = GetOptions();
  std::unique_ptr<FaultInjectionTestEnv> mock_env(
      new FaultInjectionTestEnv(env_));
  if (!options.enable_multi_batch_write) {
    return;
  }
  constexpr int kNumThreads = 4;
  constexpr int kNumWrite = 4;
  constexpr int kNumBatch = 8;
  constexpr int kBatchSize = 16;
  options.env = mock_env.get();
  options.write_buffer_size = 1024 * 128;
  Reopen(options);
  std::vector<port::Thread> threads;
  for (int t = 0; t < kNumThreads; t++) {
    threads.push_back(port::Thread(
        [&](int index) {
          WriteOptions opt;
          std::vector<WriteBatch> data(kNumBatch);
          for (int j = 0; j < kNumWrite; j++) {
            std::vector<WriteBatch*> batches;
            for (int i = 0; i < kNumBatch; i++) {
              WriteBatch* batch = &data[i];
              batch->Clear();
              for (int k = 0; k < kBatchSize; k++) {
                batch->Put("key_" + ToString(index) + "_" + ToString(j) + "_" +
                               ToString(i) + "_" + ToString(k),
                           "value" + ToString(k));
              }
              batches.push_back(batch);
            }
            dbfull()->MultiBatchWrite(opt, std::move(batches));
          }
        },
        t));
  }
  for (int i = 0; i < kNumThreads; i++) {
    threads[i].join();
  }
  ReadOptions opt;
  for (int t = 0; t < kNumThreads; t++) {
    std::string value;
    for (int i = 0; i < kNumWrite; i++) {
      for (int j = 0; j < kNumBatch; j++) {
        for (int k = 0; k < kBatchSize; k++) {
          ASSERT_OK(dbfull()->Get(opt,
                                  "key_" + ToString(t) + "_" + ToString(i) +
                                      "_" + ToString(j) + "_" + ToString(k),
                                  &value));
          std::string expected_value = "value" + ToString(k);
          ASSERT_EQ(expected_value, value);
        }
      }
    }
  }

  Close();
}

class SimpleCallback : public PostWriteCallback {
  std::function<void(SequenceNumber)> f_;

 public:
  SimpleCallback(std::function<void(SequenceNumber)>&& f) : f_(f) {}

  void Callback(SequenceNumber seq) override { f_(seq); }
};

TEST_P(DBWriteTest, PostWriteCallback) {
  Options options = GetOptions();
  if (options.two_write_queues) {
    // Not compatible.
    return;
  }
  Reopen(options);

  std::vector<port::Thread> threads;

  port::Mutex the_first_can_exit_write_mutex;
  the_first_can_exit_write_mutex.Lock();
  port::Mutex can_flush_mutex;
  can_flush_mutex.Lock();
  port::Mutex the_second_can_exit_write_mutex;
  the_second_can_exit_write_mutex.Lock();

  std::atomic<uint64_t> written(0);
  std::atomic<bool> flushed(false);

  threads.push_back(port::Thread([&] {
    WriteBatch batch;
    WriteOptions opts;
    opts.sync = false;
    opts.disableWAL = true;
    SimpleCallback callback([&](SequenceNumber seq) {
      ASSERT_NE(seq, 0);
      can_flush_mutex.Unlock();
      the_first_can_exit_write_mutex.Lock();
      the_second_can_exit_write_mutex.Unlock();
    });
    batch.Put("key", "value");
    ASSERT_OK(dbfull()->Write(opts, &batch, &callback));
    written.fetch_add(1, std::memory_order_relaxed);
  }));
  threads.push_back(port::Thread([&] {
    WriteBatch batch;
    WriteOptions opts;
    opts.sync = false;
    opts.disableWAL = true;
    SimpleCallback callback([&](SequenceNumber seq) {
      ASSERT_NE(seq, 0);
      the_second_can_exit_write_mutex.Lock();
    });
    batch.Put("key", "value");
    ASSERT_OK(dbfull()->Write(opts, &batch, &callback));
    written.fetch_add(1, std::memory_order_relaxed);
  }));
  // Flush will enter write thread and wait for pending writes.
  threads.push_back(port::Thread([&] {
    FlushOptions opts;
    opts.wait = false;
    can_flush_mutex.Lock();
    ASSERT_OK(dbfull()->Flush(opts));
    flushed.store(true, std::memory_order_relaxed);
  }));

  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  ASSERT_EQ(written.load(std::memory_order_relaxed), 0);
  ASSERT_EQ(flushed.load(std::memory_order_relaxed), false);

  the_first_can_exit_write_mutex.Unlock();
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  ASSERT_EQ(written.load(std::memory_order_relaxed), 2);
  ASSERT_EQ(flushed.load(std::memory_order_relaxed), true);

  for (auto& t : threads) {
    t.join();
  }
}

TEST_P(DBWriteTest, PostWriteCallbackEmptyBatch) {
  Options options = GetOptions();
  if (options.two_write_queues) {
    // Not compatible.
    return;
  }
  Reopen(options);
  WriteBatch batch;
  WriteOptions opts;
  opts.sync = false;
  opts.disableWAL = true;
  SequenceNumber seq = 0;
  SimpleCallback callback([&](SequenceNumber s) { seq = s; });
  ASSERT_OK(dbfull()->Write(opts, &batch, &callback));
  ASSERT_NE(seq, 0);
}

INSTANTIATE_TEST_CASE_P(DBWriteTestInstance, DBWriteTest,
                        testing::Values(DBTestBase::kDefault,
                                        DBTestBase::kConcurrentWALWrites,
                                        DBTestBase::kPipelinedWrite,
                                        DBTestBase::kMultiBatchWrite));

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  RegisterCustomObjects(argc, argv);
  return RUN_ALL_TESTS();
}
