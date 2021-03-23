#include "Database.h"

namespace db {

utils::BoxT<DBU> Database::getWireBox(int layerIdx, utils::PointT<DBU> pu, utils::PointT<DBU> pv) const {
    DBU width = layers[layerIdx].width * 0.5;
    if (pu.x > pv.x || pu.y > pv.y)
        std::swap(pu, pv);
    return utils::BoxT<DBU>(pu.x-width, pu.y-width, pv.x+width, pv.y+width);
}

int Database::countOvlp(const BoxOnLayer &box,
                            const vector<utils::BoxT<DBU>> &regions,
                            const vector<utils::BoxT<DBU>> &neiMetals) const {
    int numOvlp = 0;
    for (const auto& neiMetal : neiMetals) {
        // getOvlpFixedMetals
        for (auto forbidRegion : regions) {
            const auto &ovlp = forbidRegion.IntersectWith(neiMetal);
            if (ovlp.IsValid()) {
                numOvlp += (ovlp.area() > 0);
            }
        }

        // getOvlpFixedMetalForbidRegions
        const auto &forbidRegions = getAccurateMetalRectForbidRegions({box.layerIdx, neiMetal});
        for (auto forbidRegion : forbidRegions) {
            const auto &ovlp = forbidRegion.IntersectWith(box);
            if (ovlp.IsValid()) {
                numOvlp += (ovlp.area() > 0);
            }
        }

        // getOvlpC2CMetals
        if (!layers[box.layerIdx].isEolDominated(neiMetal)) {
            DBU space = layers[box.layerIdx].getParaRunSpace(neiMetal);
            numOvlp += (utils::L2Dist(box, neiMetal) < space);
        }
    }
    return numOvlp;
}

int Database::getPinLinkVio(const BoxOnLayer& box, int netIdx, bool debug) const {
    int lid = box.layerIdx;
    const auto &regions = getAccurateMetalRectForbidRegions(box);
    utils::BoxT<DBU> queryBox = box;
    queryBox.x.low -= layers[lid].fixedMetalQueryMargin;
    queryBox.x.high += layers[lid].fixedMetalQueryMargin;
    queryBox.y.low -= layers[lid].fixedMetalQueryMargin;
    queryBox.y.high += layers[lid].fixedMetalQueryMargin;

    for (const auto& region : regions) {
        queryBox = queryBox.UnionWith(region);
    }

    // fixed metal
    boostBox rtreeQueryBox(boostPoint(queryBox.x.low, queryBox.y.low), boostPoint(queryBox.x.high, queryBox.y.high));
    vector<std::pair<boostBox, int>> queryResults;
    fixedMetals[lid].query(bgi::intersects(rtreeQueryBox), std::back_inserter(queryResults));
    vector<utils::BoxT<DBU>> neiMetals;
    for (const auto& queryResult : queryResults) {
        if (queryResult.second != netIdx) {
            const auto& b = queryResult.first;
            neiMetals.emplace_back(bg::get<bg::min_corner, 0>(b),
                                    bg::get<bg::min_corner, 1>(b),
                                    bg::get<bg::max_corner, 0>(b),
                                    bg::get<bg::max_corner, 1>(b));
        }
    }

    GridBoxOnLayer queryGridBox = rangeSearch(BoxOnLayer(lid, queryBox));
    // queryGridBox.trackRange.low -= 1;
    // queryGridBox.trackRange.high += 1;
    // queryGridBox.crossPointRange.low -= 1;
    // queryGridBox.crossPointRange.high += 1;
    // getRoutedBox(queryGridBox, neiMetals, netIdx);

    // routed metal (via only)
    GridPoint via;
    int lc=0, uc=0;
    if (lid > 0) {
        int low = lid - 1;
        via.layerIdx = low;
        const auto &loBox = getLower(queryGridBox);
        lc = max(0, loBox.crossPointRange.low - 1);
        uc = min(layers[low].numCrossPoints() - 1, loBox.crossPointRange.high + 1);
        for (int t=loBox.trackRange.low-1; t<=loBox.trackRange.high; t++) {
            via.trackIdx = t;
            auto itLo = routedViaMap[low][t].lower_bound(lc);
            auto itHi = routedViaMap[low][t].upper_bound(uc);
            for (auto it=itLo; it!=itHi; it++) {
                if (it->second == netIdx) continue;
                via.crossPointIdx = it->first;
                const auto &viaLoc = getLoc(via);
                const auto viaType = getViaType(via);
                neiMetals.push_back(viaType->getShiftedTopMetal(viaLoc));
            }
        }
    }
    lc = max(0, queryGridBox.crossPointRange.low - 1);
    uc = min(layers[lid].numCrossPoints() - 1, queryGridBox.crossPointRange.high + 1);
    via.layerIdx = lid;
    for (int t=queryGridBox.trackRange.low-1; t<=queryGridBox.trackRange.high; t++) {
        via.trackIdx = t;
        auto itLo = routedViaMap[lid][t].lower_bound(lc);
        auto itHi = routedViaMap[lid][t].upper_bound(uc);
        for (auto it=itLo; it!=itHi; it++) {
            if (it->second == netIdx) continue;
            via.crossPointIdx = it->first;
            const auto &viaLoc = getLoc(via);
            const auto viaType = getViaType(via);
            neiMetals.push_back(viaType->getShiftedBotMetal(viaLoc));
        }
    }

    if (debug) {
        log() << " ------ getPinLinkVio ------ " << std::endl;
        log() << "   forbid regions for : " << box << std::endl;
        for (const auto &region : regions)
            log() << "      " << region << std::endl;
        log() << "   neiMetals : " << std::endl;
        for (const auto &nm : neiMetals)
            log() << "      " << nm << std::endl;
    }

    return countOvlp(box, regions, neiMetals);
}



void Database::getRoutedBox(GridBoxOnLayer &queryGrid,
                                vector<utils::BoxT<DBU>> &neiMetals,
                                int netIdx) const {
    int lid = queryGrid.layerIdx;
    const auto &layer = layers[lid];
    auto dir = layer.direction;
    utils::PointT<DBU> pu, pv;
    int lc = 0, uc = 0;
    GridPoint via;

    queryGrid.trackRange.low = max(0, queryGrid.trackRange.low);
    queryGrid.crossPointRange.low = max(0, queryGrid.crossPointRange.low);
    queryGrid.trackRange.high = min(layers[lid].numTracks()-1, queryGrid.trackRange.high);
    queryGrid.crossPointRange.high = min(layers[lid].numCrossPoints()-1, queryGrid.crossPointRange.high);

    // lower vias
    if (lid > 0) {
        int low = lid - 1;
        via.layerIdx = low;
        const auto &lowerGrid = getLower(queryGrid);
        // lc = max(0, lowerGrid.crossPointRange.low),
        // uc = min(layers[low].numCrossPoints() - 1, lowerGrid.crossPointRange.high);
        for (int t=lowerGrid.trackRange.low; t<=lowerGrid.trackRange.high; t++) {
            via.trackIdx = t;
            auto itLo = routedViaMap[low][t].lower_bound(lc);
            auto itHi = routedViaMap[low][t].upper_bound(uc);
            for (auto it=itLo; it!=itHi; it++) {
                if (it->second == netIdx) continue;
                via.crossPointIdx = it->first;
                const auto &viaLoc = getLoc(via);
                const auto viaType = getViaType(via);
                neiMetals.push_back(viaType->getShiftedTopMetal(viaLoc));
            }
        }
    }
    via.layerIdx = lid;
    lc = queryGrid.crossPointRange.low;
    uc = queryGrid.crossPointRange.high;
    for (int t=queryGrid.trackRange.low; t<=queryGrid.trackRange.high; t++) {
        // current vias
        via.trackIdx = t;
        auto itLo = routedViaMap[lid][t].lower_bound(lc);
        auto itHi = routedViaMap[lid][t].upper_bound(uc);
        for (auto it=itLo; it!=itHi; it++) {
            if (it->second == netIdx) continue;
            via.crossPointIdx = it->first;
            const auto &viaLoc = getLoc(via);
            const auto viaType = getViaType(via);
            neiMetals.push_back(viaType->getShiftedBotMetal(viaLoc));
        }
        // wires
        auto queryInterval = boost::icl::interval<int>::closed(lc, uc);
        auto intervals = routedWireMap[lid][t].equal_range(queryInterval);
        for (auto it = intervals.first; it != intervals.second; ++it) {
            int usage = it->second.size();
            if (it->second.count(netIdx))
                usage--;
            if (usage > 0) {
                pu[dir] = layer.tracks[t].location;
                pv[dir] = layer.tracks[t].location;
                pu[1-dir] = layer.crossPoints[first(it->first)].location;
                pv[1-dir] = layer.crossPoints[last(it->first)].location;
                neiMetals.push_back(getWireBox(lid, pu, pv));
            }
        }
    }
}

void Database::getFixedBox(const BoxOnLayer &queryBox,
                            vector<utils::BoxT<DBU>> &neiMetals,
                            int netIdx) const {
    boostBox rtreeQueryBox(boostPoint(queryBox.x.low, queryBox.y.low),
                            boostPoint(queryBox.x.high, queryBox.y.high));
    vector<std::pair<boostBox, int>> queryResults;
    fixedMetals[queryBox.layerIdx].query(bgi::intersects(rtreeQueryBox), std::back_inserter(queryResults));

    for (const auto &pair : queryResults) {
        if (pair.second != netIdx) {
            const auto &b = pair.first;
            neiMetals.emplace_back(bg::get<bg::min_corner, 0>(b),
                                    bg::get<bg::min_corner, 1>(b),
                                    bg::get<bg::max_corner, 0>(b),
                                    bg::get<bg::max_corner, 1>(b));
        }
    }
}

bool Database::hasVioRoutedMetalOnTrack(int netIdx, int trackIdx, DBU cl, DBU cu) const {
    const auto &cp = layers[1].crossPoints;
    int lc = getSurroundingCrossPoint(1, cl).low - 1,
        uc = getSurroundingCrossPoint(1, cu).high + 1,
        dir = 1 - layers[1].direction;
    DBU rcl = 0, rcu = 0, botViaSpace = layers[1].maxEolSpace + cutLayers[0].defaultViaType().top[dir].high,
        topViaSpace = layers[1].maxEolSpace + cutLayers[1].defaultViaType().bot[dir].high,
        wireSpace = layers[1].maxEolSpace + layers[1].width * 0.5;

    // wire
    auto queryItvl = boost::icl::interval<int>::closed(lc, uc);
    auto itvls = routedWireMap[1][trackIdx].equal_range(queryItvl);
    for (auto it=itvls.first; it!=itvls.second; it++) {
        for (int id : it->second) {
            if (id != netIdx) {
                rcl = cp[first(it->first)].location;
                rcu = cp[last(it->first)].location;
                if ((cl-rcu) < wireSpace && (rcl-cu) < wireSpace)
                    return true;
            }
        }
    }

    // via
    auto lb = routedViaMap[1][trackIdx].lower_bound(lc);
    auto ub = routedViaMap[1][trackIdx].upper_bound(uc);
    for (auto it=lb; it!=ub; it++) {
        if (it->second != netIdx) {
            rcl = cp[it->first].location;
            if (abs(cl-rcl) < botViaSpace || abs(rcl-cu) < botViaSpace)
                return true;
        }
    }
    lb = routedViaMapUpper[1][trackIdx].lower_bound(lc);
    ub = routedViaMapUpper[1][trackIdx].upper_bound(uc);
    for (auto it=lb; it!=ub; it++) {
        if (it->second != netIdx) {
            rcu = cp[it->first].location;
            if (abs(cl-rcu) < topViaSpace || abs(rcu-cu) < topViaSpace)
                return true;
        }
    }

    return false;
}

// void Database::debugHasVioRoutedMetalOnTrack(int netIdx, int trackIdx, DBU cl, DBU cu) const {
//     const auto &cp = layers[1].crossPoints;
//     int lc = getSurroundingCrossPoint(1, cl).low - 1,
//         uc = getSurroundingCrossPoint(1, cu).high + 1,
//         dir = 1 - layers[1].direction;
//     DBU rcl = 0, rcu = 0, botViaSpace = layers[1].maxEolSpace + cutLayers[0].defaultViaType().top[dir].high,
//         topViaSpace = layers[1].maxEolSpace + cutLayers[1].defaultViaType().bot[dir].high,
//         wireSpace = layers[1].maxEolSpace + layers[1].width * 0.5;
//     printf("    debug vio routed : eol : %ld\n", layers[1].maxEolSpace);
//     printf("    debug vio routed : bot : %ld\n", cutLayers[0].defaultViaType().top[dir].high);
//     printf("    debug vio routed : top : %ld\n", cutLayers[1].defaultViaType().bot[dir].high);

//     // wire
//     auto queryItvl = boost::icl::interval<int>::closed(lc, uc);
//     auto itvls = routedWireMap[1][trackIdx].equal_range(queryItvl);
//     for (auto it=itvls.first; it!=itvls.second; it++) {
//         for (int id : it->second) {
//             if (id != netIdx) {
//                 rcl = cp[first(it->first)].location;
//                 rcu = cp[last(it->first)].location;
//                 if ((cl-rcu) < wireSpace && (rcl-cu) < wireSpace) {
//                     printf("    debug vio routed : wire (%ld, %ld);   thre : %ld\n", rcl, rcu, wireSpace);
//                 }
//             }
//         }
//     }

//     // via
//     auto lb = routedViaMap[1][trackIdx].lower_bound(lc);
//     auto ub = routedViaMap[1][trackIdx].upper_bound(uc);
//     for (auto it=lb; it!=ub; it++) {
//         if (it->second != netIdx) {
//             rcl = cp[it->first].location;
//             if ((cl-rcl) < botViaSpace || (rcl-cu) < botViaSpace) {
//                 printf("    debug vio routed : via (%ld, %ld);   bot : %ld\n", rcl, rcu, botViaSpace);
//             }
//         }
//     }
//     lb = routedViaMapUpper[1][trackIdx].lower_bound(lc);
//     ub = routedViaMapUpper[1][trackIdx].upper_bound(uc);
//     for (auto it=lb; it!=ub; it++) {
//         if (it->second != netIdx) {
//             rcu = cp[it->first].location;
//             if ((cl-rcu) < topViaSpace || (rcu-cu) < topViaSpace) {
//                 printf("    debug vio routed : via (%ld, %ld);   top : %ld\n", rcl, rcu, topViaSpace);
//             }
//         }
//     }
// }


}   // namespace db



// const ViaType * Database::getBestViaTypeForShift(const BoxOnLayer &accessBox) const {
//     const auto &cutLayer = cutLayers[0];
//     int dir = 1 - layers[1].direction;
//     for (const auto &type : cutLayer.allViaTypes) {
//         if (type.topDir == dir &&
//             accessBox.x.range() >= type.bot.x.range() &&
//             accessBox.y.range() >= type.bot.y.range()) {
//             return &type;
//         }
//     }
//     return nullptr;
// }

// int Database::getPinViaVio(const BoxOnLayer& box, int netIdx, bool debug) const {
//     int lid = box.layerIdx;
//     const auto &regions = getAccurateMetalRectForbidRegions(box);
//     utils::BoxT<DBU> queryBox = box;
//     queryBox.x.low -= layers[lid].fixedMetalQueryMargin;
//     queryBox.x.high += layers[lid].fixedMetalQueryMargin;
//     queryBox.y.low -= layers[lid].fixedMetalQueryMargin;
//     queryBox.y.high += layers[lid].fixedMetalQueryMargin;

//     for (const auto& region : regions) {
//         queryBox = queryBox.UnionWith(region);
//     }

//     // fixed metal
//     boostBox rtreeQueryBox(boostPoint(queryBox.x.low, queryBox.y.low), boostPoint(queryBox.x.high, queryBox.y.high));
//     vector<std::pair<boostBox, int>> queryResults;
//     fixedMetals[lid].query(bgi::intersects(rtreeQueryBox), std::back_inserter(queryResults));
//     vector<utils::BoxT<DBU>> neiMetals;
//     for (const auto& queryResult : queryResults) {
//         if (queryResult.second != netIdx) {
//             const auto& b = queryResult.first;
//             neiMetals.emplace_back(bg::get<bg::min_corner, 0>(b),
//                                     bg::get<bg::min_corner, 1>(b),
//                                     bg::get<bg::max_corner, 0>(b),
//                                     bg::get<bg::max_corner, 1>(b));
//         }
//     }

//     // routed metal (via only)
//     GridBoxOnLayer queryGridBox = rangeSearch(BoxOnLayer(lid, queryBox));
//     GridPoint via;
//     int lc=0, uc=0;
//     if (lid > 0) {
//         int low = lid - 1;
//         via.layerIdx = low;
//         const auto &loBox = getLower(queryGridBox);
//         lc = max(0, loBox.crossPointRange.low - 1);
//         uc = min(layers[low].numCrossPoints() - 1, loBox.crossPointRange.high + 1);
//         for (int t=loBox.trackRange.low-1; t<=loBox.trackRange.high; t++) {
//             via.trackIdx = t;
//             auto itLo = routedViaMap[low][t].lower_bound(lc);
//             auto itHi = routedViaMap[low][t].upper_bound(uc);
//             for (auto it=itLo; it!=itHi; it++) {
//                 if (it->second == netIdx) continue;
//                 via.crossPointIdx = it->first;
//                 const auto &viaLoc = getLoc(via);
//                 const auto viaType = getViaType(via);
//                 neiMetals.push_back(viaType->getShiftedTopMetal(viaLoc));
//             }
//         }
//     }
//     lc = max(0, queryGridBox.crossPointRange.low - 1);
//     uc = min(layers[lid].numCrossPoints() - 1, queryGridBox.crossPointRange.high + 1);
//     via.layerIdx = lid;
//     for (int t=queryGridBox.trackRange.low-1; t<=queryGridBox.trackRange.high; t++) {
//         via.trackIdx = t;
//         auto itLo = routedViaMap[lid][t].lower_bound(lc);
//         auto itHi = routedViaMap[lid][t].upper_bound(uc);
//         for (auto it=itLo; it!=itHi; it++) {
//             if (it->second == netIdx) continue;
//             via.crossPointIdx = it->first;
//             const auto &viaLoc = getLoc(via);
//             const auto viaType = getViaType(via);
//             neiMetals.push_back(viaType->getShiftedBotMetal(viaLoc));
//         }
//     }

//     if (debug) {
//         log() << " ------ getPinLinkVio ------ " << std::endl;
//         log() << "   forbid regions for : " << box << std::endl;
//         for (const auto &region : regions)
//             log() << "      " << region << std::endl;
//         log() << "   neiMetals : " << std::endl;
//         for (const auto &nm : neiMetals)
//             log() << "      " << nm << std::endl;
//     }

//     return countOvlp(box, regions, neiMetals);
// }

// int Database::countFixedMetals(const utils::BoxT<DBU> &viaBox, int netIdx) const {
//     DBU space = layers[0].paraRunSpaceForLargerWidth;
//     boostBox rtreeQueryBox(boostPoint(viaBox.x.low-space, viaBox.y.low-space),
//                             boostPoint(viaBox.x.high+space, viaBox.y.high+space));
//     vector<std::pair<boostBox, int>> queryResults;
//     fixedMetals[0].query(bgi::intersects(rtreeQueryBox), std::back_inserter(queryResults));
    
//     int num = 0;
//     for (const auto &pair : queryResults) {
//         if (pair.second != netIdx) num++;
//     }
//     return num;
// }