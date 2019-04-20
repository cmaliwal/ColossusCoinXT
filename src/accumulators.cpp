// Copyright (c) 2017-2018 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "accumulators.h"
#include "accumulatormap.h"
#include "chainparams.h"
#include "main.h"
#include "txdb.h"
#include "init.h"
#include "spork.h"
#include "accumulatorcheckpoints.h"
#include "zpivchain.h"

using namespace libzerocoin;

std::map<uint32_t, CBigNum> mapAccumulatorValues;
std::list<uint256> listAccCheckpointsNoDB;

// ZCV2PARAMS: reindexing support (for V1 => V2 checkpoints generation)
// std::map<int, uint256> mapCheckpointsCache;

uint32_t ParseChecksum(uint256 nChecksum, CoinDenomination denomination)
{
    //shift to the beginning bit of this denomination and trim any remaining bits by returning 32 bits only
    int pos = distance(zerocoinDenomList.begin(), find(zerocoinDenomList.begin(), zerocoinDenomList.end(), denomination));
    nChecksum = nChecksum >> (32*((zerocoinDenomList.size() - 1) - pos));
    return nChecksum.Get32();
}

uint32_t GetChecksum(const CBigNum &bnValue)
{
    CDataStream ss(SER_GETHASH, 0);
    ss << bnValue;
    uint256 hash = Hash(ss.begin(), ss.end());

    return hash.Get32();
}

// Find the first occurance of a certain accumulator checksum. Return 0 if not found.
int GetChecksumHeight(uint32_t nChecksum, CoinDenomination denomination)
{
    CBlockIndex* pindex = chainActive[Params().Zerocoin_StartHeight()];
    if (!pindex)
        return 0;

    //Search through blocks to find the checksum
    while (pindex) {
        if (ParseChecksum(pindex->nAccumulatorCheckpoint, denomination) == nChecksum)
            return pindex->nHeight;

        //Skip forward in groups of 10 blocks since checkpoints only change every 10 blocks
        if (pindex->nHeight % 10 == 0) {
            if (pindex->nHeight + 10 > chainActive.Height())
                return 0;
            pindex = chainActive[pindex->nHeight + 10];
            continue;
        }

        pindex = chainActive.Next(pindex);
    }

    return 0;
}

bool GetAccumulatorValueFromChecksum(uint32_t nChecksum, bool fMemoryOnly, CBigNum& bnAccValue)
{
    if (mapAccumulatorValues.count(nChecksum)) {
        bnAccValue = mapAccumulatorValues.at(nChecksum);
        return true;
    }

    if (fMemoryOnly)
        return false;

    if (!zerocoinDB->ReadAccumulatorValue(nChecksum, bnAccValue)) {
        bnAccValue = 0;
    }

    return true;
}

// ZCV2PARAMS: reindexing support (for V1 => V2 checkpoints generation) (only used from rpc)
bool GetAccumulatorValueFromDB(int nHeight, CoinDenomination denom, CBigNum& bnAccValue)
{
    CBlockIndex* pindex = chainActive[nHeight];
    uint256 nCheckpoint = pindex->nAccumulatorCheckpoint;

    // if (mapCheckpointsCache.count(nHeight)) {
    //     nCheckpoint = mapCheckpointsCache.at(nHeight);
    // }

    uint32_t nChecksum = ParseChecksum(nCheckpoint, denom);
    return GetAccumulatorValueFromChecksum(nChecksum, false, bnAccValue);
}

bool GetAccumulatorValueFromDB(uint256 nCheckpoint, CoinDenomination denom, CBigNum& bnAccValue)
{
    uint32_t nChecksum = ParseChecksum(nCheckpoint, denom);
    return GetAccumulatorValueFromChecksum(nChecksum, false, bnAccValue);
}

void AddAccumulatorChecksum(const uint32_t nChecksum, const CBigNum &bnValue)
{
    //Since accumulators are switching at v2, stop databasing v1 because its useless. Only focus on v2.
    if (chainActive.Height() >= Params().Zerocoin_Block_V2_Start()) {
        zerocoinDB->WriteAccumulatorValue(nChecksum, bnValue);
        mapAccumulatorValues.insert(make_pair(nChecksum, bnValue));
    }
}

void AddAccumulatorChecksum(const uint32_t nChecksum, const CBigNum &bnValue, bool fMemoryOnly)
{
    if(!fMemoryOnly)
        zerocoinDB->WriteAccumulatorValue(nChecksum, bnValue);
    mapAccumulatorValues.insert(make_pair(nChecksum, bnValue));
}

void DatabaseChecksums(AccumulatorMap& mapAccumulators)
{
    uint256 nCheckpoint = 0;
    for (auto& denom : zerocoinDenomList) {
        CBigNum bnValue = mapAccumulators.GetValue(denom);
        uint32_t nCheckSum = GetChecksum(bnValue);
        AddAccumulatorChecksum(nCheckSum, bnValue);
        nCheckpoint = nCheckpoint << 32 | nCheckSum;
    }
}

bool EraseChecksum(uint32_t nChecksum)
{
    //erase from both memory and database
    mapAccumulatorValues.erase(nChecksum);
    return zerocoinDB->EraseAccumulatorValue(nChecksum);
}

bool EraseAccumulatorValues(const uint256& nCheckpointErase, const uint256& nCheckpointPrevious)
{
    for (auto& denomination : zerocoinDenomList) {
        uint32_t nChecksumErase = ParseChecksum(nCheckpointErase, denomination);
        uint32_t nChecksumPrevious = ParseChecksum(nCheckpointPrevious, denomination);

        //if the previous checksum is the same, then it should remain in the database and map
        if(nChecksumErase == nChecksumPrevious)
            continue;

        if (!EraseChecksum(nChecksumErase))
            return false;
    }

    return true;
}

bool LoadAccumulatorValuesFromDB(const uint256 nCheckpoint)
{
    for (auto& denomination : zerocoinDenomList) {
        uint32_t nChecksum = ParseChecksum(nCheckpoint, denomination);

        //if read is not successful then we are not in a state to verify zerocoin transactions
        CBigNum bnValue;
        if (!zerocoinDB->ReadAccumulatorValue(nChecksum, bnValue)) {
            LogPrint("zero","%s : Missing databased value for checksum %d\n", __func__, nChecksum);
            if (!count(listAccCheckpointsNoDB.begin(), listAccCheckpointsNoDB.end(), nCheckpoint))
                listAccCheckpointsNoDB.push_back(nCheckpoint);
            return false;
        }
        mapAccumulatorValues.insert(make_pair(nChecksum, bnValue));
    }
    return true;
}

//Erase accumulator checkpoints for a certain block range
bool EraseCheckpoints(int nStartHeight, int nEndHeight)
{
    if (chainActive.Height() < nStartHeight)
        return false;

    nEndHeight = min(chainActive.Height(), nEndHeight);

    CBlockIndex* pindex = chainActive[nStartHeight];
    uint256 nCheckpointPrev = pindex->pprev->nAccumulatorCheckpoint;

    //Keep a list of checkpoints from the previous block so that we don't delete them
    list<uint32_t> listCheckpointsPrev;
    for (auto denom : zerocoinDenomList)
        listCheckpointsPrev.emplace_back(ParseChecksum(nCheckpointPrev, denom));

    while (true) {
        uint256 nCheckpointDelete = pindex->nAccumulatorCheckpoint;

        for (auto denom : zerocoinDenomList) {
            uint32_t nChecksumDelete = ParseChecksum(nCheckpointDelete, denom);
            if (count(listCheckpointsPrev.begin(), listCheckpointsPrev.end(), nCheckpointDelete))
                continue;
            EraseChecksum(nChecksumDelete);
        }
        LogPrintf("%s : erasing checksums for block %d\n", __func__, pindex->nHeight);

        if (pindex->nHeight + 1 <= nEndHeight)
            pindex = chainActive.Next(pindex);
        else
            break;
    }

    return true;
}

// ZCV2CHECKS: quick fix, we need to somehow recognize last old checkpoint (already written into blocks) and 'match' it
// to the 'new' (based on the new big-num) last checkpoint (as loaded from json). Once blocks (post-V2) get going we won't
// need this (but will be needed for anyone syncing from the start).
uint256 GetFlattenCheckpoint(AccumulatorCheckpoints::Checkpoint checkpoint)
{
    uint256 nCheckpoint = 0;
    for (auto it : checkpoint) {
        libzerocoin::CoinDenomination denom = it.first;
        CBigNum bnValue = it.second;
        uint32_t nCheckSum = GetChecksum(bnValue);
        nCheckpoint = nCheckpoint << 32 | nCheckSum;
    }
    return nCheckpoint;
    // for (auto& denom : zerocoinDenomList) {
    //     CBigNum bnValue = mapAccumulators.GetValue(denom);
    //     uint32_t nCheckSum = GetChecksum(bnValue);
    //     AddAccumulatorChecksum(nCheckSum, bnValue);
    //     nCheckpoint = nCheckpoint << 32 | nCheckSum;
    // }
    // return nCheckpoint;
}

// ZCV2CHECKS: quick fix
bool IsLatestPreV2Checkpoint(uint256 nCheckpoint)
{
    int nHeightCheckpoint;
    AccumulatorCheckpoints::Checkpoint checkpoint = 
        AccumulatorCheckpoints::GetOldLatestPreV2Checkpoint(nHeightCheckpoint);
    uint256 nCheckpointLatest = GetFlattenCheckpoint(checkpoint);
    return nCheckpointLatest == nCheckpoint;
}

bool InitializeAccumulators(const int nHeight, int& nHeightCheckpoint, AccumulatorMap& mapAccumulators)
{
    if (nHeight < Params().Zerocoin_StartHeight())
        return error("%s: height is below zerocoin activated", __func__);

    //On a specific block, a recalculation of the accumulators will be forced
    if (nHeight == Params().Zerocoin_Block_RecalculateAccumulators() && Params().NetworkID() != CBaseChainParams::REGTEST) {
        mapAccumulators.Reset();
        if (!mapAccumulators.LoadFlat(chainActive[Params().Zerocoin_Block_LastGoodCheckpoint()]->nAccumulatorCheckpoint))
            return error("%s: failed to reset to previous checkpoint when recalculating accumulators", __func__);

        // Erase the checkpoints from the period of time that bad mints were being made
        if (!EraseCheckpoints(Params().Zerocoin_Block_LastGoodCheckpoint() + 1, nHeight))
            return error("%s : failed to erase Checkpoints while recalculating checkpoints", __func__);

        nHeightCheckpoint = Params().Zerocoin_Block_LastGoodCheckpoint();
        return true;
    }

    if (nHeight >= Params().Zerocoin_Block_V2_Start()) {
        //after v2_start, accumulators need to use v2 params
        mapAccumulators.Reset(Params().Zerocoin_Params(false));

        // 20 after v2 start is when the new checkpoints will be in the block, so don't need to load hard checkpoints
        if (nHeight <= Params().Zerocoin_Block_V2_Start() + 20 && Params().NetworkID() != CBaseChainParams::REGTEST) {
            //Load hard coded checkpointed value
            AccumulatorCheckpoints::Checkpoint checkpoint = AccumulatorCheckpoints::GetClosestCheckpoint(nHeight,
                                                                                                         nHeightCheckpoint);
            if (nHeightCheckpoint < 0)
                return error("%s: failed to load hard-checkpoint for block %s", __func__, nHeight);

            mapAccumulators.Load(checkpoint);
            return true;
        }
    }

    //Use the previous block's checkpoint to initialize the accumulator's state
    uint256 nCheckpointPrev = chainActive[nHeight - 1]->nAccumulatorCheckpoint;

    // ZCV2PARAMS: using cache, temp fix (the cache would normally be empty so it can stay)
    // if (mapCheckpointsCache.count(nHeight - 1)) {
    //     nCheckpointPrev = mapCheckpointsCache.at(nHeight - 1);
    // }

    if (nCheckpointPrev == 0)
        mapAccumulators.Reset();
    else if (!mapAccumulators.LoadFlat(nCheckpointPrev)) {
        // ZCV2CHECKS: quick fix
        if (IsLatestPreV2Checkpoint(nCheckpointPrev)) {
            auto checkpoint = AccumulatorCheckpoints::GetClosestCheckpoint(nHeight, nHeightCheckpoint);
            if (nHeightCheckpoint < 0)
                return error("%s: failed to load/match old hard-checkpoint for block %s", __func__, nHeight);
            mapAccumulators.Load(checkpoint);
        } else {
            return error("%s: failed to reset to previous checkpoint", __func__);
        }
    }

    nHeightCheckpoint = nHeight;
    return true;
}

// ZCV2PARAMS: 
// bool CalculateAccumulatorCheckpointNCache(int nHeight, uint256& nCheckpoint, AccumulatorMap& mapAccumulators)
// {
//     auto result = CalculateAccumulatorCheckpoint(nHeight, nCheckpoint, mapAccumulators);
//     if (nCheckpoint > 0)
//         mapCheckpointsCache.insert(make_pair(nHeight, nCheckpoint));
//     return result;
// }
bool CacheCheckpoint(int nHeight, uint256& nCheckpoint)
{
    // if (mapCheckpointsCache.count(nHeight)) {
    //     // nCheckpointPrev = mapCheckpointsCache.at(nHeight - 1);
    //     return true;
    // }
    // if (nCheckpoint > 0)
    //     mapCheckpointsCache.insert(make_pair(nHeight, nCheckpoint));
    return true;
}

//Get checkpoint value for a specific block height
bool CalculateAccumulatorCheckpoint(int nHeight, uint256& nCheckpoint, AccumulatorMap& mapAccumulators)
{
    // only called from reindexing (main), CreateNewBlock (miner) & ValidateAccumulatorCheckpoint/ConnectBlock (main)
    // ZCV2PARAMS: this (v2 start check) is important, it renders checkpoints to zero for anything pre-V2, 
    // so it shouldn't be called for anything older than that basically, just turning this off temporarily
    // if (nHeight < Params().Zerocoin_StartHeight()) {
    if (nHeight < Params().Zerocoin_Block_V2_Start()) {
        nCheckpoint = 0;
        return true;
    }

    //the checkpoint is updated every ten blocks, return current active checkpoint if not update block
    if (nHeight % 10 != 0) {
        nCheckpoint = chainActive[nHeight - 1]->nAccumulatorCheckpoint;
        return true;
    }

    //set the accumulators to last checkpoint value
    int nHeightCheckpoint;
    mapAccumulators.Reset();
    if (!InitializeAccumulators(nHeight, nHeightCheckpoint, mapAccumulators))
        return error("%s: failed to initialize accumulators", __func__);

    //Whether this should filter out invalid/fraudulent outpoints
    bool fFilterInvalid = nHeight >= Params().Zerocoin_Block_RecalculateAccumulators();

    //Accumulate all coins over the last ten blocks that havent been accumulated (height - 20 through height - 11)
    int nTotalMintsFound = 0;
    CBlockIndex *pindex = chainActive[nHeightCheckpoint - 20];

    while (pindex->nHeight < nHeight - 10) {
        // checking whether we should stop this process due to a shutdown request
        if (ShutdownRequested())
            return false;

        //make sure this block is eligible for accumulation
        if (pindex->nHeight < Params().Zerocoin_StartHeight()) {
            pindex = chainActive[pindex->nHeight + 1];
            continue;
        }

        //grab mints from this block
        CBlock block;
        if(!ReadBlockFromDisk(block, pindex))
            return error("%s: failed to read block from disk", __func__);

        std::list<PublicCoin> listPubcoins;
        if (!BlockToPubcoinList(block, listPubcoins, fFilterInvalid))
            return error("%s: failed to get zerocoin mintlist from block %d", __func__, pindex->nHeight);

        nTotalMintsFound += listPubcoins.size();
        LogPrint("zero", "%s found %d mints\n", __func__, listPubcoins.size());

        //add the pubcoins to accumulator
        for (const PublicCoin& pubcoin : listPubcoins) {
            if(!mapAccumulators.Accumulate(pubcoin, true))
                return error("%s: failed to add pubcoin to accumulator at height %d", __func__, pindex->nHeight);
        }
        pindex = chainActive.Next(pindex);
    }

    // if there were no new mints found, the accumulator checkpoint will be the same as the last checkpoint
    if (nTotalMintsFound == 0)
        nCheckpoint = chainActive[nHeight - 1]->nAccumulatorCheckpoint;
    else
        nCheckpoint = mapAccumulators.GetCheckpoint();

    LogPrint("zero", "%s checkpoint=%s\n", __func__, nCheckpoint.GetHex());
    return true;
}

bool InvalidCheckpointRange(int nHeight)
{
    return nHeight > Params().Zerocoin_Block_LastGoodCheckpoint() && nHeight < Params().Zerocoin_Block_RecalculateAccumulators();
}

bool ValidateAccumulatorCheckpoint(const CBlock& block, CBlockIndex* pindex, AccumulatorMap& mapAccumulators)
{
    // ZCV2PARAMS: this (v2 start check) is important, calculating/validation is off pre-V2, as the calc results in 0
    // and CalculateAccumulatorCheckpoint is only called during manual reindex (ReindexAccumulators) or in miner/newblock 

    // ZCFIXTODO: 
    // if (!fVerifyingBlocks && pindex->nHeight >= Params().Zerocoin_StartHeight() && pindex->nHeight % 10 == 0) {
    //V1 accumulators are completely phased out by the time this code hits the public and begins generating new checkpoints
    //It is VERY IMPORTANT that when this is being run and height < v2_start, then zPIV need to be disabled at the same time!!
    if (pindex->nHeight < Params().Zerocoin_Block_V2_Start() || fVerifyingBlocks)
        return true;

    if (pindex->nHeight % 10 == 0) {
        uint256 nCheckpointCalculated = 0;

        if (!CalculateAccumulatorCheckpoint(pindex->nHeight, nCheckpointCalculated, mapAccumulators))
            return error("%s : failed to calculate accumulator checkpoint", __func__);

        if (nCheckpointCalculated != block.nAccumulatorCheckpoint) {
            LogPrintf("%s: block=%d calculated: %s\n block: %s\n", __func__, pindex->nHeight, nCheckpointCalculated.GetHex(), block.nAccumulatorCheckpoint.GetHex());
            return error("%s : accumulator does not match calculated value", __func__);
        }

        return true;
    }

    if (block.nAccumulatorCheckpoint != pindex->pprev->nAccumulatorCheckpoint)
        return error("%s : new accumulator checkpoint generated on a block that is not multiple of 10", __func__);

    return true;
}

void RandomizeSecurityLevel(int& nSecurityLevel)
{
    //security level: this is an important prevention of tracing the coins via timing. Security level represents how many checkpoints
    //of accumulated coins are added *beyond* the checkpoint that the mint being spent was added too. If each spend added the exact same
    //amounts of checkpoints after the mint was accumulated, then you could know the range of blocks that the mint originated from.
    if (nSecurityLevel < 100) {
        //add some randomness to the user's selection so that it is not always the same
        nSecurityLevel += CBigNum::randBignum(10).getint();

        //security level 100 represents adding all available coins that have been accumulated - user did not select this
        if (nSecurityLevel >= 100)
            nSecurityLevel = 99;
    }
}

//Compute how many coins were added to an accumulator up to the end height
int ComputeAccumulatedCoins(int nHeightEnd, libzerocoin::CoinDenomination denom)
{
    CBlockIndex* pindex = chainActive[GetZerocoinStartHeight()];
    int n = 0;
    while (pindex->nHeight < nHeightEnd) {
        n += count(pindex->vMintDenominationsInBlock.begin(), pindex->vMintDenominationsInBlock.end(), denom);
        pindex = chainActive.Next(pindex);
    }

    return n;
}

list<PublicCoin> GetPubcoinFromBlock(const CBlockIndex* pindex){
    //grab mints from this block
    CBlock block;
    if(!ReadBlockFromDisk(block, pindex))
        throw GetPubcoinException("GetPubcoinFromBlock: failed to read block from disk while adding pubcoins to witness");
    list<libzerocoin::PublicCoin> listPubcoins;
    if(!BlockToPubcoinList(block, listPubcoins, true))
        throw GetPubcoinException("GetPubcoinFromBlock: failed to get zerocoin mintlist from block "+std::to_string(pindex->nHeight)+"\n");
    return listPubcoins;
}

int AddBlockMintsToAccumulator(const CoinDenomination den, const CBloomFilter filter, const CBlockIndex* pindex,
                               libzerocoin::Accumulator* accumulator, bool isWitness, list<CBigNum>& notAddedCoins)
{
    // if this block contains mints of the denomination that is being spent, then add them to the witness
    int nMintsAdded = 0;
    if (pindex->MintedDenomination(den)) {
        //grab mints from this block
        list<PublicCoin> listPubcoins = GetPubcoinFromBlock(pindex);

        //add the mints to the witness
        for (const PublicCoin& pubcoin : listPubcoins) {
            if (pubcoin.getDenomination() != den) {
                continue;
            }

            bool filterContains = filter.contains(pubcoin.getValue().getvch());

            if (isWitness && filterContains) {
                notAddedCoins.emplace_back(pubcoin.getValue());
                continue;
            }

            accumulator->increment(pubcoin.getValue());
            ++nMintsAdded;
        }
    }

    return nMintsAdded;
}

int AddBlockMintsToAccumulator(const libzerocoin::PublicCoin& coin, const int nHeightMintAdded, const CBlockIndex* pindex,
                           libzerocoin::Accumulator* accumulator, bool isWitness)
{
    // if this block contains mints of the denomination that is being spent, then add them to the witness
    int nMintsAdded = 0;
    if (pindex->MintedDenomination(coin.getDenomination())) {
        //grab mints from this block
        list<PublicCoin> listPubcoins = GetPubcoinFromBlock(pindex);

        //add the mints to the witness
        for (const PublicCoin& pubcoin : listPubcoins) {
            if (pubcoin.getDenomination() != coin.getDenomination())
                continue;

            if (isWitness && pindex->nHeight == nHeightMintAdded && pubcoin.getValue() == coin.getValue())
                continue;

            accumulator->increment(pubcoin.getValue());
            ++nMintsAdded;
        }
    }

    return nMintsAdded;
}

// only used for new tx-s
bool GetAccumulatorValue(int& nHeight, const libzerocoin::CoinDenomination denom, CBigNum& bnAccValue)
{
    if (nHeight > chainActive.Height())
        return error("%s: height %d is more than active chain height", __func__, nHeight);

    //Every situation except for about 20 blocks should use this method
    uint256 nCheckpointBeforeMint = chainActive[nHeight]->nAccumulatorCheckpoint;
    if (nHeight > Params().Zerocoin_Block_V2_Start() + 20) {
        return GetAccumulatorValueFromDB(nCheckpointBeforeMint, denom, bnAccValue);
    }

    // ZCV2PARAMS: this is important to sync the accumulators from old modulus (wrong hex) to new modulus (decimal->hex).
    // for the time being, till we accumulate enough, don't rely on the prevous blocks' nAccumulatorCheckpoint-s,
    // as those would be wrong (old-style, 617 in length, as opposed to the new 512 ones).
    
    int nHeightCheckpoint = 0;
    AccumulatorCheckpoints::Checkpoint checkpoint = AccumulatorCheckpoints::GetClosestCheckpoint(nHeight, nHeightCheckpoint);
    if (nHeightCheckpoint < 0) {
        //Start at the first zerocoin
        libzerocoin::Accumulator accumulator(Params().Zerocoin_Params(false), denom);
        bnAccValue = accumulator.getValue();
        nHeight = Params().Zerocoin_StartHeight() + 10;
        return true;
    }

    nHeight = nHeightCheckpoint;
    bnAccValue = checkpoint.at(denom);

    return true;
}

/**
 * TODO: Why we are locking the wallet in this way?
 * @return
 */
bool LockMethod(){
    int nLockAttempts = 0;
    while (nLockAttempts < 100) {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain) {
            MilliSleep(50);
            nLockAttempts++;
            continue;
        }
        break;
    }
    if (nLockAttempts == 100)
        return error("%s: could not get lock on cs_main", __func__);
    // locked
    return true;
}

std::list<CBlockIndex*> calculateAccumulatedBlocksFor(
        int startHeight,
        int nHeightStop,
        CBlockIndex *pindex,
        int &nCheckpointsAdded,
        CBigNum &bnAccValue,
        libzerocoin::Accumulator &accumulator,
        libzerocoin::CoinDenomination den,
        int nSecurityLevel
){

    std::list<CBlockIndex*> blocksToInclude;
    int amountOfScannedBlocks = 0;
    bool fDoubleCounted = false;
    while (pindex) {
        if (pindex->nHeight != startHeight && pindex->pprev->nAccumulatorCheckpoint != pindex->nAccumulatorCheckpoint)
            ++nCheckpointsAdded;

        //If the security level is satisfied, or the stop height is reached, then initialize the accumulator from here
        bool fSecurityLevelSatisfied = (nSecurityLevel != 100 && nCheckpointsAdded >= nSecurityLevel);
        if (pindex->nHeight >= nHeightStop || fSecurityLevelSatisfied) {
            //If this height is within the invalid range (when fraudulent coins were being minted), then continue past this range
            if(InvalidCheckpointRange(pindex->nHeight))
                continue;

            bnAccValue = 0;
            uint256 nCheckpointSpend = chainActive[pindex->nHeight + 10]->nAccumulatorCheckpoint;
            if (!GetAccumulatorValueFromDB(nCheckpointSpend, den, bnAccValue) || bnAccValue == 0) {
                throw new ChecksumInDbNotFoundException(
                        "calculateAccumulatedBlocksFor : failed to find checksum in database for accumulator");
            }
            accumulator.setValue(bnAccValue);
            break;
        }

        // Add it
        blocksToInclude.push_back(pindex);

        // 10 blocks were accumulated twice when zPIV v2 was activated
        if (pindex->nHeight == 1050010 && !fDoubleCounted) {
            pindex = chainActive[1050000];
            fDoubleCounted = true;
            continue;
        }

        amountOfScannedBlocks++;
        pindex = chainActive.Next(pindex);
    }

    return blocksToInclude;
}

// used in lightzpivthread.cpp, we don't have that yet
bool CalculateAccumulatorWitnessFor(
        const ZerocoinParams* params,
        int startHeight,
        int maxCalulationRange,
        CoinDenomination den,
        const CBloomFilter& filter,
        Accumulator& accumulator,
        AccumulatorWitness& witness,
        int nSecurityLevel,
        int& nMintsAdded,
        string& strError,
        list<CBigNum>& ret,
        int &heightStop
){
    // Lock
    if (!LockMethod()) return false;

    try {
        // Dummy coin init
        PublicCoin temp(params, 0, den);
        // Dummy Acc init
        Accumulator testingAcc(params, den);

        //get the checkpoint added at the next multiple of 10
        int nHeightCheckpoint = startHeight + (10 - (startHeight % 10));

        // Get the base accumulator
        //CBigNum bnAccValue = accumulator.getValue();
        // TODO: This needs to be changed to the partial witness calculation on the next version.
        CBigNum bnAccValue = 0;
        if (GetAccumulatorValue(nHeightCheckpoint, den, bnAccValue)) {
            accumulator.setValue(bnAccValue);
            witness.resetValue(accumulator, temp);
        }

        // Add the pubcoins from the blockchain up to the next checksum starting from the block
        CBlockIndex *pindex = chainActive[nHeightCheckpoint -10];
        int nChainHeight = chainActive.Height();
        int nHeightStop = nChainHeight % 10;
        nHeightStop = nChainHeight - nHeightStop - 20; // at least two checkpoints deep

        if (nHeightStop - startHeight > maxCalulationRange) {
            int stop = (startHeight + maxCalulationRange);
            int nHeightStop = stop % 10;
            nHeightStop = stop - nHeightStop - 20;
        }
        heightStop = nHeightStop;

        // Iterate through the chain and calculate the witness
        int nCheckpointsAdded = 0;
        nMintsAdded = 0;
        RandomizeSecurityLevel(nSecurityLevel); //make security level not always the same and predictable
        // Starts on top of the witness that the node sent
        libzerocoin::Accumulator witnessAccumulator(params, den, witness.getValue());

        std::list<CBlockIndex*> blocksToInclude = calculateAccumulatedBlocksFor(
                startHeight,
                nHeightStop,
                pindex,
                nCheckpointsAdded,
                bnAccValue,
                accumulator,
                den,
                nSecurityLevel
        );

        // Now accumulate the coins
        for (const CBlockIndex *blockIndex : blocksToInclude) {
            nMintsAdded += AddBlockMintsToAccumulator(den, filter, blockIndex, &witnessAccumulator, true, ret);
        }

        // A certain amount of accumulated coins are required
        if (nMintsAdded < Params().Zerocoin_RequiredAccumulation()) {
            strError = _(strprintf("Less than %d mints added, unable to create spend",
                                   Params().Zerocoin_RequiredAccumulation()).c_str());
            throw NotEnoughMintsException(strError);
        }

        witness.resetValue(witnessAccumulator, temp);

        // calculate how many mints of this denomination existed in the accumulator we initialized
        nMintsAdded += ComputeAccumulatedCoins(startHeight, den);
        LogPrint("zero", "%s : %d mints added to witness\n", __func__, nMintsAdded);

        return true;

    } catch (ChecksumInDbNotFoundException e) {
        return error("%s: ChecksumInDbNotFoundException: %s", __func__, e.message);
    } catch (GetPubcoinException e) {
        return error("%s: GetPubcoinException: %s", __func__, e.message);
    }
}


int SearchMintHeightOf(CBigNum value){
    uint256 txid;
    if (!zerocoinDB->ReadCoinMint(value, txid))
        throw searchMintHeightException("searchForMintHeightOf:: failed to read mint from db");

    CTransaction txMinted;
    uint256 hashBlock;
    if (!GetTransaction(txid, txMinted, hashBlock))
        throw searchMintHeightException("searchForMintHeightOf:: failed to read tx");

    int nHeightTest;
    if (!IsTransactionInChain(txid, nHeightTest))
        throw searchMintHeightException("searchForMintHeightOf:: mint tx "+ txid.GetHex() +" is not in chain");

    return mapBlockIndex[hashBlock]->nHeight;
}

// only used for new tx-s
bool GenerateAccumulatorWitness(
        const PublicCoin &coin,
        Accumulator& accumulator,
        AccumulatorWitness& witness,
        int nSecurityLevel,
        int& nMintsAdded,
        string& strError,
        CBlockIndex* pindexCheckpoint)
{
    try {
        // Lock
        LogPrint("zero", "%s: generating\n", __func__);
        if (!LockMethod()) return false;
        LogPrint("zero", "%s: after lock\n", __func__);

        int nHeightMintAdded = SearchMintHeightOf(coin.getValue());
        //get the checkpoint added at the next multiple of 10
        int nHeightCheckpoint = nHeightMintAdded + (10 - (nHeightMintAdded % 10));
        //the height to start accumulating coins to add to witness
        int nAccStartHeight = nHeightMintAdded - (nHeightMintAdded % 10);

        //Get the accumulator that is right before the cluster of blocks containing our mint was added to the accumulator
        CBigNum bnAccValue = 0;
        if (GetAccumulatorValue(nHeightCheckpoint, coin.getDenomination(), bnAccValue)) {
            if(!bnAccValue && Params().NetworkID() == CBaseChainParams::REGTEST){
                accumulator.setInitialValue();
                witness.resetValue(accumulator, coin);
            }else {
                accumulator.setValue(bnAccValue);
                witness.resetValue(accumulator, coin);
            }
        }

        //add the pubcoins from the blockchain up to the next checksum starting from the block
        CBlockIndex *pindex = chainActive[nHeightCheckpoint - 10];
        int nChainHeight = chainActive.Height();
        int nHeightStop = nChainHeight % 10;
        nHeightStop = nChainHeight - nHeightStop - 20; // at least two checkpoints deep

        //If looking for a specific checkpoint
        if (pindexCheckpoint)
            nHeightStop = pindexCheckpoint->nHeight - 10;

        //Iterate through the chain and calculate the witness
        int nCheckpointsAdded = 0;
        nMintsAdded = 0;
        RandomizeSecurityLevel(nSecurityLevel); //make security level not always the same and predictable
        libzerocoin::Accumulator witnessAccumulator = accumulator;

        std::list<CBlockIndex*> blocksToInclude = calculateAccumulatedBlocksFor(
                nAccStartHeight,
                nHeightStop,
                pindex,
                nCheckpointsAdded,
                bnAccValue,
                accumulator,
                coin.getDenomination(),
                nSecurityLevel
        );

        // Now accumulate the coins
        for (const CBlockIndex *blockIndex : blocksToInclude) {
            nMintsAdded += AddBlockMintsToAccumulator(coin, nHeightMintAdded, blockIndex, &witnessAccumulator, true);
        }

        witness.resetValue(witnessAccumulator, coin);
        if (!witness.VerifyWitness(accumulator, coin))
            return error("%s: failed to verify witness", __func__);

        // A certain amount of accumulated coins are required
        if (nMintsAdded < Params().Zerocoin_RequiredAccumulation()) {
            strError = _(strprintf("Less than %d mints added, unable to create spend",
                                   Params().Zerocoin_RequiredAccumulation()).c_str());
            return error("%s : %s", __func__, strError);
        }

        // calculate how many mints of this denomination existed in the accumulator we initialized
        nMintsAdded += ComputeAccumulatedCoins(nAccStartHeight, coin.getDenomination());
        LogPrint("zero", "%s : %d mints added to witness\n", __func__, nMintsAdded);

        return true;

    // TODO: I know that could merge all of this exception but maybe it's not really good.. think if we should have a different treatment for each one
    } catch (searchMintHeightException e) {
        return error("%s: searchMintHeightException: %s", __func__, e.message);
    } catch (ChecksumInDbNotFoundException e) {
        return error("%s: ChecksumInDbNotFoundException: %s", __func__, e.message);
    } catch (GetPubcoinException e) {
        return error("%s: GetPubcoinException: %s", __func__, e.message);
    }
}

bool GenerateAccumulatorWitnessOld(
        const PublicCoin &coin, 
        Accumulator& accumulator, 
        AccumulatorWitness& witness, 
        int nSecurityLevel, 
        int& nMintsAdded, 
        string& strError)
{
    uint256 txid;
    if (!zerocoinDB->ReadCoinMint(coin.getValue(), txid)) {
        LogPrint("zero","%s failed to read mint from db\n", __func__);
        return false;
    }

    CTransaction txMinted;
    uint256 hashBlock;
    if (!GetTransaction(txid, txMinted, hashBlock)) {
        LogPrint("zero","%s failed to read tx\n", __func__);
        return false;
    }

    int nHeightMintAdded= mapBlockIndex[hashBlock]->nHeight;
    uint256 nCheckpointBeforeMint = 0;
    CBlockIndex* pindex = chainActive[nHeightMintAdded];
    int nChanges = 0;

    //find the checksum when this was added to the accumulator officially, which will be two checksum changes later
    //reminder that checksums are generated when the block height is a multiple of 10
    while (pindex->nHeight < chainActive.Tip()->nHeight - 1) {
        if (pindex->nHeight == nHeightMintAdded) {
            pindex = chainActive[pindex->nHeight + 1];
            continue;
        }

        //check if the next checksum was generated
        if (pindex->nHeight % 10 == 0) {
            nChanges++;

            if (nChanges == 1) {
                nCheckpointBeforeMint = pindex->nAccumulatorCheckpoint;
                break;
            }
        }
        pindex = chainActive.Next(pindex);
    }

    //the height to start accumulating coins to add to witness
    int nAccStartHeight = nHeightMintAdded - (nHeightMintAdded % 10);

    //If the checkpoint is from the recalculated checkpoint period, then adjust it
    int nHeight_LastGoodCheckpoint = Params().Zerocoin_Block_LastGoodCheckpoint();
    int nHeight_Recalculate = Params().Zerocoin_Block_RecalculateAccumulators();
    if (pindex->nHeight < nHeight_Recalculate - 10 && pindex->nHeight > nHeight_LastGoodCheckpoint) {
        //The checkpoint before the mint will be the last good checkpoint
        nCheckpointBeforeMint = chainActive[nHeight_LastGoodCheckpoint]->nAccumulatorCheckpoint;
        nAccStartHeight = nHeight_LastGoodCheckpoint - 10;
    }

    //Get the accumulator that is right before the cluster of blocks containing our mint was added to the accumulator
    CBigNum bnAccValue = 0;
    if (GetAccumulatorValueFromDB(nCheckpointBeforeMint, coin.getDenomination(), bnAccValue)) {
        if (bnAccValue > 0) {
            accumulator.setValue(bnAccValue);
            witness.resetValue(accumulator, coin);
        }
    }

    //security level: this is an important prevention of tracing the coins via timing. Security level represents how many checkpoints
    //of accumulated coins are added *beyond* the checkpoint that the mint being spent was added too. If each spend added the exact same
    //amounts of checkpoints after the mint was accumulated, then you could know the range of blocks that the mint originated from.
    if (nSecurityLevel < 100) {
        //add some randomness to the user's selection so that it is not always the same
        nSecurityLevel += CBigNum::randBignum(10).getint();

        //security level 100 represents adding all available coins that have been accumulated - user did not select this
        if (nSecurityLevel >= 100)
            nSecurityLevel = 99;
    }

    //add the pubcoins (zerocoinmints that have been published to the chain) up to the next checksum starting from the block
    pindex = chainActive[nAccStartHeight];
    int nChainHeight = chainActive.Height();
    int nHeightStop = nChainHeight % 10;
    nHeightStop = nChainHeight - nHeightStop - 20; // at least two checkpoints deep
    int nCheckpointsAdded = 0;
    nMintsAdded = 0;
    while (pindex->nHeight < nHeightStop + 1) {
        if (pindex->nHeight != nAccStartHeight && pindex->pprev->nAccumulatorCheckpoint != pindex->nAccumulatorCheckpoint)
            ++nCheckpointsAdded;

        //if a new checkpoint was generated on this block, and we have added the specified amount of checkpointed accumulators,
        //then initialize the accumulator at this point and break
        if (!InvalidCheckpointRange(pindex->nHeight) && (pindex->nHeight >= nHeightStop || (nSecurityLevel != 100 && nCheckpointsAdded >= nSecurityLevel))) {
            uint32_t nChecksum = ParseChecksum(chainActive[pindex->nHeight + 10]->nAccumulatorCheckpoint, coin.getDenomination());
            CBigNum bnAccValue = 0;
            if (!zerocoinDB->ReadAccumulatorValue(nChecksum, bnAccValue)) {
                LogPrintf("%s : failed to find checksum in database for accumulator\n", __func__);
                return false;
            }
            accumulator.setValue(bnAccValue);
            break;
        }

        // if this block contains mints of the denomination that is being spent, then add them to the witness
        if (pindex->MintedDenomination(coin.getDenomination())) {
            //grab mints from this block
            CBlock block;
            if(!ReadBlockFromDisk(block, pindex)) {
                LogPrintf("%s: failed to read block from disk while adding pubcoins to witness\n", __func__);
                return false;
            }

            list<PublicCoin> listPubcoins;
            if(!BlockToPubcoinList(block, listPubcoins, true)) {
                LogPrintf("%s: failed to get zerocoin mintlist from block %n\n", __func__, pindex->nHeight);
                return false;
            }

            //add the mints to the witness
            for (const PublicCoin pubcoin : listPubcoins) {
                if (pubcoin.getDenomination() != coin.getDenomination())
                    continue;

                if (pindex->nHeight == nHeightMintAdded && pubcoin.getValue() == coin.getValue())
                    continue;

                witness.addRawValue(pubcoin.getValue());
                ++nMintsAdded;
            }
        }

        pindex = chainActive[pindex->nHeight + 1];
    }

    if (nMintsAdded < Params().Zerocoin_RequiredAccumulation()) {
        strError = _(strprintf("Less than %d mints added, unable to create spend", Params().Zerocoin_RequiredAccumulation()).c_str());
        LogPrintf("%s : %s\n", __func__, strError);
        return false;
    }

    // calculate how many mints of this denomination existed in the accumulator we initialized
    int nZerocoinStartHeight = GetZerocoinStartHeight();
    pindex = chainActive[nZerocoinStartHeight];
    while (pindex->nHeight < nAccStartHeight) {
        nMintsAdded += count(pindex->vMintDenominationsInBlock.begin(), pindex->vMintDenominationsInBlock.end(), coin.getDenomination());
        pindex = chainActive[pindex->nHeight + 1];
    }

    LogPrint("zero","%s : %d mints added to witness\n", __func__, nMintsAdded);
    return true;
}

map<CoinDenomination, int> GetMintMaturityHeight()
{
    map<CoinDenomination, pair<int, int > > mapDenomMaturity;
    for (auto denom : libzerocoin::zerocoinDenomList)
        mapDenomMaturity.insert(make_pair(denom, make_pair(0, 0)));

    int nConfirmedHeight = chainActive.Height() - Params().Zerocoin_MintRequiredConfirmations();

    // A mint need to get to at least the min maturity height before it will spend.
    int nMinimumMaturityHeight = nConfirmedHeight - (nConfirmedHeight % 10);
    CBlockIndex* pindex = chainActive[nConfirmedHeight];

    while (pindex && pindex->nHeight > Params().Zerocoin_StartHeight()) {
        bool isFinished = true;
        for (auto denom : libzerocoin::zerocoinDenomList) {
            //If the denom has not already had a mint added to it, then see if it has a mint added on this block
            if (mapDenomMaturity.at(denom).first < Params().Zerocoin_RequiredAccumulation()) {
                mapDenomMaturity.at(denom).first += count(pindex->vMintDenominationsInBlock.begin(),
                                                          pindex->vMintDenominationsInBlock.end(), denom);

                //if mint was found then record this block as the first block that maturity occurs.
                if (mapDenomMaturity.at(denom).first >= Params().Zerocoin_RequiredAccumulation())
                    mapDenomMaturity.at(denom).second = std::min(pindex->nHeight, nMinimumMaturityHeight);

                //Signal that we are finished
                isFinished = false;
            }
        }

        if (isFinished)
            break;
        pindex = chainActive[pindex->nHeight - 1];
    }

    //Generate final map
    map<CoinDenomination, int> mapRet;
    for (auto denom : libzerocoin::zerocoinDenomList)
        mapRet.insert(make_pair(denom, mapDenomMaturity.at(denom).second));

    return mapRet;
}
