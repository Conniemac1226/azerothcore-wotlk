/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "MapUpdater.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Metric.h"
#include "PoolMgr.h"
#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace
{
struct MapDiagnosticRow
{
    uint32 mapId = 0;
    uint32 instanceId = 0;
    std::string mapName;
    std::string mapType;
    MapUpdateDiagnosticStats stats;
    uint32 players = 0;
    uint64 creatures = 0;
    uint64 gameObjects = 0;
    uint64 updatableObjects = 0;
    uint64 pendingUpdatableObjects = 0;
    uint32 createdGrids = 0;
    uint32 loadedGrids = 0;
    uint32 createdCells = 0;
    uint64 creatureRespawns = 0;
    uint64 gameObjectRespawns = 0;
    uint64 spawnedPoolCreatures = 0;
    uint64 spawnedPoolGameObjects = 0;
    uint64 spawnedPools = 0;
};

char const* GetMapDiagnosticType(Map* map)
{
    if (map->Instanceable() && !map->GetInstanceId())
        return "container";
    if (map->IsBattleArena())
        return "arena";
    if (map->IsBattleground())
        return "battleground";
    if (map->IsRaid())
        return "raid";
    if (map->IsDungeon())
        return "dungeon";
    return "world";
}

double MicrosecondsToMilliseconds(uint64 microseconds)
{
    return static_cast<double>(microseconds) / 1000.0;
}

double AverageMilliseconds(uint64 microseconds, uint64 samples)
{
    return samples ? MicrosecondsToMilliseconds(microseconds) / static_cast<double>(samples) : 0.0;
}

double ExecutionPercentage(uint64 phaseMicroseconds, uint64 executionMicroseconds)
{
    return executionMicroseconds ? 100.0 * static_cast<double>(phaseMicroseconds) /
        static_cast<double>(executionMicroseconds) : 0.0;
}
}

class UpdateRequest
{
public:
    UpdateRequest() = default;
    virtual ~UpdateRequest() = default;

    virtual void call() = 0;
};

class MapUpdateRequest : public UpdateRequest
{
public:
    MapUpdateRequest(Map& m, MapUpdater& u, uint32 d, uint32 sd)
        : m_map(m), m_updater(u), m_diff(d), s_diff(sd)
    {
        if (m_updater.DiagnosticsEnabled())
            _queuedAt = std::chrono::steady_clock::now();
    }

    void call() override
    {
        METRIC_TIMER("map_update_time_diff", METRIC_TAG("map_id", std::to_string(m_map.GetId())));

        std::chrono::steady_clock::time_point startedAt;
        if (m_updater.DiagnosticsEnabled())
        {
            startedAt = std::chrono::steady_clock::now();
            uint64 const queueWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
                startedAt - _queuedAt).count();
            m_map.BeginUpdateDiagnostics(queueWaitUs);
        }

        m_map.Update(m_diff, s_diff);

        if (m_updater.DiagnosticsEnabled())
        {
            uint64 const executionUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            m_map.FinishUpdateDiagnostics(executionUs);
        }

        m_updater.update_finished();
    }

private:
    Map& m_map;
    MapUpdater& m_updater;
    uint32 m_diff;
    uint32 s_diff;
    std::chrono::steady_clock::time_point _queuedAt;
};

class MapPreloadRequest : public UpdateRequest
{
public:
    MapPreloadRequest(uint32 mapId, MapUpdater& updater)
        : _mapId(mapId), _updater(updater)
    {
    }

    void call() override
    {
        Map* map = sMapMgr->CreateBaseMap(_mapId);
        LOG_INFO("server.loading", ">> Loading All Grids For Map {} ({})", map->GetId(), map->GetMapName());
        map->LoadAllGrids();
        _updater.update_finished();
    }

private:
    uint32 _mapId;
    MapUpdater& _updater;
};

class LFGUpdateRequest : public UpdateRequest
{
public:
    LFGUpdateRequest(MapUpdater& u, uint32 d) : m_updater(u), m_diff(d)
    {
        if (m_updater.DiagnosticsEnabled())
            _queuedAt = std::chrono::steady_clock::now();
    }

    void call() override
    {
        std::chrono::steady_clock::time_point startedAt;
        if (m_updater.DiagnosticsEnabled())
            startedAt = std::chrono::steady_clock::now();

        sLFGMgr->Update(m_diff, 1);

        if (m_updater.DiagnosticsEnabled())
        {
            std::chrono::steady_clock::time_point const finishedAt = std::chrono::steady_clock::now();
            uint64 const queueWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
                startedAt - _queuedAt).count();
            uint64 const executionUs = std::chrono::duration_cast<std::chrono::microseconds>(
                finishedAt - startedAt).count();
            m_updater.RecordLFGDiagnostics(queueWaitUs, executionUs);
        }

        m_updater.update_finished();
    }
private:
    MapUpdater& m_updater;
    uint32 m_diff;
    std::chrono::steady_clock::time_point _queuedAt;
};

MapUpdater::MapUpdater() : pending_requests(0), _cancelationToken(false)
{
}

void MapUpdater::ConfigureDiagnostics()
{
    _diagnosticsEnabled = sConfigMgr->GetOption<bool>("MapUpdate.Diagnostics.Enable", false);
    _diagnosticsIntervalSeconds = std::max<uint32>(30,
        sConfigMgr->GetOption<uint32>("MapUpdate.Diagnostics.Interval", 300));
    _diagnosticsTopCount = std::clamp<uint32>(
        sConfigMgr->GetOption<uint32>("MapUpdate.Diagnostics.TopCount", 10), 1, 25);
    _lastDiagnosticsReport = std::chrono::steady_clock::now();

    if (_diagnosticsEnabled)
    {
        LOG_INFO("time.update.map",
            "Map update diagnostics enabled: interval={}s top_count={}. This temporary profiler adds clock "
            "sampling overhead to map updates.",
            _diagnosticsIntervalSeconds, _diagnosticsTopCount);
    }
}

void MapUpdater::activate(std::size_t num_threads)
{
    _workerThreads.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
    {
        _workerThreads.push_back(std::thread(&MapUpdater::WorkerThread, this));
    }
}

void MapUpdater::deactivate()
{
    _cancelationToken = true;

    wait();  // This is where we wait for tasks to complete

    _queue.Cancel();  // Cancel the queue to prevent further task processing

    // Join all worker threads
    for (auto& thread : _workerThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

void MapUpdater::wait()
{
    std::chrono::steady_clock::time_point startedAt;
    if (_diagnosticsEnabled)
        startedAt = std::chrono::steady_clock::now();

    std::unique_lock<std::mutex> guard(_lock);  // Guard lock for safe waiting

    // Wait until there are no pending requests
    _condition.wait(guard, [this] {
        return pending_requests.load(std::memory_order_acquire) == 0;
    });

    if (_diagnosticsEnabled)
    {
        uint64 const waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        ++_diagnosticWaitSamples;
        _diagnosticWaitTotalUs += waitUs;
        _diagnosticWaitMaxUs = std::max(_diagnosticWaitMaxUs, waitUs);
    }
}

void MapUpdater::RecordLFGDiagnostics(uint64 queueWaitUs, uint64 executionUs)
{
    ++_diagnosticLfgSamples;
    _diagnosticLfgQueueTotalUs += queueWaitUs;
    _diagnosticLfgQueueMaxUs = std::max(_diagnosticLfgQueueMaxUs, queueWaitUs);
    _diagnosticLfgExecutionTotalUs += executionUs;
    _diagnosticLfgExecutionMaxUs = std::max(_diagnosticLfgExecutionMaxUs, executionUs);
}

void MapUpdater::ReportDiagnostics()
{
    if (!_diagnosticsEnabled)
        return;

    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    if (now - _lastDiagnosticsReport < std::chrono::seconds(_diagnosticsIntervalSeconds))
        return;

    _lastDiagnosticsReport = now;

    std::vector<MapDiagnosticRow> rows;
    uint64 totalPlayers = 0;
    uint64 totalCreatures = 0;
    uint64 totalGameObjects = 0;
    uint64 totalUpdatableObjects = 0;
    uint64 totalPendingUpdatableObjects = 0;
    uint64 totalCreatedGrids = 0;
    uint64 totalLoadedGrids = 0;
    uint64 totalCreatedCells = 0;
    uint64 totalCreatureRespawns = 0;
    uint64 totalGameObjectRespawns = 0;
    uint64 totalSpawnedPoolCreatures = 0;
    uint64 totalSpawnedPoolGameObjects = 0;
    uint64 totalSpawnedPools = 0;
    uint32 mapCount = 0;
    uint32 instanceCount = 0;
    uint32 arenaCount = 0;
    uint32 battlegroundCount = 0;
    uint32 dungeonCount = 0;

    sMapMgr->DoForAllMaps([&](Map* map)
    {
        ++mapCount;
        if (map->GetInstanceId())
        {
            ++instanceCount;
            if (map->IsBattleArena())
                ++arenaCount;
            else if (map->IsBattleground())
                ++battlegroundCount;
            else if (map->IsDungeon())
                ++dungeonCount;
        }

        MapDiagnosticRow row;
        row.mapId = map->GetId();
        row.instanceId = map->GetInstanceId();
        row.mapName = map->GetMapName();
        row.mapType = GetMapDiagnosticType(map);
        row.stats = map->ConsumeUpdateDiagnosticStats();
        row.players = map->GetPlayersCountExceptGMs();
        row.creatures = map->GetObjectsStore().Size<Creature>();
        row.gameObjects = map->GetObjectsStore().Size<GameObject>();
        row.updatableObjects = map->GetUpdatableObjectsCount();
        row.pendingUpdatableObjects = map->GetPendingUpdatableObjectsCount();
        row.createdGrids = map->GetCreatedGridsCount();
        row.loadedGrids = map->GetLoadedGridsCount();
        row.createdCells = map->GetCreatedCellsInMapCount();
        row.creatureRespawns = map->GetCreatureRespawnTimes().size();
        row.gameObjectRespawns = map->GetGORespawnTimes().size();
        row.spawnedPoolCreatures = map->GetPoolData().GetSpawnedCreatureCount();
        row.spawnedPoolGameObjects = map->GetPoolData().GetSpawnedGameObjectCount();
        row.spawnedPools = map->GetPoolData().GetSpawnedPoolCount();

        totalPlayers += row.players;
        totalCreatures += row.creatures;
        totalGameObjects += row.gameObjects;
        totalUpdatableObjects += row.updatableObjects;
        totalPendingUpdatableObjects += row.pendingUpdatableObjects;
        totalCreatedGrids += row.createdGrids;
        totalLoadedGrids += row.loadedGrids;
        totalCreatedCells += row.createdCells;
        totalCreatureRespawns += row.creatureRespawns;
        totalGameObjectRespawns += row.gameObjectRespawns;
        totalSpawnedPoolCreatures += row.spawnedPoolCreatures;
        totalSpawnedPoolGameObjects += row.spawnedPoolGameObjects;
        totalSpawnedPools += row.spawnedPools;

        if (row.stats.samples)
            rows.push_back(std::move(row));
    });

    std::sort(rows.begin(), rows.end(), [](MapDiagnosticRow const& left, MapDiagnosticRow const& right)
    {
        if (left.stats.executionTotalUs != right.stats.executionTotalUs)
            return left.stats.executionTotalUs > right.stats.executionTotalUs;
        return left.stats.executionMaxUs > right.stats.executionMaxUs;
    });

    uint64 totalSamples = 0;
    uint64 totalExecutionUs = 0;
    uint64 maximumExecutionUs = 0;
    uint64 maximumQueueUs = 0;
    for (MapDiagnosticRow const& row : rows)
    {
        totalSamples += row.stats.samples;
        totalExecutionUs += row.stats.executionTotalUs;
        maximumExecutionUs = std::max(maximumExecutionUs, row.stats.executionMaxUs);
        maximumQueueUs = std::max(maximumQueueUs, row.stats.queueMaxUs);
    }

    LOG_INFO("time.update.map",
        "MapUpdateDiag summary interval={}s maps={} instances={} dungeon={} battleground={} arena={} "
        "samples={} execution_total_ms={:.1f} execution_max_ms={:.1f} queue_max_ms={:.1f} "
        "wait_avg_ms={:.3f} wait_max_ms={:.3f} lfg_avg_ms={:.3f} lfg_max_ms={:.3f} "
        "lfg_queue_avg_ms={:.3f} lfg_queue_max_ms={:.3f} players={} "
        "creatures={} gameobjects={} updatable={}/{} grids={}/{} cells={} respawns={}/{} pools={}/{}/{}",
        _diagnosticsIntervalSeconds, mapCount, instanceCount, dungeonCount, battlegroundCount, arenaCount,
        totalSamples, MicrosecondsToMilliseconds(totalExecutionUs), MicrosecondsToMilliseconds(maximumExecutionUs),
        MicrosecondsToMilliseconds(maximumQueueUs),
        AverageMilliseconds(_diagnosticWaitTotalUs, _diagnosticWaitSamples),
        MicrosecondsToMilliseconds(_diagnosticWaitMaxUs),
        AverageMilliseconds(_diagnosticLfgExecutionTotalUs, _diagnosticLfgSamples),
        MicrosecondsToMilliseconds(_diagnosticLfgExecutionMaxUs),
        AverageMilliseconds(_diagnosticLfgQueueTotalUs, _diagnosticLfgSamples),
        MicrosecondsToMilliseconds(_diagnosticLfgQueueMaxUs), totalPlayers, totalCreatures, totalGameObjects,
        totalUpdatableObjects, totalPendingUpdatableObjects, totalLoadedGrids, totalCreatedGrids, totalCreatedCells,
        totalCreatureRespawns, totalGameObjectRespawns, totalSpawnedPoolCreatures, totalSpawnedPoolGameObjects,
        totalSpawnedPools);

    _diagnosticWaitSamples = 0;
    _diagnosticWaitTotalUs = 0;
    _diagnosticWaitMaxUs = 0;
    _diagnosticLfgSamples = 0;
    _diagnosticLfgQueueTotalUs = 0;
    _diagnosticLfgQueueMaxUs = 0;
    _diagnosticLfgExecutionTotalUs = 0;
    _diagnosticLfgExecutionMaxUs = 0;

    uint32 const reportCount = std::min<uint32>(_diagnosticsTopCount, static_cast<uint32>(rows.size()));
    for (uint32 rank = 0; rank < reportCount; ++rank)
    {
        MapDiagnosticRow const& row = rows[rank];
        uint64 phaseTotalUs = 0;
        for (uint64 phaseUs : row.stats.phaseTotalUs)
            phaseTotalUs += phaseUs;
        uint64 const otherUs = row.stats.executionTotalUs > phaseTotalUs ?
            row.stats.executionTotalUs - phaseTotalUs : 0;

        LOG_INFO("time.update.map",
            "MapUpdateDiag rank={} map={} instance={} type={} name='{}' samples={} full={} total_ms={:.1f} "
            "avg_ms={:.3f} max_ms={:.3f} queue_avg_ms={:.3f} queue_max_ms={:.3f} "
            "phase_pct[c={:.1f} se={:.1f} ev={:.1f} r={:.1f} p={:.1f} np={:.1f} ou={:.1f} sc={:.1f} "
            "mv={:.1f} t={:.1f} other={:.1f}] objects[p={} c={} go={} up={}/{}] grids={}/{} cells={} "
            "respawns={}/{} pools={}/{}/{}",
            rank + 1, row.mapId, row.instanceId, row.mapType, row.mapName, row.stats.samples,
            row.stats.fullUpdates, MicrosecondsToMilliseconds(row.stats.executionTotalUs),
            AverageMilliseconds(row.stats.executionTotalUs, row.stats.samples),
            MicrosecondsToMilliseconds(row.stats.executionMaxUs),
            AverageMilliseconds(row.stats.queueTotalUs, row.stats.samples),
            MicrosecondsToMilliseconds(row.stats.queueMaxUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_COLLISION],
                row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_SESSIONS],
                row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_EVENTS], row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_RESPAWNS], row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_PLAYERS], row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_NON_PLAYERS],
                row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_OBJECT_UPDATES],
                row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_SCRIPTS], row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_MOVEMENT], row.stats.executionTotalUs),
            ExecutionPercentage(row.stats.phaseTotalUs[MAP_UPDATE_DIAGNOSTIC_TAIL], row.stats.executionTotalUs),
            ExecutionPercentage(otherUs, row.stats.executionTotalUs), row.players, row.creatures, row.gameObjects,
            row.updatableObjects, row.pendingUpdatableObjects, row.loadedGrids, row.createdGrids, row.createdCells,
            row.creatureRespawns, row.gameObjectRespawns, row.spawnedPoolCreatures, row.spawnedPoolGameObjects,
            row.spawnedPools);
    }
}

void MapUpdater::schedule_task(UpdateRequest* request)
{
    // Atomic increment for pending_requests
    pending_requests.fetch_add(1, std::memory_order_release);
    _queue.Push(request);
}

void MapUpdater::schedule_update(Map& map, uint32 diff, uint32 s_diff)
{
    schedule_task(new MapUpdateRequest(map, *this, diff, s_diff));
}

void MapUpdater::schedule_map_preload(uint32 mapid)
{
    schedule_task(new MapPreloadRequest(mapid, *this));
}

void MapUpdater::schedule_lfg_update(uint32 diff)
{
    schedule_task(new LFGUpdateRequest(*this, diff));
}

bool MapUpdater::activated()
{
    return !_workerThreads.empty();
}

void MapUpdater::update_finished()
{
    // Atomic decrement for pending_requests
    if (pending_requests.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        // Only notify when pending_requests becomes 0 (i.e., all tasks are finished)
        std::lock_guard<std::mutex> lock(_lock);  // Lock only for condition variable notification
        _condition.notify_all();  // Notify waiting threads that all requests are complete
    }
}

void MapUpdater::WorkerThread()
{
    LoginDatabase.WarnAboutSyncQueries(true);
    CharacterDatabase.WarnAboutSyncQueries(true);
    WorldDatabase.WarnAboutSyncQueries(true);

    while (!_cancelationToken)
    {
        UpdateRequest* request = nullptr;

        _queue.WaitAndPop(request);  // Wait for and pop a request from the queue

        if (!_cancelationToken && request)
        {
            request->call();  // Execute the request
            delete request;  // Clean up after processing
        }
    }
}
