#include <Databases/DatabaseReplicatedWorker.h>

#include <filesystem>
#include <Core/ServerUUID.h>
#include <Core/Settings.h>
#include <Databases/DatabaseReplicated.h>
#include <Interpreters/Context.h>
#include <Interpreters/DDLTask.h>
#include <Interpreters/DatabaseCatalog.h>
#include <base/sleep.h>
#include <Common/FailPoint.h>
#include <Common/OpenTelemetryTraceContext.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/thread_local_rng.h>

namespace fs = std::filesystem;

namespace DB
{
namespace Setting
{
    extern const SettingsUInt64 database_replicated_initial_query_timeout_sec;
}

namespace DatabaseReplicatedSetting
{
    extern const DatabaseReplicatedSettingsBool check_consistency;
    extern const DatabaseReplicatedSettingsUInt64 max_replication_lag_to_enqueue;
    extern const DatabaseReplicatedSettingsUInt64 max_retries_before_automatic_recovery;
    extern const DatabaseReplicatedSettingsUInt64 wait_entry_commited_timeout_sec;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int DATABASE_REPLICATION_FAILED;
    extern const int NOT_A_LEADER;
    extern const int QUERY_WAS_CANCELLED;
    extern const int TABLE_IS_DROPPED;
    extern const int UNFINISHED;
}

namespace FailPoints
{
    extern const char database_replicated_delay_recovery[];
    extern const char database_replicated_delay_entry_execution[];
}

static constexpr const char * FORCE_AUTO_RECOVERY_DIGEST = "42";

DatabaseReplicatedDDLWorker::DatabaseReplicatedDDLWorker(DatabaseReplicated * db, ContextPtr context_)
    : DDLWorker(
          /* pool_size */ 1,
          db->zookeeper_path + "/log",
          db->zookeeper_path + "/replicas",
          context_,
          nullptr,
          {},
          fmt::format("DDLWorker({})", db->getDatabaseName()))
    , database(db)
{
    /// Pool size must be 1 to avoid reordering of log entries.
    /// TODO Make a dependency graph of DDL queries. It will allow to execute independent entries in parallel.
    /// We also need similar graph to load tables on server startup in order of topsort.
}

bool DatabaseReplicatedDDLWorker::initializeMainThread()
{
    {
        std::lock_guard lock(initialization_duration_timer_mutex);
        initialization_duration_timer.emplace();
        initialization_duration_timer->start();
    }

    while (!stop_flag)
    {
        try
        {
            chassert(!database->is_probably_dropped);
            auto zookeeper = getAndSetZooKeeper();
            if (database->is_readonly)
                database->tryConnectToZooKeeperAndInitDatabase(LoadingStrictnessLevel::ATTACH);
            if (database->is_probably_dropped)
            {
                /// The flag was set in tryConnectToZooKeeperAndInitDatabase
                LOG_WARNING(log, "Exiting main thread, because the database was probably dropped");
                /// NOTE It will not stop cleanup thread until DDLWorker::shutdown() call (cleanup thread will just do nothing)
                break;
            }

            if (database->db_settings[DatabaseReplicatedSetting::max_retries_before_automatic_recovery]
                && database->db_settings[DatabaseReplicatedSetting::max_retries_before_automatic_recovery] <= subsequent_errors_count)
            {
                String current_task_name;
                {
                    std::unique_lock lock{mutex};
                    current_task_name = current_task;
                }
                LOG_WARNING(log, "Database got stuck at processing task {}: it failed {} times in a row with the same error. "
                                 "Will reset digest to mark our replica as lost, and trigger recovery from the most up-to-date metadata "
                                 "from ZooKeeper. See max_retries_before_automatic_recovery setting. The error: {}",
                            current_task, subsequent_errors_count.load(), last_unexpected_error);

                String digest_str;
                zookeeper->tryGet(database->replica_path + "/digest", digest_str);
                LOG_WARNING(log, "Resetting digest from {} to {}", digest_str, FORCE_AUTO_RECOVERY_DIGEST);
                zookeeper->trySet(database->replica_path + "/digest", FORCE_AUTO_RECOVERY_DIGEST);
            }

            initializeReplication();
            initialized = true;
            {
                std::lock_guard lock(initialization_duration_timer_mutex);
                initialization_duration_timer.reset();
            }
            return true;
        }
        catch (...)
        {
            tryLogCurrentException(log, fmt::format("Error on initialization of {}", database->getDatabaseName()));
            sleepForSeconds(5);
        }
    }

    {
        std::lock_guard lock(initialization_duration_timer_mutex);
        initialization_duration_timer.reset();
    }

    return false;
}

void DatabaseReplicatedDDLWorker::shutdown()
{
    DDLWorker::shutdown();
    wait_current_task_change.notify_all();
}

void DatabaseReplicatedDDLWorker::initializeReplication()
{
    /// Check if we need to recover replica.
    /// Invariant: replica is lost if it's log_ptr value is less then max_log_ptr - logs_to_keep.

    auto zookeeper = getAndSetZooKeeper();

    /// Create "active" node (remove previous one if necessary)
    String active_path = fs::path(database->replica_path) / "active";
    String active_id = toString(ServerUUID::get());
    zookeeper->deleteEphemeralNodeIfContentMatches(active_path, [&active_id] (const std::string & actual_content)
    {
        if (actual_content.ends_with(DatabaseReplicated::REPLICA_UNSYNCED_MARKER))
            return active_id == actual_content.substr(0, actual_content.size() - strlen(DatabaseReplicated::REPLICA_UNSYNCED_MARKER));
        return active_id == actual_content;
    });
    bool first_initialization = active_node_holder == nullptr;
    if (active_node_holder)
        active_node_holder->setAlreadyRemoved();
    active_node_holder.reset();

    String log_ptr_str = zookeeper->get(database->replica_path + "/log_ptr");
    UInt32 our_log_ptr = parse<UInt32>(log_ptr_str);
    UInt32 max_log_ptr = parse<UInt32>(zookeeper->get(database->zookeeper_path + "/max_log_ptr"));
    logs_to_keep = parse<UInt32>(zookeeper->get(database->zookeeper_path + "/logs_to_keep"));

    UInt64 digest;
    String digest_str;
    UInt64 local_digest;
    if (zookeeper->tryGet(database->replica_path + "/digest", digest_str))
    {
        digest = parse<UInt64>(digest_str);
        std::lock_guard lock{database->metadata_mutex};
        local_digest = database->tables_metadata_digest;
    }
    else
    {
        LOG_WARNING(log, "Did not find digest in ZooKeeper, creating it");
        /// Database was created by old ClickHouse versions, let's create the node
        std::lock_guard lock{database->metadata_mutex};
        digest = local_digest = database->tables_metadata_digest;
        digest_str = toString(digest);
        zookeeper->create(database->replica_path + "/digest", digest_str, zkutil::CreateMode::Persistent);
    }

    LOG_TRACE(log, "Trying to initialize replication: our_log_ptr={}, max_log_ptr={}, local_digest={}, zk_digest={}",
              our_log_ptr, max_log_ptr, local_digest, digest);

    bool is_new_replica = our_log_ptr == 0;
    bool lost_according_to_log_ptr = our_log_ptr + logs_to_keep < max_log_ptr;
    bool lost_according_to_digest = database->db_settings[DatabaseReplicatedSetting::check_consistency] && local_digest != digest;

    if (is_new_replica || lost_according_to_log_ptr || lost_according_to_digest)
    {
        if (!is_new_replica)
            LOG_WARNING(log, "Replica seems to be lost: our_log_ptr={}, max_log_ptr={}, local_digest={}, zk_digest={}",
                        our_log_ptr, max_log_ptr, local_digest, digest);

        database->recoverLostReplica(zookeeper, our_log_ptr, max_log_ptr);

        fiu_do_on(FailPoints::database_replicated_delay_recovery,
        {
            std::chrono::milliseconds sleep_time{3000 + thread_local_rng() % 2000};
            std::this_thread::sleep_for(sleep_time);
        });

        zookeeper->set(database->replica_path + "/log_ptr", toString(max_log_ptr));
        initializeLogPointer(DDLTaskBase::getLogEntryName(max_log_ptr));
    }
    else
    {
        String log_entry_name = DDLTaskBase::getLogEntryName(our_log_ptr);
        last_skipped_entry_name.emplace(log_entry_name);
        initializeLogPointer(log_entry_name);
    }

    {
        std::lock_guard lock{database->metadata_mutex};
        if (!database->checkDigestValid(context))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Inconsistent database metadata after reconnection to ZooKeeper");
    }

    if (is_new_replica || first_initialization)
    {
        /// The current max_log_ptr might increase significantly while we were executing recoverLostReplica.
        /// If it exceeds max_replication_lag_to_enqueue - this replica will refuse to accept other queries.
        /// Also, if we just mark this replica active - other replicas will wait for it when executing queries with
        /// distributed_ddl_output_mode = '*_only_active', although it may be still busy with previous queries.
        /// Provide a way to identify such replicas to avoid waiting for them until they catch up.
        UInt32 new_max_log_ptr = parse<UInt32>(zookeeper->get(database->zookeeper_path + "/max_log_ptr"));
        unsynced_after_recovery = max_log_ptr + database->db_settings[DatabaseReplicatedSetting::max_replication_lag_to_enqueue] <= new_max_log_ptr;
        LOG_INFO(log, "Finishing replica initialization, our_log_ptr={}, max_log_ptr={}, unsynced_after_recovery={}", max_log_ptr, new_max_log_ptr, unsynced_after_recovery.load());
        if (unsynced_after_recovery)
            active_id += DatabaseReplicated::REPLICA_UNSYNCED_MARKER;
    }
    else
    {
        /// For lost_according_to_digest and ordinary connection loss we don't mark replica as "unsynced"
        /// because there's much lower chance of huge replication lag, and also we want to avoid metadata consistency issues for existing replicas
        /// when we don't wait for such replicas due to distributed_ddl_output_mode = '*_only_active'.
        unsynced_after_recovery = false;
    }

    zookeeper->create(active_path, active_id, zkutil::CreateMode::Ephemeral);
    active_node_holder_zookeeper = zookeeper;
    active_node_holder = zkutil::EphemeralNodeHolder::existing(active_path, *active_node_holder_zookeeper);
}

String DatabaseReplicatedDDLWorker::enqueueQuery(DDLLogEntry & entry, const ZooKeeperRetriesInfo &)
{
    auto zookeeper = getAndSetZooKeeper();
    return enqueueQueryImpl(zookeeper, entry, database);
}

bool DatabaseReplicatedDDLWorker::waitForReplicaToProcessAllEntries(UInt64 timeout_ms)
{
    auto zookeeper = getAndSetZooKeeper();
    const auto our_log_ptr_path = database->replica_path + "/log_ptr";
    const auto max_log_ptr_path = database->zookeeper_path + "/max_log_ptr";
    UInt32 our_log_ptr = parse<UInt32>(zookeeper->get(our_log_ptr_path));
    UInt32 max_log_ptr = parse<UInt32>(zookeeper->get(max_log_ptr_path));
    chassert(our_log_ptr <= max_log_ptr);

    /// max_log_ptr is the number of the last successfully executed request on the initiator
    /// The log could contain other entries which are not committed yet
    /// This equality is enough to say that current replicas is up-to-date
    if (our_log_ptr == max_log_ptr)
        return true;

    auto max_log =  DDLTask::getLogEntryName(max_log_ptr);

    {
        std::unique_lock lock{mutex};
        LOG_TRACE(log, "Waiting for worker thread to process all entries before {}, current task is {}", max_log, current_task);
        bool processed = wait_current_task_change.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]()
        {
            return zookeeper->expired() || current_task >= max_log || stop_flag;
        });

        if (!processed)
            return false;
    }

    /// Lets now wait for max_log_ptr to be processed
    Coordination::Stat stat;
    auto event_ptr = std::make_shared<Poco::Event>();
    auto new_log = zookeeper->get(our_log_ptr_path, &stat, event_ptr);

    if (new_log == toString(max_log_ptr))
        return true;

    return event_ptr->tryWait(timeout_ms);
}


String DatabaseReplicatedDDLWorker::enqueueQueryImpl(const ZooKeeperPtr & zookeeper, DDLLogEntry & entry,
                               DatabaseReplicated * const database, bool committed, Coordination::Requests additional_checks)
{
    const String query_path_prefix = database->zookeeper_path + "/log/query-";

    /// We cannot create sequential node and it's ephemeral child in a single transaction, so allocate sequential number another way
    String counter_prefix = database->zookeeper_path + "/counter/cnt-";
    String counter_lock_path = database->zookeeper_path + "/counter_lock";

    String counter_path;
    size_t iters = 1000;
    while (--iters)
    {
        Coordination::Requests ops;
        ops.emplace_back(zkutil::makeCreateRequest(counter_lock_path, database->getFullReplicaName(), zkutil::CreateMode::Ephemeral));
        ops.emplace_back(zkutil::makeCreateRequest(counter_prefix, "", zkutil::CreateMode::EphemeralSequential));
        ops.insert(ops.end(), additional_checks.begin(), additional_checks.end());
        Coordination::Responses res;

        Coordination::Error code = zookeeper->tryMulti(ops, res);
        if (code == Coordination::Error::ZOK)
        {
            counter_path = dynamic_cast<const Coordination::CreateResponse &>(*res[1]).path_created;
            break;
        }
        if (res[0]->error != Coordination::Error::ZNODEEXISTS)
            zkutil::KeeperMultiException::check(code, ops, res);

        sleepForMilliseconds(50);
    }

    if (counter_path.empty())
        throw Exception(ErrorCodes::UNFINISHED,
                        "Cannot enqueue query, because some replica are trying to enqueue another query. "
                        "It may happen on high queries rate or, in rare cases, after connection loss. Client should retry.");

    String node_path = query_path_prefix + counter_path.substr(counter_prefix.size());

    /// Now create task in queue
    Coordination::Requests ops;
    /// Query is not committed yet, but we have to write it into log to avoid reordering
    ops.emplace_back(zkutil::makeCreateRequest(node_path, entry.toString(), zkutil::CreateMode::Persistent));
    /// '/try' will be replaced with '/committed' or will be removed due to expired session or other error
    if (committed)
        ops.emplace_back(zkutil::makeCreateRequest(node_path + "/committed", database->getFullReplicaName(), zkutil::CreateMode::Persistent));
    else
        ops.emplace_back(zkutil::makeCreateRequest(node_path + "/try", database->getFullReplicaName(), zkutil::CreateMode::Ephemeral));
    /// We don't need it anymore
    ops.emplace_back(zkutil::makeRemoveRequest(counter_path, -1));
    /// Unlock counters
    ops.emplace_back(zkutil::makeRemoveRequest(counter_lock_path, -1));
    /// Create status dirs
    ops.emplace_back(zkutil::makeCreateRequest(node_path + "/active", "", zkutil::CreateMode::Persistent));
    ops.emplace_back(zkutil::makeCreateRequest(node_path + "/finished", "", zkutil::CreateMode::Persistent));
    ops.emplace_back(zkutil::makeCreateRequest(node_path + "/synced", "", zkutil::CreateMode::Persistent));
    zookeeper->multi(ops);


    return node_path;
}

String DatabaseReplicatedDDLWorker::tryEnqueueAndExecuteEntry(DDLLogEntry & entry, ContextPtr query_context, bool internal_query)
{
    /// NOTE Possibly it would be better to execute initial query on the most up-to-date node,
    /// but it requires more complex logic around /try node.

    OpenTelemetry::SpanHolder span(__FUNCTION__);
    span.addAttribute("clickhouse.cluster", database->getDatabaseName());
    entry.tracing_context = OpenTelemetry::CurrentContext();

    auto zookeeper = getAndSetZooKeeper();
    UInt32 our_log_ptr = getLogPointer();

    UInt32 max_log_ptr = parse<UInt32>(zookeeper->get(database->zookeeper_path + "/max_log_ptr"));

    if (our_log_ptr + database->db_settings[DatabaseReplicatedSetting::max_replication_lag_to_enqueue] < max_log_ptr)
        throw Exception(ErrorCodes::NOT_A_LEADER, "Cannot enqueue query on this replica, "
                        "because it has replication lag of {} queries. Try other replica.", max_log_ptr - our_log_ptr);

    String entry_path = enqueueQueryImpl(zookeeper, entry, database, false, query_context->getDDLAdditionalChecksOnEnqueue());
    auto try_node = zkutil::EphemeralNodeHolder::existing(entry_path + "/try", *zookeeper);
    String entry_name = entry_path.substr(entry_path.rfind('/') + 1);
    auto task = std::make_unique<DatabaseReplicatedTask>(entry_name, entry_path, database);
    task->entry = entry;
    task->parseQueryFromEntry(context);
    chassert(!task->entry.query.empty());
    assert(!zookeeper->exists(task->getFinishedNodePath()));
    task->is_initial_query = true;

    UInt64 timeout = query_context->getSettingsRef()[Setting::database_replicated_initial_query_timeout_sec];
    StopToken cancellation = query_context->getDDLQueryCancellation();
    StopCallback cancellation_callback(cancellation, [&] { wait_current_task_change.notify_all(); });
    LOG_DEBUG(log, "Waiting for worker thread to process all entries before {} (timeout: {}s{})", entry_name, timeout, cancellation.stop_possible() ? ", cancellable" : "");

    {
        std::unique_lock lock{mutex};
        bool processed = wait_current_task_change.wait_for(lock, std::chrono::seconds(timeout), [&]()
        {
            assert(zookeeper->expired() || current_task <= entry_name);

            if (zookeeper->expired() || stop_flag)
            {
                LOG_TRACE(log, "Not enqueueing query: {}", stop_flag ? "replication stopped" : "ZooKeeper session expired");
                throw Exception(ErrorCodes::DATABASE_REPLICATION_FAILED, "ZooKeeper session expired or replication stopped, try again");
            }

            if (cancellation.stop_requested())
            {
                LOG_TRACE(log, "DDL query was cancelled");
                throw Exception(ErrorCodes::QUERY_WAS_CANCELLED, "DDL query was cancelled");
            }

            return current_task == entry_name;
        });

        if (!processed)
            throw Exception(ErrorCodes::UNFINISHED, "Timeout: Cannot enqueue query on this replica, "
                            "most likely because replica is busy with previous queue entries");
    }

    if (entry.parent_table_uuid.has_value() && !checkParentTableExists(entry.parent_table_uuid.value()))
        throw Exception(ErrorCodes::TABLE_IS_DROPPED, "Parent table doesn't exist");

    processTask(*task, zookeeper, internal_query);

    if (!task->was_executed)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Entry {} was executed, but was not committed: code {}: {}",
            task->entry_name,
            task->execution_status.code,
            task->execution_status.message);
    }

    try_node->setAlreadyRemoved();

    return entry_path;
}

DDLTaskPtr DatabaseReplicatedDDLWorker::initAndCheckTask(const String & entry_name, String & out_reason, const ZooKeeperPtr & zookeeper, bool dry_run)
{
    if (!dry_run)
    {
        std::lock_guard lock{mutex};
        if (current_task < entry_name)
        {
            current_task = entry_name;
            wait_current_task_change.notify_all();
        }
    }

    UInt32 our_log_ptr = getLogPointer();
    UInt32 entry_num = DatabaseReplicatedTask::getLogEntryNumber(entry_name);

    if (entry_num <= our_log_ptr)
    {
        out_reason = fmt::format("Task {} already executed according to log pointer {}", entry_name, our_log_ptr);
        return {};
    }

    fiu_do_on(FailPoints::database_replicated_delay_entry_execution,
    {
        std::chrono::milliseconds sleep_time{thread_local_rng() % 2000};
        std::this_thread::sleep_for(sleep_time);
    });

    if (unsynced_after_recovery)
    {
        UInt32 max_log_ptr = parse<UInt32>(getAndSetZooKeeper()->get(fs::path(database->zookeeper_path) / "max_log_ptr"));
        LOG_TRACE(log, "Replica was not fully synced after recovery: our_log_ptr={}, max_log_ptr={}", our_log_ptr, max_log_ptr);
        chassert(our_log_ptr < max_log_ptr);
        bool became_synced = our_log_ptr + database->db_settings[DatabaseReplicatedSetting::max_replication_lag_to_enqueue] >= max_log_ptr;
        if (became_synced)
        {
            /// Remove the REPLICA_UNSYNCED_MARKER
            String active_id = toString(ServerUUID::get());
            active_node_holder_zookeeper->set(active_node_holder->getPath(), active_id);
            unsynced_after_recovery = false;
            LOG_INFO(log, "Replica became synced after recovery: our_log_ptr={}, max_log_ptr={}", our_log_ptr, max_log_ptr);
        }
    }

    String entry_path = fs::path(queue_dir) / entry_name;
    auto task = std::make_unique<DatabaseReplicatedTask>(entry_name, entry_path, database);

    String initiator_name;
    zkutil::EventPtr wait_committed_or_failed = std::make_shared<Poco::Event>();

    String try_node_path = fs::path(entry_path) / "try";
    if (!dry_run && zookeeper->tryGet(try_node_path, initiator_name, nullptr, wait_committed_or_failed))
    {
        task->is_initial_query = initiator_name == task->host_id_str;

        /// Query is not committed yet. We cannot just skip it and execute next one, because reordering may break replication.
        LOG_TRACE(log, "Waiting for initiator {} to commit or rollback entry {}", initiator_name, entry_path);
        constexpr size_t wait_time_ms = 1000;
        size_t max_iterations = database->db_settings[DatabaseReplicatedSetting::wait_entry_commited_timeout_sec];
        size_t iteration = 0;

        while (!wait_committed_or_failed->tryWait(wait_time_ms))
        {
            if (stop_flag)
            {
                /// We cannot return task to process and we cannot return nullptr too,
                /// because nullptr means "task should not be executed".
                /// We can only exit by exception.
                throw Exception(ErrorCodes::UNFINISHED, "Replication was stopped");
            }

            if (max_iterations <= ++iteration)
            {
                /// What can we do if initiator hangs for some reason? Seems like we can remove /try node.
                /// Initiator will fail to commit ZooKeeperMetadataTransaction (including ops for replicated table) if /try does not exist.
                /// But it's questionable.

                /// We use tryRemove(...) because multiple hosts (including initiator) may try to do it concurrently.
                auto code = zookeeper->tryRemove(try_node_path);
                if (code != Coordination::Error::ZOK && code != Coordination::Error::ZNONODE)
                    throw Coordination::Exception::fromPath(code, try_node_path);

                if (!zookeeper->exists(fs::path(entry_path) / "committed"))
                {
                    out_reason = fmt::format("Entry {} was forcefully cancelled due to timeout", entry_name);
                    return {};
                }
            }
        }
    }

    if (!zookeeper->exists(fs::path(entry_path) / "committed"))
    {
        out_reason = fmt::format("Entry {} hasn't been committed", entry_name);
        return {};
    }

    if (task->is_initial_query)
    {
        assert(!zookeeper->exists(fs::path(entry_path) / "try"));
        assert(zookeeper->exists(fs::path(entry_path) / "committed") == (zookeeper->get(task->getFinishedNodePath()) == ExecutionStatus(0).serializeText()));
        out_reason = fmt::format("Entry {} has been executed as initial query", entry_name);
        return {};
    }

    String node_data = zookeeper->get(entry_path);
    task->entry.parse(node_data);

    if (task->entry.query.empty())
    {
        /// Some replica is added or removed, let's update cached cluster
        database->setCluster(database->getClusterImpl());
        if (!database->replica_group_name.empty())
            database->setCluster(database->getClusterImpl(/*all_groups*/ true), /*all_groups*/ true);
        out_reason = fmt::format("Entry {} is a dummy task", entry_name);
        return {};
    }

    task->parseQueryFromEntry(context);

    if (zookeeper->exists(task->getFinishedNodePath()))
    {
        out_reason = fmt::format("Task {} has been already processed", entry_name);
        return {};
    }

    if (task->entry.parent_table_uuid.has_value() && !checkParentTableExists(task->entry.parent_table_uuid.value()))
    {
        out_reason = fmt::format("Parent table {} doesn't exist", task->entry.parent_table_uuid.value());
        return {};
    }

    return task;
}

bool DatabaseReplicatedDDLWorker::canRemoveQueueEntry(const String & entry_name, const Coordination::Stat &)
{
    UInt32 entry_number = DDLTaskBase::getLogEntryNumber(entry_name);
    UInt32 max_log_ptr = parse<UInt32>(getAndSetZooKeeper()->get(fs::path(database->zookeeper_path) / "max_log_ptr"));
    return entry_number + logs_to_keep < max_log_ptr;
}

bool DatabaseReplicatedDDLWorker::checkParentTableExists(const UUID & uuid) const
{
    auto [db, table] = DatabaseCatalog::instance().tryGetByUUID(uuid);
    return db.get() == database && table != nullptr && !table->is_dropped.load() && !table->is_detached.load();
}

void DatabaseReplicatedDDLWorker::initializeLogPointer(const String & processed_entry_name)
{
    updateMaxDDLEntryID(processed_entry_name);
}

UInt32 DatabaseReplicatedDDLWorker::getLogPointer() const
{
    /// NOTE it may not be equal to the log_ptr in zk:
    ///  - max_id can be equal to log_ptr - 1 due to race condition (when it's updated in zk, but not updated in memory yet)
    ///  - max_id can be greater than log_ptr, because log_ptr is not updated for failed and dummy entries
    return max_id.load();
}

UInt64 DatabaseReplicatedDDLWorker::getCurrentInitializationDurationMs() const
{
    std::lock_guard lock(initialization_duration_timer_mutex);
    return initialization_duration_timer ? initialization_duration_timer->elapsedMilliseconds() : 0;
}

}
