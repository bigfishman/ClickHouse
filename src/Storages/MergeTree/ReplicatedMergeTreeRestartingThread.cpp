#include <IO/Operators.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Storages/MergeTree/ReplicatedMergeTreeRestartingThread.h>
#include <Storages/MergeTree/ReplicatedMergeTreeQuorumEntry.h>
#include <Storages/MergeTree/ReplicatedMergeTreeAddress.h>
#include <Interpreters/Context.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/randomSeed.h>


namespace ProfileEvents
{
    extern const Event ReplicaPartialShutdown;
}

namespace CurrentMetrics
{
    extern const Metric ReadonlyReplica;
}


namespace DB
{

namespace ErrorCodes
{
    extern const int REPLICA_IS_ALREADY_ACTIVE;
}

namespace
{
    constexpr auto retry_period_ms = 10 * 1000;
}

/// Used to check whether it's us who set node `is_active`, or not.
static String generateActiveNodeIdentifier()
{
    return "pid: " + toString(getpid()) + ", random: " + toString(randomSeed());
}

ReplicatedMergeTreeRestartingThread::ReplicatedMergeTreeRestartingThread(StorageReplicatedMergeTree & storage_)
    : storage(storage_)
    , log_name(storage.getStorageID().getFullTableName() + " (ReplicatedMergeTreeRestartingThread)")
    , log(&Poco::Logger::get(log_name))
    , active_node_identifier(generateActiveNodeIdentifier())
{
    const auto storage_settings = storage.getSettings();
    check_period_ms = storage_settings->zookeeper_session_expiration_check_period.totalSeconds() * 1000;

    task = storage.global_context.getSchedulePool().createTask(log_name, [this]{ run(); });
}

void ReplicatedMergeTreeRestartingThread::run()
{
    if (need_stop)
        return;

    try
    {
        if (first_time || storage.getZooKeeper()->expired())
        {
            startup_completed = false;

            if (first_time)
            {
                LOG_DEBUG(log, "Activating replica.");
            }
            else
            {
                LOG_WARNING(log, "ZooKeeper session has expired. Switching to a new session.");

                bool old_val = false;
                if (storage.is_readonly.compare_exchange_strong(old_val, true))
                    CurrentMetrics::add(CurrentMetrics::ReadonlyReplica);

                partialShutdown();
            }

            if (!startup_completed)
            {
                try
                {
                    storage.setZooKeeper(storage.global_context.getZooKeeper());
                }
                catch (const Coordination::Exception &)
                {
                    /// The exception when you try to zookeeper_init usually happens if DNS does not work. We will try to do it again.
                    tryLogCurrentException(log, __PRETTY_FUNCTION__);

                    if (first_time)
                        storage.startup_event.set();
                    task->scheduleAfter(retry_period_ms);
                    return;
                }

                if (!need_stop && !tryStartup())
                {
                    if (first_time)
                        storage.startup_event.set();
                    task->scheduleAfter(retry_period_ms);
                    return;
                }

                if (first_time)
                    storage.startup_event.set();

                startup_completed = true;
            }

            if (need_stop)
                return;

            bool old_val = true;
            if (storage.is_readonly.compare_exchange_strong(old_val, false))
                CurrentMetrics::sub(CurrentMetrics::ReadonlyReplica);

            first_time = false;
        }
    }
    catch (...)
    {
        storage.startup_event.set();
        tryLogCurrentException(log, __PRETTY_FUNCTION__);
    }

    task->scheduleAfter(check_period_ms);
}


bool ReplicatedMergeTreeRestartingThread::tryStartup()
{
    try
    {
        removeFailedQuorumParts();
        activateReplica();

        const auto & zookeeper = storage.getZooKeeper();
        const auto storage_settings = storage.getSettings();

        storage.cloneReplicaIfNeeded(zookeeper);

        storage.queue.load(zookeeper);

        /// pullLogsToQueue() after we mark replica 'is_active' (and after we repair if it was lost);
        /// because cleanup_thread doesn't delete log_pointer of active replicas.
        storage.queue.pullLogsToQueue(zookeeper);
        storage.queue.removeCurrentPartsFromMutations();
        storage.last_queue_update_finish_time.store(time(nullptr));

        updateQuorumIfWeHavePart();

        if (storage_settings->replicated_can_become_leader)
            storage.enterLeaderElection();
        else
            LOG_INFO(log, "Will not enter leader election because replicated_can_become_leader=0");

        /// Anything above can throw a KeeperException if something is wrong with ZK.
        /// Anything below should not throw exceptions.

        storage.partial_shutdown_called = false;
        storage.partial_shutdown_event.reset();

        storage.queue_updating_task->activateAndSchedule();
        storage.mutations_updating_task->activateAndSchedule();
        storage.mutations_finalizing_task->activateAndSchedule();
        storage.cleanup_thread.start();
        storage.part_check_thread.start();

        return true;
    }
    catch (...)
    {
        storage.replica_is_active_node = nullptr;

        try
        {
            throw;
        }
        catch (const Coordination::Exception & e)
        {
            LOG_ERROR(log, "Couldn't start replication: {}. {}", e.what(), DB::getCurrentExceptionMessage(true));
            return false;
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::REPLICA_IS_ALREADY_ACTIVE)
                throw;

            LOG_ERROR(log, "Couldn't start replication: {}. {}", e.what(), DB::getCurrentExceptionMessage(true));
            return false;
        }
    }
}


void ReplicatedMergeTreeRestartingThread::removeFailedQuorumParts()
{
    auto zookeeper = storage.getZooKeeper();

    Strings failed_parts;
    if (zookeeper->tryGetChildren(storage.zookeeper_path + "/quorum/failed_parts", failed_parts) != Coordination::Error::ZOK)
        return;

    /// Firstly, remove parts from ZooKeeper
    storage.tryRemovePartsFromZooKeeperWithRetries(failed_parts);

    for (const auto & part_name : failed_parts)
    {
        auto part = storage.getPartIfExists(
            part_name, {MergeTreeDataPartState::PreCommitted, MergeTreeDataPartState::Committed, MergeTreeDataPartState::Outdated});

        if (part)
        {
            LOG_DEBUG(log, "Found part {} with failed quorum. Moving to detached. This shouldn't happen often.", part_name);
            storage.forgetPartAndMoveToDetached(part, "noquorum");
            storage.queue.removeFromVirtualParts(part->info);
        }
    }
}


void ReplicatedMergeTreeRestartingThread::updateQuorumIfWeHavePart()
{
    auto zookeeper = storage.getZooKeeper();

    String quorum_str;
    if (zookeeper->tryGet(storage.zookeeper_path + "/quorum/status", quorum_str))
    {
        ReplicatedMergeTreeQuorumEntry quorum_entry;
        quorum_entry.fromString(quorum_str);

        if (!quorum_entry.replicas.count(storage.replica_name)
            && zookeeper->exists(storage.replica_path + "/parts/" + quorum_entry.part_name))
        {
            LOG_WARNING(log, "We have part {} but we is not in quorum. Updating quorum. This shouldn't happen often.", quorum_entry.part_name);
            storage.updateQuorum(quorum_entry.part_name);
        }
    }
}


void ReplicatedMergeTreeRestartingThread::activateReplica()
{
    auto zookeeper = storage.getZooKeeper();

    /// How other replicas can access this one.
    ReplicatedMergeTreeAddress address = storage.getReplicatedMergeTreeAddress();

    String is_active_path = storage.replica_path + "/is_active";

    /** If the node is marked as active, but the mark is made in the same instance, delete it.
      * This is possible only when session in ZooKeeper expires.
      */
    String data;
    Coordination::Stat stat;
    bool has_is_active = zookeeper->tryGet(is_active_path, data, &stat);
    if (has_is_active && data == active_node_identifier)
    {
        auto code = zookeeper->tryRemove(is_active_path, stat.version);

        if (code == Coordination::Error::ZBADVERSION)
            throw Exception("Another instance of replica " + storage.replica_path + " was created just now."
                " You shouldn't run multiple instances of same replica. You need to check configuration files.",
                ErrorCodes::REPLICA_IS_ALREADY_ACTIVE);

        if (code != Coordination::Error::ZOK && code != Coordination::Error::ZNONODE)
            throw Coordination::Exception(code, is_active_path);
    }

    /// Simultaneously declare that this replica is active, and update the host.
    Coordination::Requests ops;
    ops.emplace_back(zkutil::makeCreateRequest(is_active_path, active_node_identifier, zkutil::CreateMode::Ephemeral));
    ops.emplace_back(zkutil::makeSetRequest(storage.replica_path + "/host", address.toString(), -1));

    try
    {
        zookeeper->multi(ops);
    }
    catch (const Coordination::Exception & e)
    {
        if (e.code == Coordination::Error::ZNODEEXISTS)
            throw Exception("Replica " + storage.replica_path + " appears to be already active. If you're sure it's not, "
                "try again in a minute or remove znode " + storage.replica_path + "/is_active manually", ErrorCodes::REPLICA_IS_ALREADY_ACTIVE);

        throw;
    }

    /// `current_zookeeper` lives for the lifetime of `replica_is_active_node`,
    ///  since before changing `current_zookeeper`, `replica_is_active_node` object is destroyed in `partialShutdown` method.
    storage.replica_is_active_node = zkutil::EphemeralNodeHolder::existing(is_active_path, *storage.current_zookeeper);
}


void ReplicatedMergeTreeRestartingThread::partialShutdown()
{
    ProfileEvents::increment(ProfileEvents::ReplicaPartialShutdown);

    storage.partial_shutdown_called = true;
    storage.partial_shutdown_event.set();
    storage.replica_is_active_node = nullptr;

    LOG_TRACE(log, "Waiting for threads to finish");

    storage.exitLeaderElection();

    storage.queue_updating_task->deactivate();
    storage.mutations_updating_task->deactivate();
    storage.mutations_finalizing_task->deactivate();

    storage.cleanup_thread.stop();
    storage.part_check_thread.stop();

    LOG_TRACE(log, "Threads finished");
}


void ReplicatedMergeTreeRestartingThread::shutdown()
{
    /// Stop restarting_thread before stopping other tasks - so that it won't restart them again.
    need_stop = true;
    task->deactivate();
    LOG_TRACE(log, "Restarting thread finished");

    /// Stop other tasks.
    partialShutdown();
}

}
