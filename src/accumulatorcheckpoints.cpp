// Copyright (c) 2018 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "accumulatorcheckpoints.h"
#include "accumulatorcheckpoints.json.h"
#include "util.h"

namespace AccumulatorCheckpoints
{
    std::map<int, Checkpoint> mapCheckpoints;
    int nHeightOldPreV2;
    Checkpoint checkpointOldPreV2;

    UniValue read_json(const std::string& jsondata)
    {
        UniValue v;

        if (!v.read(jsondata) || !v.isArray())
        {
            return UniValue(UniValue::VARR);
        }
        return v.get_array();
    }

    bool GetCheckpointFromUniValueIndex(const UniValue &v, int idx, int& nHeight, Checkpoint& checkpoint)
    {
        const UniValue &val = v[idx];
        const UniValue &o = val.get_obj();

        const UniValue &vHeight = find_value(o, "height");
        if (!vHeight.isNum())
            return false;

        nHeight = vHeight.get_int();
        if (nHeight < 0)
            return false;

        //Checkpoint checkpoint;
        for (auto denom : libzerocoin::zerocoinDenomList) {
            const UniValue& vDenomValue = find_value(o, std::to_string(denom));
            if (!vDenomValue.isStr()) {
                error("LoadCheckpoints() : invalid denomination!? Ignoring till we fix it");
                // return true;
                // continue;
                return false;
            }
            CBigNum bn = 0;
            bn.SetHex(vDenomValue.get_str());
            checkpoint.insert(std::make_pair(denom, bn));
        }

        return true;
    }

    bool LoadCheckpoints(const std::string& strNetwork)
    {
        UniValue v;
        UniValue vold;
        if (strNetwork == "main")
            v = read_json(GetMainCheckpoints());
        else if (strNetwork == "test") {
            v = read_json(GetTestCheckpoints());
            vold = read_json(GetOldTestCheckpoints());
        }
        else if (strNetwork == "regtest")
            v = read_json(GetRegTestCheckpoints());
        else
            return false;

        if (v.empty() || vold.empty())
            return false;

        {
            // const UniValue &valOldLast = vold[vold.size() - 1];
            if(!GetCheckpointFromUniValueIndex(vold, vold.size() - 1, nHeightOldPreV2, checkpointOldPreV2)){
                error("LoadCheckpoints() : (old) invalid json, height, denomination!?");
                return false;
            }
        }

        for (unsigned int idx = 0; idx < v.size(); idx++) {
            int nHeight;
            Checkpoint checkpoint;
            if(!GetCheckpointFromUniValueIndex(v, idx, nHeight, checkpoint)){
                error("LoadCheckpoints() : invalid json, height, denomination!?");
                return false;
            }
            mapCheckpoints.insert(make_pair(nHeight, checkpoint));


            // const UniValue &val = v[idx];
            // const UniValue &o = val.get_obj();

            // const UniValue &vHeight = find_value(o, "height");
            // if (!vHeight.isNum())
            //     return false;

            // int nHeight = vHeight.get_int();
            // if (nHeight < 0)
            //     return false;

            // Checkpoint checkpoint;
            // for (auto denom : libzerocoin::zerocoinDenomList) {
            //     const UniValue& vDenomValue = find_value(o, std::to_string(denom));
            //     if (!vDenomValue.isStr()) {
            //         error("LoadCheckpoints() : invalid denomination!? Ignoring till we fix it");
            //         return true;
            //         // continue;
            //         // return false;
            //     }
            //     CBigNum bn = 0;
            //     bn.SetHex(vDenomValue.get_str());
            //     checkpoint.insert(std::make_pair(denom, bn));
            // }

            // mapCheckpoints.insert(make_pair(nHeight, checkpoint));
        }
        return true;
    }

    Checkpoint GetClosestCheckpoint(const int& nHeight, int& nHeightCheckpoint)
    {
        int lastLowerHeight = 0;
        nHeightCheckpoint = -1;
        for (auto it : mapCheckpoints) {
            //only checkpoints that are less than the height requested (unless height is less than the first checkpoint)
            if (it.first < nHeight) {
                if (it.first > lastLowerHeight)
                    lastLowerHeight = it.first;
                if (nHeightCheckpoint == -1)
                    nHeightCheckpoint = it.first;
                // ZCV2PARAMS: this doesn't work (what was the purpose?), all checks will be fore >V2 so might work mostly
                // but unless we have new checkpoints for larger diffs after V2 it won't.
                if (nHeight - it.first < nHeightCheckpoint)
                    nHeightCheckpoint = it.first;
            }
        }

        if (nHeightCheckpoint != -1) {
            if (lastLowerHeight > nHeightCheckpoint){
                nHeightCheckpoint = lastLowerHeight;
            }
            return mapCheckpoints.at(nHeightCheckpoint);
        }

        return Checkpoint();
    }

    Checkpoint GetOldLatestPreV2Checkpoint(int& nHeightCheckpoint)
    {
        return GetOldLatestPreV2Checkpoint(nHeightOldPreV2 + 10, nHeightCheckpoint);
    }
    Checkpoint GetOldLatestPreV2Checkpoint(int nHeight, int& nHeightCheckpoint)
    {
        // nHeight is only to recheck the heights if we'd want to (i.e. whether it matches the latest-pre-V2)
        nHeightCheckpoint = nHeightOldPreV2;
        return checkpointOldPreV2;
    }
}

