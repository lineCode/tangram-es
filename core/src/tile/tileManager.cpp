#include "tileManager.h"
#include "scene/scene.h"
#include "tile/mapTile.h"
#include "view/view.h"

#include <chrono>
#include <algorithm>

TileManager::TileManager() {
    // Instantiate workers
    for (size_t i = 0; i < MAX_WORKERS; i++) {
        m_workers.push_back(std::unique_ptr<TileWorker>(new TileWorker()));
    }
}

TileManager::TileManager(TileManager&& _other) :
    m_view(std::move(_other.m_view)),
    m_tileSet(std::move(_other.m_tileSet)),
    m_dataSources(std::move(_other.m_dataSources)),
    m_workers(std::move(_other.m_workers)),
    m_queuedTiles(std::move(_other.m_queuedTiles)) {
}

TileManager::~TileManager() {
    for (auto& worker : m_workers) {
        if (!worker->isFree()) {
            worker->abort();
            worker->getTileResult();
        }
        // We stop all workers before we destroy the resources they use.
        // TODO: This will wait for any pending network requests to finish,
        // which could delay closing of the application. 
    }
    m_dataSources.clear();
    m_tileSet.clear();
}

void TileManager::addToWorkerQueue(const std::string& _rawData, const TileID& _tileId, const int _dataSourceID) {
    m_queuedTiles.push_front(WorkerData(_rawData, _tileId, _dataSourceID));
}

bool TileManager::updateTileSet() {
    
    bool tileSetChanged = false;
    
    // Check if any native worker needs to be dispatched i.e. queuedTiles is not empty
    {
        auto workersIter = m_workers.begin();
        auto queuedTilesIter = m_queuedTiles.begin();

        while (workersIter != m_workers.end() && queuedTilesIter != m_queuedTiles.end()) {

            auto& workerData = *queuedTilesIter;
            auto& worker = *workersIter;

            if (worker->isFree()) {
                logMsg("Dispatched worker for tile: [%d, %d, %d]\n", workerData.tileID->x, workerData.tileID->y, workerData.tileID->z);
                worker->processTileData(workerData, m_dataSources, m_scene->getStyles(), *m_view);
                queuedTilesIter = m_queuedTiles.erase(queuedTilesIter);
            }

            ++workersIter;
        }
    }

    // Check if any incoming tiles are finished
    for (auto& worker : m_workers) {
        
        if (!worker->isFree() && worker->isFinished()) {
            
            // Get result from worker and move it into tile set
            auto tile = worker->getTileResult();
            const TileID& id = tile->getID();
            logMsg("Tile [%d, %d, %d] finished loading\n", id.z, id.x, id.y);
            std::swap(m_tileSet[id], tile);
            cleanProxyTiles(id);
            tileSetChanged = true;
            
        }
        
    }
    
    if (! (m_view->changedOnLastUpdate() || tileSetChanged) ) {
        // No new tiles have come into view and no tiles have finished loading, 
        // so the tileset is unchanged
        return false;
    }
    
    const std::set<TileID>& visibleTiles = m_view->getVisibleTiles();
    
    // Loop over visibleTiles and add any needed tiles to tileSet
    {
        auto setTilesIter = m_tileSet.begin();
        auto visTilesIter = visibleTiles.begin();
        
        while (visTilesIter != visibleTiles.end()) {
            
            if (setTilesIter == m_tileSet.end() || *visTilesIter < setTilesIter->first) {
                // tileSet is missing an element present in visibleTiles
                addTile(*visTilesIter);
                tileSetChanged = true;
                ++visTilesIter;
            } else if (setTilesIter->first < *visTilesIter) {
                // visibleTiles is missing an element present in tileSet (handled below)
                ++setTilesIter;
            } else {
                // tiles in both sets match, move on
                ++setTilesIter;
                ++visTilesIter;
            }
        }
    }
    
    // Loop over tileSet and remove any tiles that are neither visible nor proxies
    {
        auto setTilesIter = m_tileSet.begin();
        auto visTilesIter = visibleTiles.begin();
        
        while (setTilesIter != m_tileSet.end()) {
            
            if (visTilesIter == visibleTiles.end() || setTilesIter->first < *visTilesIter) {
                // visibleTiles is missing an element present in tileSet
                if (setTilesIter->second->getProxyCounter() <= 0) {
                    removeTile(setTilesIter);
                    tileSetChanged = true;
                } else {
                    ++setTilesIter;
                }
            } else if (*visTilesIter < setTilesIter->first) {
                // tileSet is missing an element present in visibleTiles (shouldn't occur)
                ++visTilesIter;
            } else {
                // tiles in both sets match, move on
                ++setTilesIter;
                ++visTilesIter;
            }
        }
    }
    
    return tileSetChanged;
}

void TileManager::addTile(const TileID& _tileID) {
    
    std::shared_ptr<MapTile> tile(new MapTile(_tileID, m_view->getMapProjection()));
    m_tileSet[_tileID] = std::move(tile);

    for(int dsIndex = 0; dsIndex < m_dataSources.size(); dsIndex++) {
        // ByPass Network Request if data already loaded/parsed
        // Create workerData with empty "rawData", parsed data will be fetched in the Worker::processTileData
        if(m_dataSources[dsIndex]->hasTileData(_tileID)) {
            addToWorkerQueue(std::string(""), _tileID, dsIndex);
        } else if( !m_dataSources[dsIndex]->loadTileData(_tileID, dsIndex) ) {
            logMsg("ERROR: Loading failed for tile [%d, %d, %d]\n", _tileID.z, _tileID.x, _tileID.y);
        }
    }
    
    //Add Proxy if corresponding proxy MapTile ready
    updateProxyTiles(_tileID, m_view->isZoomIn());
}

void TileManager::removeTile(std::map< TileID, std::shared_ptr<MapTile> >::iterator& _tileIter) {
    
    const TileID& id = _tileIter->first;

    // tmp workerData required as "compare value" for std::find.
    WorkerData tmp = WorkerData(std::string(""), id, 0);
    
    // Remove tile from queue, if present
    const auto& found = std::find(m_queuedTiles.begin(), m_queuedTiles.end(), tmp);
    if (found != m_queuedTiles.end()) {
        m_queuedTiles.erase(found);
        cleanProxyTiles(id);
    }
    
    // If a worker is loading this tile, abort it
    for (const auto& worker : m_workers) {
        if (!worker->isFree() && worker->getTileID() == id) {
            //TODO: Before aborting worker, call DataSource->cancel to make platform specific http cancel request.
            worker->abort();
            // Proxy tiles will be cleaned in update loop
        }
    }

    // Remove labels for tile
    LabelContainer::GetInstance()->removeLabels(id);

    // Remove tile from set
    _tileIter = m_tileSet.erase(_tileIter);
    
}

void TileManager::updateProxyTiles(const TileID& _tileID, bool _zoomingIn) {
    if (_zoomingIn) {
        // zoom in - add parent
        const auto& parentID = _tileID.getParent();
        const auto& parentTileIter = m_tileSet.find(parentID);
        if (parentID.isValid() && parentTileIter != m_tileSet.end()) {
            parentTileIter->second->incProxyCounter();
        }
    } else {
        for(int i = 0; i < 4; i++) {
            const auto& childID = _tileID.getChild(i);
            const auto& childTileIter = m_tileSet.find(childID);
            if(childID.isValid(m_view->s_maxZoom) && childTileIter != m_tileSet.end()) {
                childTileIter->second->incProxyCounter();
            }
        }
    }
}

void TileManager::cleanProxyTiles(const TileID& _tileID) {
    // check if parent proxy is present
    const auto& parentID = _tileID.getParent();
    const auto& parentTileIter = m_tileSet.find(parentID);
    if (parentID.isValid() && parentTileIter != m_tileSet.end()) {
        parentTileIter->second->decProxyCounter();
    }
    
    // check if child proxies are present
    for(int i = 0; i < 4; i++) {
        const auto& childID = _tileID.getChild(i);
        const auto& childTileIter = m_tileSet.find(childID);
        if(childID.isValid(m_view->s_maxZoom) && childTileIter != m_tileSet.end()) {
            childTileIter->second->decProxyCounter();
        }
    }
}
