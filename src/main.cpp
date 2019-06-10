// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"

#include "context.h"
#include "accumulators.h"
#include "addrman.h"
#include "alert.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "init.h"
#include "kernel.h"
#include "masternode-budget.h"
#include "masternode-payments.h"
#include "masternodeman.h"
#include "merkleblock.h"
#include "neti2pd.h"
#include "netbase.h"
#include "netdestination.h"
#include "obfuscation.h"
#include "pow.h"
#include "spork.h"
#include "sporkdb.h"
#include "swifttx.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validationinterface.h"
#include "autoupdatemodel.h"
#include "accumulatormap.h"
#include "primitives/zerocoin.h"
#include "libzerocoin/Denominations.h"

#include <sstream>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>

#include <numeric>

using namespace boost;
using namespace std;
using namespace libzerocoin;

#if defined(NDEBUG)
#error "COLX cannot be compiled without assertions."
#endif

// 6 comes from OPCODE (1) + vch.size() (1) + BIGNUM size (4)
#define SCRIPT_OFFSET 6
// For Script size (BIGNUM/Uint256 size)
#define BIGNUM_SIZE   4

/**
 * Global state
 */
CCriticalSection cs_main;

BlockMap mapBlockIndex;
map<uint256, uint256> mapProofOfStake;
set<pair<COutPoint, unsigned int> > setStakeSeen;
map<unsigned int, unsigned int> mapHashedBlocks;
CChain chainActive;
CBlockIndex* pindexBestHeader = NULL;
int64_t nTimeBestReceived = 0;
CWaitableCriticalSection csBestBlock;
CConditionVariable cvBlockChange;
int nScriptCheckThreads = 0;
bool fImporting = false;
bool fReindex = false;
bool fTxIndex = true;
bool fIsBareMultisigStd = true;
bool fCheckBlockIndex = false;
bool fVerifyingBlocks = false;
unsigned int nCoinCacheSize = 5000;
bool fAlerts = DEFAULT_ALERTS;
int64_t nReserveBalance = 0;

/** Fees smaller than this (in upiv) are considered zero fee (for relaying and mining)
 * We are ~100 times smaller then bitcoin now (2015-06-23), set minRelayTxFee only 10 times higher
 * so it's still 10 times lower comparing to bitcoin.
 */
CFeeRate minRelayTxFee = CFeeRate(1 * COIN);
CTxMemPool mempool(::minRelayTxFee);

struct COrphanTx {
    CTransaction tx;
    NodeId fromPeer;
};

map<uint256, COrphanTx> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;
map<uint256, int64_t> mapRejectedBlocks;
map<uint256, int64_t> mapZerocoinspends; //txid, time received


void EraseOrphansFor(NodeId peer);

static void CheckBlockIndex();

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const string strMessageMagic = "DarkNet Signed Message:\n";

// Internal stuff
namespace
{
struct CBlockIndexWorkComparator {
    bool operator()(CBlockIndex* pa, CBlockIndex* pb) const
    {
        // First sort by most total work, ...
        if (pa->nChainWork > pb->nChainWork) return false;
        if (pa->nChainWork < pb->nChainWork) return true;

        // ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId) return false;
        if (pa->nSequenceId > pb->nSequenceId) return true;

        // Use pointer address as tie breaker (should only happen with blocks
        // loaded from disk, as those all have id 0).
        if (pa < pb) return false;
        if (pa > pb) return true;

        // Identical blocks.
        return false;
    }
};

CBlockIndex* pindexBestInvalid;

/**
     * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
     * as good as our current tip or better. Entries may be failed, though.
     */
set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexCandidates;
/** Number of nodes with fSyncStarted. */
int nSyncStarted = 0;
/** All pairs A->B, where A (or one if its ancestors) misses transactions, but B has transactions. */
multimap<CBlockIndex*, CBlockIndex*> mapBlocksUnlinked;

CCriticalSection cs_LastBlockFile;
std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;

/**
     * Every received block is assigned a unique and increasing identifier, so we
     * know which one to give priority in case of a fork.
     */
CCriticalSection cs_nBlockSequenceId;
/** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
uint32_t nBlockSequenceId = 1;

/**
     * Sources of received blocks, to be able to send them reject messages or ban
     * them, if processing happens afterwards. Protected by cs_main.
     */
map<uint256, NodeId> mapBlockSource;

/** Blocks that are in flight, and that are in the queue to be downloaded. Protected by cs_main. */
struct QueuedBlock {
    uint256 hash;
    CBlockIndex* pindex;        //! Optional.
    int64_t nTime;              //! Time of "getdata" request in microseconds.
    int nValidatedQueuedBefore; //! Number of blocks queued with validated headers (globally) at the time this one is requested.
    bool fValidatedHeaders;     //! Whether this block has validated headers at the time of request.
};
map<uint256, pair<NodeId, list<QueuedBlock>::iterator> > mapBlocksInFlight;

NodeId _lastNodeStalling;
int _lastBlockStalling;

/** Number of blocks in flight with validated headers. */
int nQueuedValidatedHeaders = 0;

/** Number of preferable block download peers. */
int nPreferredDownload = 0;

/** Dirty block index entries. */
set<CBlockIndex*> setDirtyBlockIndex;

/** Dirty block file entries. */
set<int> setDirtyFileInfo;
} // anon namespace

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace
{
struct CBlockReject {
    unsigned char chRejectCode;
    string strRejectReason;
    uint256 hashBlock;
};

class CNodeBlocks
{
public:
    CNodeBlocks():
            maxSize(0),
            maxAvg(0)
    {
        maxSize = GetArg("-blockspamfiltermaxsize", DEFAULT_BLOCK_SPAM_FILTER_MAX_SIZE);
        maxAvg = GetArg("-blockspamfiltermaxavg", DEFAULT_BLOCK_SPAM_FILTER_MAX_AVG);
    }

    bool onBlockReceived(int nHeight) {
        if(nHeight > 0 && maxSize && maxAvg) {
            addPoint(nHeight);
            return true;
        }
        return false;
    }

    bool updateState(CValidationState& state, bool ret)
    {
        // No Blocks
        size_t size = points.size();
        if(size == 0)
            return ret;

        // Compute the number of the received blocks
        size_t nBlocks = 0;
        for(auto point : points)
        {
            nBlocks += point.second;
        }

        // Compute the average value per height
        double nAvgValue = (double)nBlocks / size;

        // Ban the node if try to spam
        bool banNode = (nAvgValue >= 1.5 * maxAvg && size >= maxAvg) ||
                       (nAvgValue >= maxAvg && nBlocks >= maxSize) ||
                       (nBlocks >= maxSize * 3);
        if(banNode)
        {
            // Clear the points and ban the node
            points.clear();
            return state.DoS(100, error("block-spam ban node for sending spam"));
        }

        return ret;
    }

private:
    void addPoint(int height)
    {
        // Remove the last element in the list
        if(points.size() == maxSize)
        {
            points.erase(points.begin());
        }

        // Add the point to the list
        int occurrence = 0;
        auto mi = points.find(height);
        if (mi != points.end())
            occurrence = (*mi).second;
        occurrence++;
        points[height] = occurrence;
    }

private:
    std::map<int,int> points;
    size_t maxSize;
    size_t maxAvg;
};

/**
 * Maintain validation-specific state about nodes, protected by cs_main, instead
 * by CI2pdNode's own locks. This simplifies asynchronous operation, where
 * processing of incoming data is done after the ProcessMessage call returns,
 * and we're no longer holding the node's locks.
 */
struct CNodeState {
    //! The peer's address
    CDestination address;
    //! Whether we have a fully established connection.
    bool fCurrentlyConnected;
    //! Accumulated misbehaviour score for this peer.
    int nMisbehavior;
    //! Whether this peer should be disconnected and banned (unless whitelisted).
    bool fShouldBan;
    //! String name of this peer (debugging/logging purposes).
    std::string name;
    //! List of asynchronously-determined block rejections to notify this peer about.
    std::vector<CBlockReject> rejects;
    //! The best known block we know this peer has announced.
    CBlockIndex* pindexBestKnownBlock;
    //! The hash of the last unknown block this peer has announced.
    uint256 hashLastUnknownBlock;
    //! The last full block we both have.
    CBlockIndex* pindexLastCommonBlock;
    //! The best header we have sent our peer.
    CBlockIndex *pindexBestHeaderSent;
    //! Whether we've started headers synchronization with this peer.
    bool fSyncStarted;
    //! Since when we're stalling block download progress (in microseconds), or 0.
    int64_t nStallingSince;
    list<QueuedBlock> vBlocksInFlight;
    int nBlocksInFlight;
    //! Whether we consider this a preferred download peer.
    bool fPreferredDownload;
    //! Whether this peer wants invs or headers (when possible) for block announcements.
    bool fPreferHeaders;
    //! Blocks sent by this node
    CNodeBlocks nodeBlocks;

    int64_t _stallingFirst;
    int64_t _stallingLast;
    int64_t _stallingCurrent;
    int64_t _stallingTotal;
    int _stallingCurrentBlock;
    bool _stillStalling;
    NodeId _previousNodeStalling;
    int _previousBlockStalling;

    CNodeState()
    {
        fCurrentlyConnected = false;
        nMisbehavior = 0;
        fShouldBan = false;
        pindexBestKnownBlock = NULL;
        hashLastUnknownBlock = uint256(0);
        pindexLastCommonBlock = NULL;
        pindexBestHeaderSent = NULL;
        fSyncStarted = false;
        nStallingSince = 0;
        nBlocksInFlight = 0;
        fPreferredDownload = false;
        fPreferHeaders = false;

        _stallingFirst = 0;
        _stallingLast = 0;
        _stallingCurrent = 0;
        _stallingTotal = 0;
        _stallingCurrentBlock = 0;
        _previousNodeStalling = 0;
        _previousBlockStalling = 0;
        _stillStalling = false;
    }
};

/** Map maintaining per-node state. Requires cs_main. */
map<NodeId, CNodeState> mapNodeState;

// Requires cs_main.
CNodeState* State(NodeId pnode)
{
    map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
    if (it == mapNodeState.end())
        return NULL;
    return &it->second;
}

int GetHeight()
{
    while (true) {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            MilliSleep(50);
            continue;
        }
        return chainActive.Height();
    }
}

void UpdatePreferredDownload(CI2pdNode* node, CNodeState* state)
{
    nPreferredDownload -= state->fPreferredDownload;

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;

    nPreferredDownload += state->fPreferredDownload;
}

void InitializeNode(NodeId nodeid, const CI2pdNode* pnode)
{
    LOCK(cs_main);
    CNodeState& state = mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
    state.name = pnode->addrName;
    state.address = pnode->addr;
}

void FinalizeNode(NodeId nodeid)
{
    LOCK(cs_main);
    CNodeState* state = State(nodeid);

    if (state->fSyncStarted)
        nSyncStarted--;

    if (state->nMisbehavior == 0 && state->fCurrentlyConnected) {
        AddressCurrentlyConnected(state->address);
    }

    BOOST_FOREACH (const QueuedBlock& entry, state->vBlocksInFlight)
        mapBlocksInFlight.erase(entry.hash);
    EraseOrphansFor(nodeid);
    nPreferredDownload -= state->fPreferredDownload;

    mapNodeState.erase(nodeid);
}

// Requires cs_main.
// +1 - done - just processed new block and 100% done
//  0 - not sure - creating new in-flight, block is removed by now and should just return
// -1 - stalling - not done, just stalling
void MarkBlockAsReceived(const uint256& hash, int isdone = 1)
{
    map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end()) {
        CNodeState* state = State(itInFlight->second.first);
        nQueuedValidatedHeaders -= itInFlight->second.second->fValidatedHeaders;
        state->vBlocksInFlight.erase(itInFlight->second.second);
        state->nBlocksInFlight--;

        NodeId nodeid = itInFlight->second.first;
        auto block = itInFlight->second.second;
        int height = block->pindex != nullptr ? block->pindex->nHeight : 0;
        int64_t nNow = GetTimeMicros();

        // check overall statistics to avoid repeating the same nodes all over again (was a bug)
        if (isdone < 0) { // stalling
            if (_lastNodeStalling == nodeid) {
                LogPrint("blockdown", "%s: _lastNodeStalling == nodeid: %d, %d, %d, %d, %d \n", 
                    __func__, nodeid, height, state->_stallingCurrent, state->_stallingCurrentBlock,
                    state->_stallingLast);
            }
            
            // state->nStallingSince -> block->nTime
            state->_stallingCurrent = (nNow - block->nTime) / 1000000;
            state->_stallingCurrentBlock = height;
            if (state->_stallingFirst == 0)
                state->_stallingFirst = block->nTime;
            state->_stallingLast = block->nTime;
            state->_stallingTotal += state->_stallingCurrent;
            state->_previousNodeStalling = _lastNodeStalling;
            state->_previousBlockStalling = _lastBlockStalling;
            state->_stillStalling = true;
            if (_lastNodeStalling == nodeid) {
                LogPrint("blockdown", "%s: shouldn't happen?: %d, %d \n", __func__, nodeid, height);
                state->_previousNodeStalling = 0;
                state->_previousBlockStalling = 0;
            }
            _lastNodeStalling = nodeid;
            _lastBlockStalling = height;
        } else if (isdone > 0) { // new block done
            NodeId prevNode = _lastNodeStalling;
            int prevBlock = _lastBlockStalling;
            while (prevNode > 0) {
                if (prevBlock != height) {
                    LogPrint("blockdown", "%s: prevBlock != height: %d, %d \n", 
                        __func__, prevBlock, height);
                    break;
                }
                CNodeState* state = State(prevNode);
                if (state == nullptr) {
                    LogPrint("blockdown", "%s: State(prevNode) == null: %d, %d \n", 
                        __func__, prevNode, prevBlock);
                    break;
                }
                if (state->_previousNodeStalling == prevNode) {
                    LogPrint("blockdown", "%s: _previousNodeStalling == prevNode: %d, %d, %d, %d, %d \n", __func__, prevNode, prevBlock, state->_stallingCurrent, state->_stallingCurrentBlock, state->_stallingLast);
                    // we shouldn't change things here but this is to correct the possible recursion
                    state->_previousNodeStalling = 0;
                    state->_previousBlockStalling = 0;
                }
                prevNode = state->_previousNodeStalling;
                prevBlock = state->_previousBlockStalling;
                state->_stillStalling = false;
                state->_previousNodeStalling = 0;
                state->_previousBlockStalling = 0;
            }
            _lastNodeStalling = 0;
            _lastBlockStalling = 0;
        } else { // new in-flight, not sure
            // do nothing, just preserve and deal w/ it within the MarkBlockAsInFlight
            // shouldn't happen?
            LogPrint("blockdown", "%s: isdone == 0: %d, %d \n", 
                __func__, nodeid, height);
            // _lastNodeStalling = _lastNodeStalling;
            // _lastBlockStalling = _lastBlockStalling;
        }

        state->nStallingSince = 0;

        mapBlocksInFlight.erase(itInFlight);
    }
}

bool IsStalling(NodeId nodeid)
{
    if (nodeid <= 0) return false;

    NodeId prevNode = _lastNodeStalling;
    int prevBlock = _lastBlockStalling;
    while (prevNode > 0) {
        if (prevNode == nodeid)
            return true;

        CNodeState* state = State(prevNode);
        if (state == nullptr) {
            LogPrint("blockdown", "%s: State(prevNode) == null: %d, %d \n", 
                __func__, prevNode, prevBlock);
            break;
        }
        if (state->_previousNodeStalling == prevNode) {
            LogPrint("blockdown", "%s: _previousNodeStalling == prevNode: %d, %d, %d, %d, %d \n", __func__, prevNode, prevBlock, state->_stallingCurrent, state->_stallingCurrentBlock, state->_stallingLast);
            // we shouldn't change things here but this is to correct the possible recursion
            state->_previousNodeStalling = 0;
            state->_previousBlockStalling = 0;
        }
        prevNode = state->_previousNodeStalling;
        prevBlock = state->_previousBlockStalling;
    }
    return false;
}

bool WasStallingRecently(NodeId nodeid, int howRecentSeconds = 0)
{
    if (nodeid <= 0) return false;

    if (IsStalling(nodeid)) return true;

    if (howRecentSeconds == 0)
        howRecentSeconds = GetArg("-stallbantime", 300); // 5 minutes

    CNodeState* state = State(nodeid);
    if (state == nullptr) {
        LogPrint("blockdown", "%s: State(nodeid) == null: %d \n", __func__, nodeid);
        return false;
    }

    int64_t nNow = GetTimeMicros();
    int nSecondsSinceStalling = (nNow - state->_stallingLast) / 1000000;
    if (nSecondsSinceStalling < howRecentSeconds)
        return true;

    return false;
}

// Requires cs_main.
void MarkBlockAsInFlight(NodeId nodeid, const uint256& hash, CBlockIndex* pindex = NULL)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    // Make sure it's not listed somewhere already.
    MarkBlockAsReceived(hash, 0);

    QueuedBlock newentry = {hash, pindex, GetTimeMicros(), nQueuedValidatedHeaders, pindex != NULL};
    nQueuedValidatedHeaders += newentry.fValidatedHeaders;
    list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
    state->nBlocksInFlight++;
    mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
}

/** Check whether the last unknown block a peer advertized is not yet known. */
void ProcessBlockAvailability(NodeId nodeid)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    if (state->hashLastUnknownBlock != 0) {
        BlockMap::iterator itOld = mapBlockIndex.find(state->hashLastUnknownBlock);
        if (itOld != mapBlockIndex.end() && itOld->second->nChainWork > 0) {
            if (state->pindexBestKnownBlock == NULL || itOld->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                state->pindexBestKnownBlock = itOld->second;
            state->hashLastUnknownBlock = uint256(0);
        }
    }
}

/** Update tracking information about which blocks a peer is assumed to have. */
void UpdateBlockAvailability(NodeId nodeid, const uint256& hash)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    ProcessBlockAvailability(nodeid);

    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end() && it->second->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == NULL || it->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            state->pindexBestKnownBlock = it->second;
    } else {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

// Requires cs_main
bool CanDirectFetch()
{
    // merged from https://github.com/bitcoin/bitcoin/pull/7129/files
    // not sure if it is applicable to the COLX
    return true;
    //return chainActive.Tip()->GetBlockTime() > GetAdjustedTime() - Params().TargetSpacing() * 20;
}

// Requires cs_main
bool PeerHasHeader(CNodeState *state, CBlockIndex *pindex)
{
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
        return true;
    else if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
        return true;
    else
        return false;
}

/** Find the last common ancestor two blocks have.
 *  Both pa and pb must be non-NULL. */
CBlockIndex* LastCommonAncestor(CBlockIndex* pa, CBlockIndex* pb)
{
    if (pa->nHeight > pb->nHeight) {
        pa = pa->GetAncestor(pb->nHeight);
    } else if (pb->nHeight > pa->nHeight) {
        pb = pb->GetAncestor(pa->nHeight);
    }

    while (pa != pb && pa && pb) {
        pa = pa->pprev;
        pb = pb->pprev;
    }

    // Eventually all chain branches meet at the genesis block.
    assert(pa == pb);
    return pa;
}

static std::string Join(std::vector<CBlockIndex*>& v)
{
    if (v.empty())
        return std::string();
    return std::accumulate(v.begin()+1, v.end(), std::to_string(v[0]->nHeight),
                        [](const std::string& a, CBlockIndex* b){
                            return a + ',' + std::to_string(b->nHeight);
                        });
}

/** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
 *  at most count entries. */
void FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<CBlockIndex*>& vBlocks, NodeId& nodeStaller)
{
    if (count == 0)
        return;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    if (state->pindexBestKnownBlock == NULL || state->pindexBestKnownBlock->nChainWork < chainActive.Tip()->nChainWork) {
        // This peer has nothing interesting.
        return;
    }

    if (state->pindexLastCommonBlock == NULL) {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock = chainActive[std::min(state->pindexBestKnownBlock->nHeight, chainActive.Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of their current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return;

    std::vector<CBlockIndex*> vToFetch;
    CBlockIndex* pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last
    // linked block we have in common with this peer. The +1 is so we can detect stalling, namely if we would be able to
    // download that next block if the window were 1 larger.
    int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);

    LogPrint("blockdown", "%s : end, height...: %d, %d, %d, %d \n", __func__, nWindowEnd, nMaxHeight, state->pindexLastCommonBlock->nHeight, state->pindexBestKnownBlock->nHeight);

    int nInFlightTimeOut = GetArg("-inflighttimeout", 180); // 3 minutes
    bool fInFlightFix = GetBoolArg("-inflightfix", true);

    NodeId waitingfor = -1;
    // nodeStaller = -1;
    
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;

        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        if (LogAcceptCategory("blockdown") && vToFetch.size() > 0) 
            LogPrint("blockdown", "%s : fetch %d, %d, %s \n", __func__, nToFetch, pindexWalk->nHeight, Join(vToFetch));

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the mean time, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded.
        BOOST_FOREACH (CBlockIndex* pindex, vToFetch) {
            if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA) {
                if (pindex->nChainTx)
                    state->pindexLastCommonBlock = pindex;
            } else if (mapBlocksInFlight.count(pindex->GetBlockHash()) == 0) {
                // The block is not already downloaded, and not yet in flight.
                if (pindex->nHeight > nWindowEnd) {
                    // We reached the end of the window.
                    if (vBlocks.size() == 0 && waitingfor != nodeid) {
                        // We aren't able to fetch anything, but we would be if the download window was one larger.
                        nodeStaller = waitingfor;
                    }
                    return;
                }
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count)
                    return;
            } else if (waitingfor == -1) {
                // This is the first already-in-flight block.
                // nodeStaller = waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
                waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;

                // int nInFlightTimeOut = GetArg("-inflighttimeout", 180); // 3 minutes
                // bool fInFlightFix = GetBoolArg("-inflightfix", true);
                auto hash = pindex->GetBlockHash();
                auto itInFlight = mapBlocksInFlight[hash].second;
                auto timeSpentSecs = (GetTimeMicros() - itInFlight->nTime) / 1000000;
                if (fInFlightFix && timeSpentSecs > nInFlightTimeOut) {
                    CNodeState* state = State(waitingfor);
                    assert(state != NULL);
                    
                    // 'un-flight' it so we can move on...
                    MarkBlockAsReceived(hash, -1);

                    LogPrintf("%s : block in flight was stalling for too long : %d, %d, %d \n", __func__, waitingfor, pindex->nHeight, (GetTimeMicros() - itInFlight->nTime) / 1000000 );

                    if (waitingfor != nodeid) {
                        // nodeStaller = waitingfor = -1;
                        waitingfor = -1;
                        
                        // now we can spin it like the rest of the lot (no need to wait for another cycle?).
                        // if (pindex->nHeight > nWindowEnd) 
                        //     return;
                        if (pindex->nHeight > nWindowEnd) {
                            // We reached the end of the window.
                            if (vBlocks.size() == 0 && waitingfor != nodeid) {
                                // We aren't able to fetch anything, but we would be if the download window was one larger.
                                nodeStaller = waitingfor;
                            }
                            return;
                        }
                        vBlocks.push_back(pindex);
                        if (vBlocks.size() == count)
                            return;
                    }
                }

                if (LogAcceptCategory("blockdown")) {
                    // auto itInFlight = mapBlocksInFlight[hash].second;
                    LogPrint("blockdown", "%s : waitingfor: %d, %d, %d \n", __func__, waitingfor, pindex->nHeight, (GetTimeMicros() - itInFlight->nTime) / 1000000 );
                }
            }
        }
    }
}

} // anon namespace

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats)
{
    LOCK(cs_main);
    CNodeState* state = State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    BOOST_FOREACH (const QueuedBlock& queue, state->vBlocksInFlight) {
        if (queue.pindex)
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
    }
    return true;
}

void RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.connect(&GetHeight);
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.AddressRefreshBroadcast.connect(&AddressRefreshBroadcast);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&GetHeight);
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.AddressRefreshBroadcast.disconnect(&AddressRefreshBroadcast);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator)
{
    // Find the first block the caller has in the main chain
    BOOST_FOREACH (const uint256& hash, locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex* pindex = (*mi).second;
            if (chain.Contains(pindex))
                return pindex;
        }
    }
    return chain.Genesis();
}

CCoinsViewCache* pcoinsTip = NULL;
CBlockTreeDB* pblocktree = NULL;
CZerocoinDB* zerocoinDB = NULL;
CSporkDB* pSporkDB = NULL;

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx, NodeId peer)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz > 5000) {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString());
        return false;
    }

    mapOrphanTransactions[hash].tx = tx;
    mapOrphanTransactions[hash].fromPeer = peer;
    BOOST_FOREACH (const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrint("mempool", "stored orphan tx %s (mapsz %u prevsz %u)\n", hash.ToString(),
        mapOrphanTransactions.size(), mapOrphanTransactionsByPrev.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return;
    BOOST_FOREACH (const CTxIn& txin, it->second.tx.vin) {
        map<uint256, set<uint256> >::iterator itPrev = mapOrphanTransactionsByPrev.find(txin.prevout.hash);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(hash);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }
    mapOrphanTransactions.erase(it);
}

void EraseOrphansFor(NodeId peer)
{
    int nErased = 0;
    map<uint256, COrphanTx>::iterator iter = mapOrphanTransactions.begin();
    while (iter != mapOrphanTransactions.end()) {
        map<uint256, COrphanTx>::iterator maybeErase = iter++; // increment to avoid iterator becoming invalid
        if (maybeErase->second.fromPeer == peer) {
            EraseOrphanTx(maybeErase->second.tx.GetHash());
            ++nErased;
        }
    }
    if (nErased > 0) LogPrint("mempool", "Erased %d orphan tx from peer %d\n", nErased, peer);
}


unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans) {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}

bool IsStandardTx(const CTransaction& tx, string& reason)
{
    AssertLockHeld(cs_main);
    if (tx.nVersion > CTransaction::CURRENT_VERSION || tx.nVersion < 1) {
        reason = "version";
        return false;
    }

    // Treat non-final transactions as non-standard to prevent a specific type
    // of double-spend attack, as well as DoS attacks. (if the transaction
    // can't be mined, the attacker isn't expending resources broadcasting it)
    // Basically we don't want to propagate transactions that can't be included in
    // the next block.
    //
    // However, IsFinalTx() is confusing... Without arguments, it uses
    // chainActive.Height() to evaluate nLockTime; when a block is accepted, chainActive.Height()
    // is set to the value of nHeight in the block. However, when IsFinalTx()
    // is called within CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a transaction can
    // be part of the *next* block, we need to call IsFinalTx() with one more
    // than chainActive.Height().
    //
    // Timestamps on the other hand don't get any special treatment, because we
    // can't know what timestamp the next block will have, and there aren't
    // timestamp applications where it matters.
    if (!IsFinalTx(tx, reason, chainActive.Height() + 1))
        return false;

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    unsigned int nMaxSize = tx.ContainsZerocoins() ? MAX_ZEROCOIN_TX_SIZE : MAX_STANDARD_TX_SIZE;
    if (sz >= nMaxSize) {
        reason = "tx-size";
        return false;
    }

    for (const CTxIn& txin : tx.vin) {
        if (txin.scriptSig.IsZerocoinSpend())
            continue;
        // Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
        // keys. (remember the 520 byte limit on redeemScript size) That works
        // out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)+3=1627
        // bytes of scriptSig, which we round off to 1650 bytes for some minor
        // future-proofing. That's also enough to spend a 20-of-20
        // CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
        // considered standard)
        if (txin.scriptSig.size() > 1650) {
            reason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly()) {
            reason = "scriptsig-not-pushonly";
            return false;
        }
    }

    unsigned int nDataOut = 0;
    txnouttype whichType;
    BOOST_FOREACH (const CTxOut& txout, tx.vout) {
        if (!::IsStandard(txout.scriptPubKey, whichType)) {
            reason = "scriptpubkey";
            return false;
        }

        if (whichType == TX_NULL_DATA)
            nDataOut++;
        else if ((whichType == TX_MULTISIG) && (!fIsBareMultisigStd)) {
            reason = "bare-multisig";
            return false;
        } else if (txout.IsDust(::minRelayTxFee)) {
            reason = "dust";
            return false;
        }
    }

    // only one OP_RETURN txout is permitted
    if (nDataOut > 1) {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

bool IsFinalTx(const CTransaction& tx, int nBlockHeight, int64_t nBlockTime)
{
    string reason;
    return IsFinalTx(tx, reason, nBlockHeight, nBlockTime);
}

bool IsFinalTx(const CTransaction& tx, std::string& reason, int nBlockHeight, int64_t nBlockTime)
{
    AssertLockHeld(cs_main);
    // Time based nLockTime implemented in 0.1.6
    if (tx.nLockTime == 0)
        return true;

    if (nBlockHeight == 0)
        nBlockHeight = chainActive.Height();

    if (nBlockTime == 0)
        nBlockTime = GetAdjustedTime();

    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;

    BOOST_FOREACH (const CTxIn& txin, tx.vin) {
        if (!txin.IsFinal()) {
            reason = strprintf("non-final, nLockTime=%u, nBlockHeight=%d, txin=%s", tx.nLockTime, nBlockHeight, txin.ToString());
            return false;
        }
    }

    return true;
}

/**
 * Check transaction inputs to mitigate two
 * potential denial-of-service attacks:
 *
 * 1. scriptSigs with extra data stuffed into them,
 *    not consumed by scriptPubKey (or P2SH script)
 * 2. P2SH scripts with a crazy number of expensive
 *    CHECKSIG/CHECKMULTISIG operations
 */
bool AreInputsStandard(const CTransaction& tx, const CCoinsViewCache& mapInputs)
{
    if (tx.IsCoinBase() || tx.IsZerocoinSpend())
        return true; // coinbase has no inputs and zerocoinspend has a special input
    //todo should there be a check for a 'standard' zerocoinspend here?

    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const CTxOut& prev = mapInputs.GetOutputFor(tx.vin[i]);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig
        // IsStandard() will have already returned false
        // and this method isn't called.
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, tx.vin[i].scriptSig, false, BaseSignatureChecker()))
            return false;

        if (whichType == TX_SCRIPTHASH) {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (Solver(subscript, whichType2, vSolutions2)) {
                int tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
                if (tmpExpected < 0)
                    return false;
                nArgsExpected += tmpExpected;
            } else {
                // Any other Script with less than 15 sigops OK:
                unsigned int sigops = subscript.GetSigOpCount(true);
                // ... extra data left on the stack after execution is OK, too:
                return (sigops <= MAX_P2SH_SIGOPS);
            }
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH (const CTxIn& txin, tx.vin) {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH (const CTxOut& txout, tx.vout) {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase() || tx.IsZerocoinSpend())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const CTxOut& prevout = inputs.GetOutputFor(tx.vin[i]);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int GetInputAge(CTxIn& vin)
{
    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);
    {
        LOCK(mempool.cs);
        CCoinsViewMemPool viewMempool(pcoinsTip, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        const CCoins* coins = view.AccessCoins(vin.prevout.hash);

        if (coins) {
            if (coins->nHeight < 0) return 0;
            return (chainActive.Tip()->nHeight + 1) - coins->nHeight;
        } else
            return -1;
    }
}

int GetInputAgeIX(uint256 nTXHash, CTxIn& vin)
{
    int sigs = 0;
    int nResult = GetInputAge(vin);
    if (nResult < 0) nResult = 0;

    if (nResult < 6) {
        std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(nTXHash);
        if (i != mapTxLocks.end()) {
            sigs = (*i).second.CountSignatures();
        }
        if (sigs >= SWIFTTX_SIGNATURES_REQUIRED) {
            return nSwiftTXDepth + nResult;
        }
    }

    return -1;
}

int GetIXConfirmations(uint256 nTXHash)
{
    int sigs = 0;

    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(nTXHash);
    if (i != mapTxLocks.end()) {
        sigs = (*i).second.CountSignatures();
    }
    if (sigs >= SWIFTTX_SIGNATURES_REQUIRED) {
        return nSwiftTXDepth;
    }

    return 0;
}

bool MoneyRange(CAmount nValueOut)
{
    return nValueOut >= 0 && nValueOut <= Params().MaxMoneyOut();
}

int GetZerocoinStartHeight()
{
    return Params().Zerocoin_StartHeight();
}

void FindMints(vector<CZerocoinMint> vMintsToFind, vector<CZerocoinMint>& vMintsToUpdate, vector<CZerocoinMint>& vMissingMints, bool fExtendedSearch)
{
    // see which mints are in our public zerocoin database. The mint should be here if it exists, unless
    // something went wrong
    for (CZerocoinMint mint : vMintsToFind) {
        uint256 txHash;
        if (!zerocoinDB->ReadCoinMint(mint.GetValue(), txHash)) {
            vMissingMints.push_back(mint);
            continue;
        }

        // make sure the txhash and block height meta data are correct for this mint
        CTransaction tx;
        uint256 hashBlock;
        if (!GetTransaction(txHash, tx, hashBlock, true)) {
            LogPrintf("%s : cannot find tx %s\n", __func__, txHash.GetHex());
            vMissingMints.push_back(mint);
            continue;
        }

        if (!mapBlockIndex.count(hashBlock)) {
            LogPrintf("%s : cannot find block %s\n", __func__, hashBlock.GetHex());
            vMissingMints.push_back(mint);
            continue;
        }

        //see if this mint is spent
        uint256 hashTxSpend = 0;
        zerocoinDB->ReadCoinSpend(mint.GetSerialNumber(), hashTxSpend);
        bool fSpent = hashTxSpend != 0;

        //if marked as spent, check that it actually made it into the chain
        CTransaction txSpend;
        uint256 hashBlockSpend;
        if (fSpent && !GetTransaction(hashTxSpend, txSpend, hashBlockSpend, true)) {
            LogPrintf("%s : cannot find spend tx %s\n", __func__, hashTxSpend.GetHex());
            zerocoinDB->EraseCoinSpend(mint.GetSerialNumber());
            mint.SetUsed(false);
            vMintsToUpdate.push_back(mint);
            continue;
        }

        //The mint has been incorrectly labelled as spent in zerocoinDB and needs to be undone
        int nHeightTx = 0;
        if (fSpent && !IsSerialInBlockchain(mint.GetSerialNumber(), nHeightTx)) {
            LogPrintf("%s : cannot find block %s. Erasing coinspend from zerocoinDB.\n", __func__, hashBlockSpend.GetHex());
            zerocoinDB->EraseCoinSpend(mint.GetSerialNumber());
            mint.SetUsed(false);
            vMintsToUpdate.push_back(mint);
            continue;
        }

        // if meta data is correct, then no need to update
        if (mint.GetTxHash() == txHash && mint.GetHeight() == mapBlockIndex[hashBlock]->nHeight && mint.IsUsed() == fSpent)
            continue;

        //mark this mint for update
        mint.SetTxHash(txHash);
        mint.SetHeight(mapBlockIndex[hashBlock]->nHeight);
        mint.SetUsed(fSpent);

        vMintsToUpdate.push_back(mint);
    }

    if (fExtendedSearch)
    {
        // search the blockchain for the meta data on our missing mints
        int nZerocoinStartHeight = GetZerocoinStartHeight();

        for (int i = nZerocoinStartHeight; i < chainActive.Height(); i++) {

            if(i % 1000 == 0)
                LogPrintf("%s : scanned %d blocks\n", __func__, i - nZerocoinStartHeight);

            if(chainActive[i]->vMintDenominationsInBlock.empty())
                continue;

            CBlock block;
            if(!ReadBlockFromDisk(block, chainActive[i]))
                continue;

            list<CZerocoinMint> vMints;
            if(!BlockToZerocoinMintList(block, vMints, true))
                continue;

            // search the blocks mints to see if it contains the mint that is requesting meta data updates
            for (CZerocoinMint mintBlockChain : vMints) {
                for (CZerocoinMint mintMissing : vMissingMints) {
                    if (mintMissing.GetValue() == mintBlockChain.GetValue()) {
                        LogPrintf("%s FOUND %s in block %d\n", __func__, mintMissing.GetValue().GetHex(), i);
                        mintMissing.SetHeight(i);
                        mintMissing.SetTxHash(mintBlockChain.GetTxHash());
                        vMintsToUpdate.push_back(mintMissing);
                    }
                }
            }
        }
    }

    //remove any missing mints that were found
    for (CZerocoinMint mintMissing : vMissingMints) {
        for (CZerocoinMint mintFound : vMintsToUpdate) {
            if (mintMissing.GetValue() == mintFound.GetValue())
                std::remove(vMissingMints.begin(), vMissingMints.end(), mintMissing);
        }
    }

}

bool GetZerocoinMint(const CBigNum& bnPubcoin, uint256& txHash)
{
    txHash = 0;
    return zerocoinDB->ReadCoinMint(bnPubcoin, txHash);
}

bool IsSerialKnown(const CBigNum& bnSerial)
{
    uint256 txHash = 0;
    return zerocoinDB->ReadCoinSpend(bnSerial, txHash);
}

bool IsSerialInBlockchain(const CBigNum& bnSerial, int& nHeightTx)
{
    uint256 txHash = 0;
    // if not in zerocoinDB then its not in the blockchain
    if (!zerocoinDB->ReadCoinSpend(bnSerial, txHash))
        return false;

    CTransaction tx;
    uint256 hashBlock;
    if (!GetTransaction(txHash, tx, hashBlock, true))
        return false;

    bool inChain = mapBlockIndex.count(hashBlock) && chainActive.Contains(mapBlockIndex[hashBlock]);
    if (inChain)
        nHeightTx = mapBlockIndex.at(hashBlock)->nHeight;

    return inChain;
}

bool RemoveSerialFromDB(const CBigNum& bnSerial)
{
    return zerocoinDB->EraseCoinSpend(bnSerial);
}

/** zerocoin transaction checks */
bool RecordMintToDB(PublicCoin publicZerocoin, const uint256& txHash)
{
    //Check the pubCoinValue didn't already store in the zerocoin database. todo: pubcoin memory map?
    //write the zerocoinmint to db if we don't already have it
    //note that many of the mint parameters are not set here because those params are private to the minter
    CZerocoinMint pubCoinTx;
    uint256 hashFromDB;
    if (zerocoinDB->ReadCoinMint(publicZerocoin.getValue(), hashFromDB)) {
        if(hashFromDB == txHash)
            return true;

        LogPrintf("RecordMintToDB: failed, we already have this public coin recorded\n");
        return false;
    }

    if (!zerocoinDB->WriteCoinMint(publicZerocoin, txHash)) {
        LogPrintf("RecordMintToDB: failed to record public coin to DB\n");
        return false;
    }

    return true;
}

bool TxOutToPublicCoin(const CTxOut txout, PublicCoin& pubCoin, CValidationState& state)
{
    CBigNum publicZerocoin;
    vector<unsigned char> vchZeroMint;
    vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + SCRIPT_OFFSET,
                           txout.scriptPubKey.begin() + txout.scriptPubKey.size());
    publicZerocoin.setvch(vchZeroMint);

    CoinDenomination denomination = AmountToZerocoinDenomination(txout.nValue);
    LogPrint("zero", "%s ZCPRINT denomination %d pubcoin %s\n", __func__, denomination, publicZerocoin.GetHex());
    if (denomination == ZQ_ERROR)
        return state.DoS(100, error("TxOutToPublicCoin : txout.nValue is not correct"));

    PublicCoin checkPubCoin(Params().Zerocoin_Params(), publicZerocoin, denomination);
    pubCoin = checkPubCoin;

    return true;
}

bool BlockToPubcoinList(const CBlock& block, list<PublicCoin>& listPubcoins, bool fFilterInvalid)
{
    for (const CTransaction tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        // Filter out mints that have used invalid outpoints
        if (fFilterInvalid) {
            bool fValid = true;
            for (const CTxIn in : tx.vin) {
                if (!ValidOutPoint(in.prevout, INT_MAX)) {
                    fValid = false;
                    break;
                }
            }
            if (!fValid)
                continue;
        }

        uint256 txHash = tx.GetHash();
        for (unsigned int i = 0; i < tx.vout.size(); i++) {
            //Filter out mints that use invalid outpoints - edge case: invalid spend with minted change
            if (fFilterInvalid && !ValidOutPoint(COutPoint(txHash, i), INT_MAX))
                break;

            const CTxOut txOut = tx.vout[i];
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            PublicCoin pubCoin(Params().Zerocoin_Params());
            if (!TxOutToPublicCoin(txOut, pubCoin, state))
                return false;

            listPubcoins.emplace_back(pubCoin);
        }
    }

    return true;
}

//return a list of zerocoin mints contained in a specific block
bool BlockToZerocoinMintList(const CBlock& block, std::list<CZerocoinMint>& vMints, bool fFilterInvalid)
{
    for (const CTransaction tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        // Filter out mints that have used invalid outpoints
        if (fFilterInvalid) {
            bool fValid = true;
            for (const CTxIn in : tx.vin) {
                if (!ValidOutPoint(in.prevout, INT_MAX)) {
                    fValid = false;
                    break;
                }
            }
            if (!fValid)
                continue;
        }

        uint256 txHash = tx.GetHash();
        for (unsigned int i = 0; i < tx.vout.size(); i++) {
            //Filter out mints that use invalid outpoints - edge case: invalid spend with minted change
            if (fFilterInvalid && !ValidOutPoint(COutPoint(txHash, i), INT_MAX))
                break;

            const CTxOut txOut = tx.vout[i];
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            PublicCoin pubCoin(Params().Zerocoin_Params());
            if (!TxOutToPublicCoin(txOut, pubCoin, state))
                return false;

            CZerocoinMint mint = CZerocoinMint(pubCoin.getDenomination(), pubCoin.getValue(), 0, 0, false);
            mint.SetTxHash(tx.GetHash());
            vMints.push_back(mint);
        }
    }

    return true;
}

bool BlockToMintValueVector(const CBlock& block, const CoinDenomination denom, vector<CBigNum>& vValues)
{
    for (const CTransaction tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        for (const CTxOut txOut : tx.vout) {
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            PublicCoin coin(Params().Zerocoin_Params());
            if (!TxOutToPublicCoin(txOut, coin, state))
                return false;

            if (coin.getDenomination() != denom)
                continue;

            vValues.push_back(coin.getValue());
        }
    }

    return true;
}

//return a list of zerocoin spends contained in a specific block, list may have many denominations
std::list<libzerocoin::CoinDenomination> ZerocoinSpendListFromBlock(const CBlock& block, bool fFilterInvalid)
{
    std::list<libzerocoin::CoinDenomination> vSpends;
    for (const CTransaction tx : block.vtx) {
        if (!tx.IsZerocoinSpend())
            continue;

        for (const CTxIn txin : tx.vin) {
            if (!txin.scriptSig.IsZerocoinSpend())
                continue;

            if (fFilterInvalid) {
                CoinSpend spend = TxInToZerocoinSpend(txin);
                if (mapInvalidSerials.count(spend.getCoinSerialNumber()))
                    continue;
            }

            libzerocoin::CoinDenomination c = libzerocoin::IntToZerocoinDenomination(txin.nSequence);
            vSpends.push_back(c);
        }
    }
    return vSpends;
}

bool CheckZerocoinMint(const uint256& txHash, const CTxOut& txout, CValidationState& state, bool fCheckOnly)
{
    PublicCoin pubCoin(Params().Zerocoin_Params());
    if(!TxOutToPublicCoin(txout, pubCoin, state))
        return state.DoS(100, error("CheckZerocoinMint(): TxOutToPublicCoin() failed"));

    if (!pubCoin.validate())
        return state.DoS(100, error("CheckZerocoinMint() : PubCoin does not validate"));

    if(!fCheckOnly && !RecordMintToDB(pubCoin, txHash))
        return state.DoS(100, error("CheckZerocoinMint(): RecordMintToDB() failed"));

    return true;
}

CoinSpend TxInToZerocoinSpend(const CTxIn& txin)
{
    // Deserialize the CoinSpend intro a fresh object
    std::vector<char, zero_after_free_allocator<char> > dataTxIn;
    dataTxIn.insert(dataTxIn.end(), txin.scriptSig.begin() + BIGNUM_SIZE, txin.scriptSig.end());

    CDataStream serializedCoinSpend(dataTxIn, SER_NETWORK, PROTOCOL_VERSION);
    return CoinSpend(Params().Zerocoin_Params(), serializedCoinSpend);
}

//Check a zerocoinspend considering external context such as blockchain data, height, etc.
bool ContextualCheckCoinSpend(const CoinSpend& spend, CBlockIndex* pindex, const uint256& txid, bool fSkipSerialCheck)
{
    // Make sure that the serial number is in valid range
    if (pindex->nHeight >= Params().Zerocoin_Block_EnforceSerialRange()) {
        if (!spend.HasValidSerial(Params().Zerocoin_Params()))
            return error("%s : txid=%s in block %d contains invalid serial %s\n", __func__, txid.GetHex(),
                         pindex->nHeight, spend.getCoinSerialNumber());
    }

    //Is the serial already in the blockchain?
    if (!fSkipSerialCheck) {
        int nHeightTxSpend = 0;
        if (IsSerialInBlockchain(spend.getCoinSerialNumber(), nHeightTxSpend)) {
            if(!fVerifyingBlocks || (fVerifyingBlocks && pindex->nHeight > nHeightTxSpend))
                return error("%s : zCOLX with serial %s is already in the block %d\n", __func__,
                             spend.getCoinSerialNumber().GetHex(), nHeightTxSpend);
        }
    }

    return true;
}

bool CheckZerocoinSpend(const CTransaction tx, bool fVerifySignature, CValidationState& state)
{
    //max needed non-mint outputs should be 2 - one for redemption address and a possible 2nd for change
    if (tx.vout.size() > 2) {
        int outs = 0;
        for (const CTxOut out : tx.vout) {
            if (out.IsZerocoinMint())
                continue;
            outs++;
        }
        if (outs > 2)
            return state.DoS(100, error("CheckZerocoinSpend(): over two non-mint outputs in a zerocoinspend transaction"));
    }

    //compute the txout hash that is used for the zerocoinspend signatures
    CMutableTransaction txTemp;
    for (const CTxOut out : tx.vout) {
        txTemp.vout.push_back(out);
    }
    uint256 hashTxOut = txTemp.GetHash();

    bool fValidated = false;
    set<CBigNum> serials;
    list<CoinSpend> vSpends;
    CAmount nTotalRedeemed = 0;
    for (const CTxIn& txin : tx.vin) {

        //only check txin that is a zcspend
        if (!txin.scriptSig.IsZerocoinSpend())
            continue;

        CoinSpend newSpend = TxInToZerocoinSpend(txin);
        vSpends.push_back(newSpend);

        //check that the denomination is valid
        if (newSpend.getDenomination() == ZQ_ERROR)
            return state.DoS(100, error("Zerocoinspend does not have the correct denomination"));

        //check that denomination is what it claims to be in nSequence
        if (newSpend.getDenomination() != txin.nSequence)
            return state.DoS(100, error("Zerocoinspend nSequence denomination does not match CoinSpend"));

        //make sure the txout has not changed
        if (newSpend.getTxOutHash() != hashTxOut)
            return state.DoS(100, error("Zerocoinspend does not use the same txout that was used in the SoK"));

        // Skip signature verification during initial block download
        if (fVerifySignature) {
            //see if we have record of the accumulator used in the spend tx
            CBigNum bnAccumulatorValue = 0;
            if(!zerocoinDB->ReadAccumulatorValue(newSpend.getAccumulatorChecksum(), bnAccumulatorValue))
                return state.DoS(100, error("Zerocoinspend could not find accumulator associated with checksum"));

            Accumulator accumulator(Params().Zerocoin_Params(), newSpend.getDenomination(), bnAccumulatorValue);

            //Check that the coin is on the accumulator
            if(!newSpend.Verify(accumulator))
                return state.DoS(100, error("CheckZerocoinSpend(): zerocoin spend did not verify"));
        }

        if (serials.count(newSpend.getCoinSerialNumber()))
            return state.DoS(100, error("Zerocoinspend serial is used twice in the same tx"));
        serials.insert(newSpend.getCoinSerialNumber());

        //make sure that there is no over redemption of coins
        nTotalRedeemed += ZerocoinDenominationToAmount(newSpend.getDenomination());
        fValidated = true;
    }

    if (nTotalRedeemed < tx.GetValueOut()) {
        LogPrintf("redeemed = %s , spend = %s \n", FormatMoney(nTotalRedeemed), FormatMoney(tx.GetValueOut()));
        return state.DoS(100, error("Transaction spend more than was redeemed in zerocoins"));
    }

    // Send signal to wallet if this is ours
    if (pwalletMain) {
        CWalletDB walletdb(pwalletMain->strWalletFile);
        list <CBigNum> listMySerials = walletdb.ListMintedCoinsSerial();
        for (const auto& newSpend : vSpends) {
            list<CBigNum>::iterator it = find(listMySerials.begin(), listMySerials.end(), newSpend.getCoinSerialNumber());
            if (it != listMySerials.end()) {
                LogPrintf("%s: %s detected spent zerocoin mint in transaction %s \n", __func__, it->GetHex(), tx.GetHash().GetHex());
                pwalletMain->NotifyZerocoinChanged(pwalletMain, it->GetHex(), "Used", CT_UPDATED);
            }
        }
    }

    return fValidated;
}

bool CheckTransaction(const CTransaction& tx, bool fZerocoinActive, CValidationState& state)
{
    // Basic checks that don't depend on any context
    if (tx.vin.empty())
        return state.DoS(10, error("CheckTransaction() : vin empty"),
            REJECT_INVALID, "bad-txns-vin-empty");
    if (tx.vout.empty())
        return state.DoS(10, error("CheckTransaction() : vout empty"),
            REJECT_INVALID, "bad-txns-vout-empty");

    // Size limits
    unsigned int nMaxSize = MAX_ZEROCOIN_TX_SIZE;

    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION) > nMaxSize)
        return state.DoS(100, error("CheckTransaction() : size limits failed"),
            REJECT_INVALID, "bad-txns-oversize");

    // Check for negative or overflow output values
    CAmount nValueOut = 0;
    int nZCSpendCount = 0;
    BOOST_FOREACH (const CTxOut& txout, tx.vout) {
        if (txout.IsEmpty() && !tx.IsCoinBase() && !tx.IsCoinStake())
            return state.DoS(100, error("CheckTransaction(): txout empty for user transaction"));

        if (txout.nValue < 0)
            return state.DoS(100, error("CheckTransaction() : txout.nValue negative"),
                REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > Params().MaxMoneyOut())
            return state.DoS(100, error("CheckTransaction() : txout.nValue too high"),
                REJECT_INVALID, "bad-txns-vout-toolarge");
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, error("CheckTransaction() : txout total out of range"),
                REJECT_INVALID, "bad-txns-txouttotal-toolarge");
        if (fZerocoinActive && txout.IsZerocoinMint()) {
            if(!CheckZerocoinMint(tx.GetHash(), txout, state, false))
                return state.DoS(100, error("CheckTransaction() : invalid zerocoin mint"));
        }
        if (fZerocoinActive && txout.scriptPubKey.IsZerocoinSpend())
            nZCSpendCount++;
    }

    if (fZerocoinActive) {
        if (nZCSpendCount > Params().Zerocoin_MaxSpendsPerTransaction())
            return state.DoS(100, error("CheckTransaction() : there are more zerocoin spends than are allowed in one transaction"));

        if (tx.IsZerocoinSpend()) {
            //require that a zerocoinspend only has inputs that are zerocoins
            for (const CTxIn in : tx.vin) {
                if (!in.scriptSig.IsZerocoinSpend())
                    return state.DoS(100,
                                     error("CheckTransaction() : zerocoinspend contains inputs that are not zerocoins"));
            }

            // Do not require signature verification if this is initial sync and a block over 24 hours old
            bool fVerifySignature = !IsInitialBlockDownload() && (GetTime() - chainActive.Tip()->GetBlockTime() < (60*60*24));
            if (!CheckZerocoinSpend(tx, fVerifySignature, state))
                return state.DoS(100, error("CheckTransaction() : invalid zerocoin spend"));
        }
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    set<CBigNum> vZerocoinSpendSerials;
    for (const CTxIn& txin : tx.vin) {
        if (vInOutPoints.count(txin.prevout))
            return state.DoS(100, error("CheckTransaction() : duplicate inputs"),
                REJECT_INVALID, "bad-txns-inputs-duplicate");

        //duplicate zcspend serials are checked in CheckZerocoinSpend()
        if (!txin.scriptSig.IsZerocoinSpend())
            vInOutPoints.insert(txin.prevout);
    }

    if (tx.IsCoinBase()) {
        if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 150)
            return state.DoS(100, error("CheckTransaction() : coinbase script size=%d", tx.vin[0].scriptSig.size()),
                REJECT_INVALID, "bad-cb-length");
    } else if (fZerocoinActive && tx.IsZerocoinSpend()) {
        if(tx.vin.size() < 1 || static_cast<int>(tx.vin.size()) > Params().Zerocoin_MaxSpendsPerTransaction())
            return state.DoS(10, error("CheckTransaction() : Zerocoin Spend has more than allowed txin's"), REJECT_INVALID, "bad-zerocoinspend");
    } else {
        BOOST_FOREACH (const CTxIn& txin, tx.vin)
            if (txin.prevout.IsNull() && (fZerocoinActive && !txin.scriptSig.IsZerocoinSpend()))
                return state.DoS(10, error("CheckTransaction() : prevout is null"),
                    REJECT_INVALID, "bad-txns-prevout-null");
    }

    return true;
}

bool CheckFinalTx(const CTransaction& tx, int flags)
{
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST) ? chainActive.Tip()->GetMedianTimePast() : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

CAmount GetMinRelayFee(const CTransaction& tx, unsigned int nBytes, bool fAllowFree)
{
    {
        LOCK(mempool.cs);
        uint256 hash = tx.GetHash();
        double dPriorityDelta = 0;
        CAmount nFeeDelta = 0;
        mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
        if (dPriorityDelta > 0 || nFeeDelta > 0)
            return 0;
    }

    CAmount nMinFee = ::minRelayTxFee.GetFee(nBytes);

    if (fAllowFree) {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        if (nBytes < (DEFAULT_BLOCK_PRIORITY_SIZE - 1000))
            nMinFee = 0;
    }

    if (!MoneyRange(nMinFee))
        nMinFee = Params().MaxMoneyOut();
    return nMinFee;
}


bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState& state, const CTransaction& tx, bool fLimitFree, bool* pfMissingInputs, bool fRejectInsaneFee, bool ignoreFees)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    //Temporarily disable zerocoin for maintenance
    if (GetAdjustedTime() > GetSporkValue(SPORK_20_ZEROCOIN_MAINTENANCE_MODE) && tx.ContainsZerocoins())
        return state.DoS(10, error("AcceptToMemoryPool : Zerocoin transactions are temporarily disabled for maintenance"), REJECT_INVALID, "bad-tx");

    if (!CheckTransaction(tx, chainActive.Height() >= Params().Zerocoin_StartHeight(), state))
        return state.DoS(100, error("AcceptToMemoryPool: : CheckTransaction failed"), REJECT_INVALID, "bad-tx");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("AcceptToMemoryPool: : coinbase as individual tx"),
            REJECT_INVALID, "coinbase");

    //Coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return state.DoS(100, error("AcceptToMemoryPool: coinstake as individual tx"),
            REJECT_INVALID, "coinstake");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    string reason;
    if (Params().RequireStandard() && !IsStandardTx(tx, reason))
        return state.DoS(0,
            error("AcceptToMemoryPool : nonstandard transaction: %s", reason),
            REJECT_NONSTANDARD, reason);
    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash)) {
        LogPrintf("%s tx already in mempool\n", __func__);
        return false;
    }

    // ----------- swiftTX transaction scanning -----------

    BOOST_FOREACH (const CTxIn& in, tx.vin) {
        if (mapLockedInputs.count(in.prevout)) {
            if (mapLockedInputs[in.prevout] != tx.GetHash()) {
                return state.DoS(0,
                    error("AcceptToMemoryPool : conflicts with existing transaction lock: %s", reason),
                    REJECT_INVALID, "tx-lock-conflict");
            }
        }
    }

    // Check for conflicts with in-memory transactions
    if (!tx.IsZerocoinSpend()) {
        LOCK(pool.cs); // protect pool.mapNextTx
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            COutPoint outpoint = tx.vin[i].prevout;
            if (pool.mapNextTx.count(outpoint)) {
                // Disable replacement feature for now
                return false;
            }
        }
    }


    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        uint256 txid = tx.GetHash();
        if(tx.IsZerocoinSpend()){
            nValueIn = tx.GetZerocoinSpent();

            //Check that txid is not already in the chain
            int nHeightTx = 0;
            if (IsTransactionInChain(tx.GetHash(), nHeightTx))
                return state.Invalid(error("AcceptToMemoryPool : zCOLX spend tx %s already in block %d", tx.GetHash().GetHex(), nHeightTx),
                                     REJECT_DUPLICATE, "bad-txns-inputs-spent");

            //Check for double spending of serial #'s
            for (const CTxIn& txIn : tx.vin) {
                if (!txIn.scriptSig.IsZerocoinSpend())
                    continue;
                CoinSpend spend = TxInToZerocoinSpend(txIn);
                if (!ContextualCheckCoinSpend(spend, chainActive.Tip(), txid))
                    return state.Invalid(error("%s: zCOLX spend in tx %s failed to pass context checks", __func__, txid.GetHex()));
            }
        } else {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            view.SetBackend(viewMemPool);

            // do we already have it?
            if (view.HaveCoins(hash))
                return false;

            // do all inputs exist?
            // Note that this does not check for the presence of actual outputs (see the next check for that),
            // only helps filling in pfMissingInputs (to determine missing vs spent).
            for (const CTxIn txin : tx.vin) {
                if (!view.HaveCoins(txin.prevout.hash)) {
                    if (pfMissingInputs)
                        *pfMissingInputs = true;
                    return false;
                }

                //Check for invalid/fraudulent inputs
                if (!ValidOutPoint(txin.prevout, chainActive.Height())) {
                    return state.Invalid(error("%s : tried to spend invalid input %s in tx %s", __func__, txin.prevout.ToString(),
                                                tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                }
            }

            // are the actual inputs available?
            if (!view.HaveInputs(tx))
                return state.Invalid(error("AcceptToMemoryPool : inputs already spent"),
                    REJECT_DUPLICATE, "bad-txns-inputs-spent");

            // Bring the best block into scope
            view.GetBestBlock();

            nValueIn = view.GetValueIn(tx);

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (Params().RequireStandard() && !AreInputsStandard(tx, view))
            return error("AcceptToMemoryPool: : nonstandard transaction input");

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        if (!tx.IsZerocoinSpend()) {
            unsigned int nSigOps = GetLegacySigOpCount(tx);
            unsigned int nMaxSigOps = MAX_TX_SIGOPS_CURRENT;
            nSigOps += GetP2SHSigOpCount(tx, view);
            if(nSigOps > nMaxSigOps)
                return state.DoS(0,
                                 error("AcceptToMemoryPool : too many sigops %s, %d > %d",
                                       hash.ToString(), nSigOps, nMaxSigOps),
                                 REJECT_NONSTANDARD, "bad-txns-too-many-sigops");
        }

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = nValueIn - nValueOut;
        double dPriority = 0;
        if (!tx.IsZerocoinSpend())
            view.GetPriority(tx, chainActive.Height());

        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height());
        unsigned int nSize = entry.GetTxSize();

        // Don't accept it if it can't get into a block
        // but prioritise dstx and don't check fees for it
        if (mapObfuscationBroadcastTxes.count(hash)) {
            mempool.PrioritiseTransaction(hash, hash.ToString(), 1000, 0.1 * COIN);
        } else if (!ignoreFees) {
            CAmount txMinFee = GetMinRelayFee(tx, nSize, true);
            if (fLimitFree && nFees < txMinFee && !tx.IsZerocoinSpend())
                return state.DoS(0, error("AcceptToMemoryPool : not enough fees %s, %d < %d",
                                        hash.ToString(), nFees, txMinFee),
                    REJECT_INSUFFICIENTFEE, "insufficient fee");

            // Require that free transactions have sufficient priority to be mined in the next block.
            if (tx.IsZerocoinMint()) {
                if(nFees < Params().Zerocoin_MintFee() * tx.GetZerocoinMintCount())
                    return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient fee for zerocoinmint");
            } else if (!tx.IsZerocoinSpend() && GetBoolArg("-relaypriority", true) && nFees < ::minRelayTxFee.GetFee(nSize) && !AllowFree(view.GetPriority(tx, chainActive.Height() + 1))) {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient priority");
            }

            // Continuously rate-limit free (really, very-low-fee) transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (fLimitFree && nFees < ::minRelayTxFee.GetFee(nSize) && !tx.IsZerocoinSpend()) {
                static CCriticalSection csFreeLimiter;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                LOCK(csFreeLimiter);

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount >= GetArg("-limitfreerelay", 30) * 10 * 1000)
                    return state.DoS(0, error("AcceptToMemoryPool : free transaction rejected by rate limiter"),
                        REJECT_INSUFFICIENTFEE, "rate limited free transaction");
                LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount + nSize);
                dFreeCount += nSize;
            }
        }

        if (fRejectInsaneFee && nFees > ::minRelayTxFee.GetFee(nSize) * 10000)
            return error("AcceptToMemoryPool: : insane fees %s, %d > %d",
                hash.ToString(),
                nFees, ::minRelayTxFee.GetFee(nSize) * 10000);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!CheckInputs(tx, state, view, true, STANDARD_SCRIPT_VERIFY_FLAGS, true)) {
            return error("AcceptToMemoryPool: : ConnectInputs failed %s", hash.ToString());
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        if (!CheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true)) {
            return error("AcceptToMemoryPool: : BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s", hash.ToString());
        }

        // Store transaction in memory
        pool.addUnchecked(hash, entry);
    }

    SyncWithWallets(tx, NULL);

    //Track zerocoinspends and ensure that they are given priority to make it into the blockchain
    if (tx.IsZerocoinSpend())
        mapZerocoinspends[tx.GetHash()] = GetAdjustedTime();

    return true;
}

bool AcceptableInputs(CTxMemPool& pool, CValidationState& state, const CTransaction& tx, bool fLimitFree, bool* pfMissingInputs, bool fRejectInsaneFee, bool isDSTX)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;


    if (!CheckTransaction(tx, chainActive.Height() >= Params().Zerocoin_StartHeight(), state))
        return error("AcceptableInputs: : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("AcceptableInputs: : coinbase as individual tx"),
            REJECT_INVALID, "coinbase");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    string reason;
    // for any real tx this will be checked on AcceptToMemoryPool anyway
    //    if (Params().RequireStandard() && !IsStandardTx(tx, reason))
    //        return state.DoS(0,
    //                         error("AcceptableInputs : nonstandard transaction: %s", reason),
    //                         REJECT_NONSTANDARD, reason);

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    // ----------- swiftTX transaction scanning -----------

    BOOST_FOREACH (const CTxIn& in, tx.vin) {
        if (mapLockedInputs.count(in.prevout)) {
            if (mapLockedInputs[in.prevout] != tx.GetHash()) {
                return state.DoS(0,
                    error("AcceptableInputs : conflicts with existing transaction lock: %s", reason),
                    REJECT_INVALID, "tx-lock-conflict");
            }
        }
    }

    // Check for conflicts with in-memory transactions
    if (!tx.IsZerocoinSpend()) {
        LOCK(pool.cs); // protect pool.mapNextTx
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            COutPoint outpoint = tx.vin[i].prevout;
            if (pool.mapNextTx.count(outpoint)) {
                // Disable replacement feature for now
                return false;
            }
        }
    }


    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            view.SetBackend(viewMemPool);

            // do we already have it?
            if (view.HaveCoins(hash))
                return false;

            // do all inputs exist?
            // Note that this does not check for the presence of actual outputs (see the next check for that),
            // only helps filling in pfMissingInputs (to determine missing vs spent).
            for (const CTxIn txin : tx.vin) {
                if (!view.HaveCoins(txin.prevout.hash)) {
                    if (pfMissingInputs)
                        *pfMissingInputs = true;
                    return false;
                }

                // check for invalid/fraudulent inputs
                if (!ValidOutPoint(txin.prevout, chainActive.Height())) {
                    return state.Invalid(error("%s : tried to spend invalid input %s in tx %s", __func__, txin.prevout.ToString(),
                                                tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                }
            }

            // are the actual inputs available?
            if (!view.HaveInputs(tx))
                return state.Invalid(error("AcceptableInputs : inputs already spent"),
                    REJECT_DUPLICATE, "bad-txns-inputs-spent");

            // Bring the best block into scope
            view.GetBestBlock();

            nValueIn = view.GetValueIn(tx);

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);
        }

        // Check for non-standard pay-to-script-hash in inputs
        // for any real tx this will be checked on AcceptToMemoryPool anyway
        //        if (Params().RequireStandard() && !AreInputsStandard(tx, view))
        //            return error("AcceptableInputs: : nonstandard transaction input");

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        unsigned int nSigOps = GetLegacySigOpCount(tx);
        unsigned int nMaxSigOps = MAX_TX_SIGOPS_CURRENT;
        nSigOps += GetP2SHSigOpCount(tx, view);
        if (nSigOps > nMaxSigOps)
            return state.DoS(0,
                error("AcceptableInputs : too many sigops %s, %d > %d",
                    hash.ToString(), nSigOps, nMaxSigOps),
                REJECT_NONSTANDARD, "bad-txns-too-many-sigops");

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = nValueIn - nValueOut;
        double dPriority = view.GetPriority(tx, chainActive.Height());

        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height());
        unsigned int nSize = entry.GetTxSize();

        // Don't accept it if it can't get into a block
        // but prioritise dstx and don't check fees for it
        if (isDSTX) {
            mempool.PrioritiseTransaction(hash, hash.ToString(), 1000, 0.1 * COIN);
        } else { // same as !ignoreFees for AcceptToMemoryPool
            CAmount txMinFee = GetMinRelayFee(tx, nSize, true);
            if (fLimitFree && nFees < txMinFee && !tx.IsZerocoinSpend())
                return state.DoS(0, error("AcceptableInputs : not enough fees %s, %d < %d",
                                        hash.ToString(), nFees, txMinFee),
                    REJECT_INSUFFICIENTFEE, "insufficient fee");

            // Require that free transactions have sufficient priority to be mined in the next block.
            CAmount txRelayFee = ::minRelayTxFee.GetFee(nSize);
            if (GetBoolArg("-relaypriority", true) && nFees < txRelayFee && !AllowFree(view.GetPriority(tx, chainActive.Height() + 1))) {
                return state.DoS(0, error("AcceptableInputs : not enough fees %s < %s", FormatMoney(nFees), FormatMoney(txRelayFee)),
                                 REJECT_INSUFFICIENTFEE, "insufficient priority");
            }

            // Continuously rate-limit free (really, very-low-fee) transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (fLimitFree && nFees < ::minRelayTxFee.GetFee(nSize) && !tx.IsZerocoinSpend()) {
                static CCriticalSection csFreeLimiter;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                LOCK(csFreeLimiter);

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount >= GetArg("-limitfreerelay", 30) * 10 * 1000)
                    return state.DoS(0, error("AcceptableInputs : free transaction rejected by rate limiter"),
                        REJECT_INSUFFICIENTFEE, "rate limited free transaction");
                LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount + nSize);
                dFreeCount += nSize;
            }
        }

        if (fRejectInsaneFee && nFees > ::minRelayTxFee.GetFee(nSize) * 10000)
            return error("AcceptableInputs: : insane fees %s, %d > %d",
                hash.ToString(),
                nFees, ::minRelayTxFee.GetFee(nSize) * 10000);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!CheckInputs(tx, state, view, false, STANDARD_SCRIPT_VERIFY_FLAGS, true)) {
            return error("AcceptableInputs: : ConnectInputs failed %s", hash.ToString());
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        // for any real tx this will be checked on AcceptToMemoryPool anyway
        //        if (!CheckInputs(tx, state, view, false, MANDATORY_SCRIPT_VERIFY_FLAGS, true))
        //        {
        //            return error("AcceptableInputs: : BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s", hash.ToString());
        //        }

        // Store transaction in memory
        // pool.addUnchecked(hash, entry);
    }

    // SyncWithWallets(tx, NULL);

    return true;
}

/** Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock */
bool GetTransaction(const uint256& hash, CTransaction& txOut, uint256& hashBlock, bool fAllowSlow)
{
    CBlockIndex* pindexSlow = NULL;
    {
        // DLOCKSFIX: // SEE: masternode-budget.cpp:Read(...)
        LOCK(cs_main);
        {
            if (mempool.lookup(hash, txOut)) {
                return true;
            }
        }

        if (fTxIndex) {
            CDiskTxPos postx;
            if (pblocktree->ReadTxIndex(hash, postx)) {
                CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
                if (file.IsNull())
                    return error("%s: OpenBlockFile failed", __func__);
                CBlockHeader header;
                try {
                    file >> header;
                    fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                    file >> txOut;
                } catch (std::exception& e) {
                    return error("%s : Deserialize or I/O error - %s", __func__, e.what());
                }
                hashBlock = header.GetHash();
                if (txOut.GetHash() != hash)
                    return error("%s : txid mismatch", __func__);
                return true;
            }

            // transaction not found in the index, nothing more can be done
            return false;
        }

        if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
            int nHeight = -1;
            {
                CCoinsViewCache& view = *pcoinsTip;
                const CCoins* coins = view.AccessCoins(hash);
                if (coins)
                    nHeight = coins->nHeight;
            }
            if (nHeight > 0)
                pindexSlow = chainActive[nHeight];
        }
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow)) {
            BOOST_FOREACH (const CTransaction& tx, block.vtx) {
                if (tx.GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}


//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool WriteBlockToDisk(CBlock& block, CDiskBlockPos& pos)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk : OpenBlockFile failed");

    // Write index header
    unsigned int nSize = fileout.GetSerializeSize(block);
    fileout << FLATDATA(Params().MessageStart()) << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk : ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk : OpenBlockFile failed");

    // Read block
    try {
        filein >> block;
    } catch (std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }

    // Check the header
    if (block.IsProofOfWork()) {
        if (block.GetHash() != Params().HashGenesisBlock() && !CheckProofOfWork(block.GetHash(), block.nBits))
            return error("ReadBlockFromDisk : Errors in block header");
    }

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex)
{
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos()))
        return false;
    if (block.GetHash() != pindex->GetBlockHash()) {
        LogPrintf("%s : block=%s index=%s\n", __func__, block.GetHash().ToString().c_str(), pindex->GetBlockHash().ToString().c_str());
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*) : GetHash() doesn't match index");
    }
    return true;
}


double ConvertBitsToDouble(unsigned int nBits)
{
    int nShift = (nBits >> 24) & 0xff;

    double dDiff =
        (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

CAmount GetBlockExpectedMint(int nHeight)
{
    return GetBlockValueReward(nHeight);
}

CAmount GetBlockValue(int nHeight)
{
    /**
     * Block 1: 12 Billions COLX pre-mined
     *
     * Block Reward:
     * Blocks 2 - 151201: 2500 COLX
     * Blocks 151202 - 302400: 1250 COLX
     * Blocks 302401 - 388799: 1000 COLX
     * Blocks 388800 - Infinite: 1500 COLX
     *
     * Proof of Stake Schedule before 388800:
     * 5% to proposals for all phases
     * 95% to reward distributed to stake wallet and master node
     *
     * Proof of Stake Schedule since 388800:
     * 10% to proposals for all phases
     * 10% to def fund address
     * 80% to reward distributed to stake wallet and master node
     */

    if (nHeight < Params().GetChainHeight(ChainHeight::H1))
        return 0;
    else if (nHeight == Params().GetChainHeight(ChainHeight::H1))
        return CAmount(12000000000) * COIN;
    else if (nHeight < Params().GetChainHeight(ChainHeight::H2))
        return 2500 * COIN;
    else if (nHeight < Params().GetChainHeight(ChainHeight::H3))
        return 1250 * COIN;
    else if (nHeight < Params().GetChainHeight(ChainHeight::H4))
        return 1000 * COIN;
    else
        return 1500 * COIN;
}

CAmount GetBlockValueReward(int nHeight)
{
    if (nHeight == Params().GetChainHeight(ChainHeight::H1))
        return GetBlockValue(nHeight); //premine has no budget allocation
    else if (nHeight < Params().GetChainHeight(ChainHeight::H4))
        return GetBlockValue(nHeight) * (100 - 5) / 100; // 5% budget
    else if (nHeight < Params().GetChainHeight(ChainHeight::H8))
        return GetBlockValue(nHeight) * (100 - Params().GetBudgetPercent()) / 100; // 10% budget
    else
        return GetBlockValue(nHeight) * (100 - Params().GetBudgetPercent() - Params().GetDevFundPercent()) / 100; // dev fund is not included in the block reward
}

CAmount GetBlockValueBudget(int nHeight)
{
    if (nHeight == Params().GetChainHeight(ChainHeight::H1))
        return 0; //premine has no budget allocation
    else if (nHeight < Params().GetChainHeight(ChainHeight::H4))
        return GetBlockValue(nHeight) * 5 / 100; // 5% budget
    else
        return GetBlockValue(nHeight) * Params().GetBudgetPercent() / 100; // 10% budget
}

CAmount GetBlockValueDevFund(int nHeight)
{
    if (nHeight < Params().GetChainHeight(ChainHeight::H4))
        return 0; // no dev fund allocation
    else
        return GetBlockValue(nHeight) * Params().GetDevFundPercent() / 100; // 10% dev fund
}

CAmount GetMasternodePayment(int nHeight, int nMasternodeCount, CAmount nMoneySupply)
{
    if (nHeight < Params().GetChainHeight(ChainHeight::H4))
        return GetBlockValueReward(nHeight) * 60 / 100; // old rules 60% goes to the masternode
    else if (nHeight >= Params().GetChainHeight(ChainHeight::H6))
        return (GetBlockValueReward(nHeight) - GetBlockValueDevFund(nHeight)) * 60 / 100; // 60% goes to the masternode again
    else if (nHeight >= Params().GetChainHeight(ChainHeight::H8))
        return GetBlockValueReward(nHeight) * 60 / 100; // dev fund is not included in the reward anymore
    else; // see-saw algorithm [H4; H6)

    const CAmount nReward = GetBlockValueReward(nHeight) - GetBlockValueDevFund(nHeight);
    const CAmount nNodeCoins = nMasternodeCount * Params().GetRequiredMasternodeCollateral();

    DebugPrintf("Adjusting seesaw at height %d with %d masternodes at %ld\n", nHeight, nMasternodeCount, GetTime());
    DebugPrintf("GetMasternodePayment(): moneysupply=%s, nodecoins=%s, reward=%s\n", FormatMoney(nMoneySupply), FormatMoney(nNodeCoins), FormatMoney(nReward));

    CAmount ret = 0;
    if (nNodeCoins == 0) {
        ret = 0;
    } else if (nNodeCoins <= (nMoneySupply * .01) && nNodeCoins > 0) {
        ret = nReward * .90;
    } else if (nNodeCoins <= (nMoneySupply * .02) && nNodeCoins > (nMoneySupply * .01)) {
        ret = nReward * .88;
    } else if (nNodeCoins <= (nMoneySupply * .03) && nNodeCoins > (nMoneySupply * .02)) {
        ret = nReward * .87;
    } else if (nNodeCoins <= (nMoneySupply * .04) && nNodeCoins > (nMoneySupply * .03)) {
        ret = nReward * .86;
    } else if (nNodeCoins <= (nMoneySupply * .05) && nNodeCoins > (nMoneySupply * .04)) {
        ret = nReward * .85;
    } else if (nNodeCoins <= (nMoneySupply * .06) && nNodeCoins > (nMoneySupply * .05)) {
        ret = nReward * .84;
    } else if (nNodeCoins <= (nMoneySupply * .07) && nNodeCoins > (nMoneySupply * .06)) {
        ret = nReward * .83;
    } else if (nNodeCoins <= (nMoneySupply * .08) && nNodeCoins > (nMoneySupply * .07)) {
        ret = nReward * .82;
    } else if (nNodeCoins <= (nMoneySupply * .09) && nNodeCoins > (nMoneySupply * .08)) {
        ret = nReward * .81;
    } else if (nNodeCoins <= (nMoneySupply * .10) && nNodeCoins > (nMoneySupply * .09)) {
        ret = nReward * .80;
    } else if (nNodeCoins <= (nMoneySupply * .11) && nNodeCoins > (nMoneySupply * .10)) {
        ret = nReward * .79;
    } else if (nNodeCoins <= (nMoneySupply * .12) && nNodeCoins > (nMoneySupply * .11)) {
        ret = nReward * .78;
    } else if (nNodeCoins <= (nMoneySupply * .13) && nNodeCoins > (nMoneySupply * .12)) {
        ret = nReward * .77;
    } else if (nNodeCoins <= (nMoneySupply * .14) && nNodeCoins > (nMoneySupply * .13)) {
        ret = nReward * .76;
    } else if (nNodeCoins <= (nMoneySupply * .15) && nNodeCoins > (nMoneySupply * .14)) {
        ret = nReward * .75;
    } else if (nNodeCoins <= (nMoneySupply * .16) && nNodeCoins > (nMoneySupply * .15)) {
        ret = nReward * .74;
    } else if (nNodeCoins <= (nMoneySupply * .17) && nNodeCoins > (nMoneySupply * .16)) {
        ret = nReward * .73;
    } else if (nNodeCoins <= (nMoneySupply * .18) && nNodeCoins > (nMoneySupply * .17)) {
        ret = nReward * .72;
    } else if (nNodeCoins <= (nMoneySupply * .19) && nNodeCoins > (nMoneySupply * .18)) {
        ret = nReward * .71;
    } else if (nNodeCoins <= (nMoneySupply * .20) && nNodeCoins > (nMoneySupply * .19)) {
        ret = nReward * .70;
    } else if (nNodeCoins <= (nMoneySupply * .21) && nNodeCoins > (nMoneySupply * .20)) {
        ret = nReward * .69;
    } else if (nNodeCoins <= (nMoneySupply * .22) && nNodeCoins > (nMoneySupply * .21)) {
        ret = nReward * .68;
    } else if (nNodeCoins <= (nMoneySupply * .23) && nNodeCoins > (nMoneySupply * .22)) {
        ret = nReward * .67;
    } else if (nNodeCoins <= (nMoneySupply * .24) && nNodeCoins > (nMoneySupply * .23)) {
        ret = nReward * .66;
    } else if (nNodeCoins <= (nMoneySupply * .25) && nNodeCoins > (nMoneySupply * .24)) {
        ret = nReward * .65;
    } else if (nNodeCoins <= (nMoneySupply * .26) && nNodeCoins > (nMoneySupply * .25)) {
        ret = nReward * .64;
    } else if (nNodeCoins <= (nMoneySupply * .27) && nNodeCoins > (nMoneySupply * .26)) {
        ret = nReward * .63;
    } else if (nNodeCoins <= (nMoneySupply * .28) && nNodeCoins > (nMoneySupply * .27)) {
        ret = nReward * .62;
    } else if (nNodeCoins <= (nMoneySupply * .29) && nNodeCoins > (nMoneySupply * .28)) {
        ret = nReward * .61;
    } else if (nNodeCoins <= (nMoneySupply * .30) && nNodeCoins > (nMoneySupply * .29)) {
        ret = nReward * .60;
    } else if (nNodeCoins <= (nMoneySupply * .31) && nNodeCoins > (nMoneySupply * .30)) {
        ret = nReward * .59;
    } else if (nNodeCoins <= (nMoneySupply * .32) && nNodeCoins > (nMoneySupply * .31)) {
        ret = nReward * .58;
    } else if (nNodeCoins <= (nMoneySupply * .33) && nNodeCoins > (nMoneySupply * .32)) {
        ret = nReward * .57;
    } else if (nNodeCoins <= (nMoneySupply * .34) && nNodeCoins > (nMoneySupply * .33)) {
        ret = nReward * .56;
    } else if (nNodeCoins <= (nMoneySupply * .35) && nNodeCoins > (nMoneySupply * .34)) {
        ret = nReward * .55;
    } else if (nNodeCoins <= (nMoneySupply * .363) && nNodeCoins > (nMoneySupply * .35)) {
        ret = nReward * .54;
    } else if (nNodeCoins <= (nMoneySupply * .376) && nNodeCoins > (nMoneySupply * .363)) {
        ret = nReward * .53;
    } else if (nNodeCoins <= (nMoneySupply * .389) && nNodeCoins > (nMoneySupply * .376)) {
        ret = nReward * .52;
    } else if (nNodeCoins <= (nMoneySupply * .402) && nNodeCoins > (nMoneySupply * .389)) {
        ret = nReward * .51;
    } else if (nNodeCoins <= (nMoneySupply * .415) && nNodeCoins > (nMoneySupply * .402)) {
        ret = nReward * .50;
    } else if (nNodeCoins <= (nMoneySupply * .428) && nNodeCoins > (nMoneySupply * .415)) {
        ret = nReward * .49;
    } else if (nNodeCoins <= (nMoneySupply * .441) && nNodeCoins > (nMoneySupply * .428)) {
        ret = nReward * .48;
    } else if (nNodeCoins <= (nMoneySupply * .454) && nNodeCoins > (nMoneySupply * .441)) {
        ret = nReward * .47;
    } else if (nNodeCoins <= (nMoneySupply * .467) && nNodeCoins > (nMoneySupply * .454)) {
        ret = nReward * .46;
    } else if (nNodeCoins <= (nMoneySupply * .48) && nNodeCoins > (nMoneySupply * .467)) {
        ret = nReward * .45;
    } else if (nNodeCoins <= (nMoneySupply * .493) && nNodeCoins > (nMoneySupply * .48)) {
        ret = nReward * .44;
    } else if (nNodeCoins <= (nMoneySupply * .506) && nNodeCoins > (nMoneySupply * .493)) {
        ret = nReward * .43;
    } else if (nNodeCoins <= (nMoneySupply * .519) && nNodeCoins > (nMoneySupply * .506)) {
        ret = nReward * .42;
    } else if (nNodeCoins <= (nMoneySupply * .532) && nNodeCoins > (nMoneySupply * .519)) {
        ret = nReward * .41;
    } else if (nNodeCoins <= (nMoneySupply * .545) && nNodeCoins > (nMoneySupply * .532)) {
        ret = nReward * .40;
    } else if (nNodeCoins <= (nMoneySupply * .558) && nNodeCoins > (nMoneySupply * .545)) {
        ret = nReward * .39;
    } else if (nNodeCoins <= (nMoneySupply * .571) && nNodeCoins > (nMoneySupply * .558)) {
        ret = nReward * .38;
    } else if (nNodeCoins <= (nMoneySupply * .584) && nNodeCoins > (nMoneySupply * .571)) {
        ret = nReward * .37;
    } else if (nNodeCoins <= (nMoneySupply * .597) && nNodeCoins > (nMoneySupply * .584)) {
        ret = nReward * .36;
    } else if (nNodeCoins <= (nMoneySupply * .61) && nNodeCoins > (nMoneySupply * .597)) {
        ret = nReward * .35;
    } else if (nNodeCoins <= (nMoneySupply * .623) && nNodeCoins > (nMoneySupply * .61)) {
        ret = nReward * .34;
    } else if (nNodeCoins <= (nMoneySupply * .636) && nNodeCoins > (nMoneySupply * .623)) {
        ret = nReward * .33;
    } else if (nNodeCoins <= (nMoneySupply * .649) && nNodeCoins > (nMoneySupply * .636)) {
        ret = nReward * .32;
    } else if (nNodeCoins <= (nMoneySupply * .662) && nNodeCoins > (nMoneySupply * .649)) {
        ret = nReward * .31;
    } else if (nNodeCoins <= (nMoneySupply * .675) && nNodeCoins > (nMoneySupply * .662)) {
        ret = nReward * .30;
    } else if (nNodeCoins <= (nMoneySupply * .688) && nNodeCoins > (nMoneySupply * .675)) {
        ret = nReward * .29;
    } else if (nNodeCoins <= (nMoneySupply * .701) && nNodeCoins > (nMoneySupply * .688)) {
        ret = nReward * .28;
    } else if (nNodeCoins <= (nMoneySupply * .714) && nNodeCoins > (nMoneySupply * .701)) {
        ret = nReward * .27;
    } else if (nNodeCoins <= (nMoneySupply * .727) && nNodeCoins > (nMoneySupply * .714)) {
        ret = nReward * .26;
    } else if (nNodeCoins <= (nMoneySupply * .74) && nNodeCoins > (nMoneySupply * .727)) {
        ret = nReward * .25;
    } else if (nNodeCoins <= (nMoneySupply * .753) && nNodeCoins > (nMoneySupply * .74)) {
        ret = nReward * .24;
    } else if (nNodeCoins <= (nMoneySupply * .766) && nNodeCoins > (nMoneySupply * .753)) {
        ret = nReward * .23;
    } else if (nNodeCoins <= (nMoneySupply * .779) && nNodeCoins > (nMoneySupply * .766)) {
        ret = nReward * .22;
    } else if (nNodeCoins <= (nMoneySupply * .792) && nNodeCoins > (nMoneySupply * .779)) {
        ret = nReward * .21;
    } else if (nNodeCoins <= (nMoneySupply * .805) && nNodeCoins > (nMoneySupply * .792)) {
        ret = nReward * .20;
    } else if (nNodeCoins <= (nMoneySupply * .818) && nNodeCoins > (nMoneySupply * .805)) {
        ret = nReward * .19;
    } else if (nNodeCoins <= (nMoneySupply * .831) && nNodeCoins > (nMoneySupply * .818)) {
        ret = nReward * .18;
    } else if (nNodeCoins <= (nMoneySupply * .844) && nNodeCoins > (nMoneySupply * .831)) {
        ret = nReward * .17;
    } else if (nNodeCoins <= (nMoneySupply * .857) && nNodeCoins > (nMoneySupply * .844)) {
        ret = nReward * .16;
    } else if (nNodeCoins <= (nMoneySupply * .87) && nNodeCoins > (nMoneySupply * .857)) {
        ret = nReward * .15;
    } else if (nNodeCoins <= (nMoneySupply * .883) && nNodeCoins > (nMoneySupply * .87)) {
        ret = nReward * .14;
    } else if (nNodeCoins <= (nMoneySupply * .896) && nNodeCoins > (nMoneySupply * .883)) {
        ret = nReward * .13;
    } else if (nNodeCoins <= (nMoneySupply * .909) && nNodeCoins > (nMoneySupply * .896)) {
        ret = nReward * .12;
    } else if (nNodeCoins <= (nMoneySupply * .922) && nNodeCoins > (nMoneySupply * .909)) {
        ret = nReward * .11;
    } else if (nNodeCoins <= (nMoneySupply * .935) && nNodeCoins > (nMoneySupply * .922)) {
        ret = nReward * .10;
    } else if (nNodeCoins <= (nMoneySupply * .945) && nNodeCoins > (nMoneySupply * .935)) {
        ret = nReward * .09;
    } else if (nNodeCoins <= (nMoneySupply * .961) && nNodeCoins > (nMoneySupply * .945)) {
        ret = nReward * .08;
    } else if (nNodeCoins <= (nMoneySupply * .974) && nNodeCoins > (nMoneySupply * .961)) {
        ret = nReward * .07;
    } else if (nNodeCoins <= (nMoneySupply * .987) && nNodeCoins > (nMoneySupply * .974)) {
        ret = nReward * .06;
    } else if (nNodeCoins <= (nMoneySupply * .99) && nNodeCoins > (nMoneySupply * .987)) {
        ret = nReward * .05;
    } else {
        ret = nReward * .01;
    }

    return ret;
}

bool IsInitialBlockDownload()
{
    LOCK(cs_main);
    if (fImporting || fReindex || fVerifyingBlocks || chainActive.Height() < Checkpoints::GetTotalBlocksEstimate())
        return true;
    static bool lockIBDState = false;
    if (lockIBDState)
        return false;
    // I2PTESTNET:    
    bool state = (chainActive.Height() < pindexBestHeader->nHeight - 24 * 6 ||
                  pindexBestHeader->GetBlockTime() < GetTime() - 6 * 60 * 60); // ~144 blocks behind -> 2 x fork detection time

    // I2PTESTNETFIX:
    if (Params().NetworkID() == CBaseChainParams::TESTNET && Params().IsBlockchainLateSynced())
        state = (chainActive.Height() < pindexBestHeader->nHeight - 24 * 6 ||
                 pindexBestHeader->GetBlockTime() < GetTime() - Params().GetBlockchainSyncedSeconds());

    if (!state)
        lockIBDState = true;
    return state;
}

bool fLargeWorkForkFound = false;
bool fLargeWorkInvalidChainFound = false;
CBlockIndex *pindexBestForkTip = NULL, *pindexBestForkBase = NULL;

void CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before the last checkpoint)
    if (IsInitialBlockDownload())
        return;

    // If our best fork is no longer within 72 blocks (+/- 3 hours if no one mines it)
    // of our head, drop it
    if (pindexBestForkTip && chainActive.Height() - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = NULL;

    if (pindexBestForkTip || (pindexBestInvalid && pindexBestInvalid->nChainWork > chainActive.Tip()->nChainWork + (GetBlockProof(*chainActive.Tip()) * 6))) {
        if (!fLargeWorkForkFound && pindexBestForkBase) {
            if (pindexBestForkBase->phashBlock) {
                std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                                      pindexBestForkBase->phashBlock->ToString() + std::string("'");
                CAlert::Notify(warning, true);
            }
        }
        if (pindexBestForkTip && pindexBestForkBase) {
            if (pindexBestForkBase->phashBlock) {
                LogPrintf("CheckForkWarningConditions: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n",
                    pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
                    pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
                fLargeWorkForkFound = true;
            }
        } else {
            LogPrintf("CheckForkWarningConditions: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n");
            fLargeWorkInvalidChainFound = true;
        }
    } else {
        fLargeWorkForkFound = false;
        fLargeWorkInvalidChainFound = false;
    }
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip)
{
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex* pfork = pindexNewForkTip;
    CBlockIndex* plonger = chainActive.Tip();
    while (pfork && pfork != plonger) {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
    }

    // We define a condition which we should warn the user about as a fork of at least 7 blocks
    // who's tip is within 72 blocks (+/- 3 hours if no one mines it) of ours
    // or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
    // We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
    // hash rate operating on the fork.
    // We define it this way because it allows us to only store the highest fork tip (+ base) which meets
    // the 7-block condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!pindexBestForkTip || (pindexBestForkTip && pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
        pindexNewForkTip->nChainWork - pfork->nChainWork > (GetBlockProof(*pfork) * 7) &&
        chainActive.Height() - pindexNewForkTip->nHeight < 72) {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions();
}

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;

    CNodeState* state = State(pnode);
    if (state == NULL)
        return;

    state->nMisbehavior += howmuch;
    int banscore = GetArg("-banscore", 100);
    if (state->nMisbehavior >= banscore && state->nMisbehavior - howmuch < banscore) {
        LogPrintf("Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", state->name, state->nMisbehavior - howmuch, state->nMisbehavior);
        state->fShouldBan = true;
    } else
        LogPrintf("Misbehaving: %s (%d -> %d)\n", state->name, state->nMisbehavior - howmuch, state->nMisbehavior);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (!pindexBestInvalid || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf("InvalidChainFound: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n",
        pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
        log(pindexNew->nChainWork.getdouble()) / log(2.0), DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
                                                               pindexNew->GetBlockTime()));
    LogPrintf("InvalidChainFound:  current best=%s  height=%d  log2_work=%.8g  date=%s\n",
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(), log(chainActive.Tip()->nChainWork.getdouble()) / log(2.0),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()));
    CheckForkWarningConditions();
}

void static InvalidBlockFound(CBlockIndex* pindex, const CValidationState& state)
{
    int nDoS = 0;
    if (state.IsInvalid(nDoS)) {
        std::map<uint256, NodeId>::iterator it = mapBlockSource.find(pindex->GetBlockHash());
        if (it != mapBlockSource.end() && State(it->second)) {
            CBlockReject reject = {state.GetRejectCode(), state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), pindex->GetBlockHash()};
            State(it->second)->rejects.push_back(reject);
            if (nDoS > 0)
                Misbehaving(it->second, nDoS);
        }
    }
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

void UpdateCoins(const CTransaction& tx, CValidationState& state, CCoinsViewCache& inputs, CTxUndo& txundo, int nHeight)
{
    // mark inputs spent
    if (!tx.IsCoinBase() && !tx.IsZerocoinSpend()) {
        txundo.vprevout.reserve(tx.vin.size());
        BOOST_FOREACH (const CTxIn& txin, tx.vin) {
            txundo.vprevout.push_back(CTxInUndo());
            bool ret = inputs.ModifyCoins(txin.prevout.hash)->Spend(txin.prevout, txundo.vprevout.back());
            assert(ret);
        }
    }

    // add outputs
    inputs.ModifyCoins(tx.GetHash())->FromTx(tx, nHeight);
}

bool CScriptCheck::operator()()
{
    const CScript& scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!VerifyScript(scriptSig, scriptPubKey, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, cacheStore), &error)) {
        return ::error("CScriptCheck(): %s:%d VerifySignature failed: %s", ptxTo->GetHash().ToString(), nIn, ScriptErrorString(error));
    }
    return true;
}

CBitcoinAddress addressExp1("DQZzqnSR6PXxagep1byLiRg9ZurCZ5KieQ");
CBitcoinAddress addressExp2("DTQYdnNqKuEHXyNeeYhPQGGGdqHbXYwjpj");

map<COutPoint, COutPoint> mapInvalidOutPoints;
map<CBigNum, CAmount> mapInvalidSerials;
void AddInvalidSpendsToMap(const CBlock& block)
{
    for (const CTransaction tx : block.vtx) {
        if (!tx.ContainsZerocoins())
            continue;

        //Check all zerocoinspends for bad serials
        for (const CTxIn in : tx.vin) {
            if (in.scriptSig.IsZerocoinSpend()) {
                CoinSpend spend = TxInToZerocoinSpend(in);

                //If serial is not valid, mark all outputs as bad
                if (!spend.HasValidSerial(Params().Zerocoin_Params())) {
                    mapInvalidSerials[spend.getCoinSerialNumber()] = spend.getDenomination() * COIN;

                    // Derive the actual valid serial from the invalid serial if possible
                    CBigNum bnActualSerial = spend.CalculateValidSerial(Params().Zerocoin_Params());
                    uint256 txHash;

                    if (zerocoinDB->ReadCoinSpend(bnActualSerial, txHash)) {
                        mapInvalidSerials[bnActualSerial] = spend.getDenomination() * COIN;

                        CTransaction txPrev;
                        uint256 hashBlock;
                        if (!GetTransaction(txHash, txPrev, hashBlock, true))
                            continue;

                        //Record all txouts from txPrev as invalid
                        for (unsigned int i = 0; i < txPrev.vout.size(); i++) {
                            //map to an empty outpoint to represent that this is the first in the chain of bad outs
                            mapInvalidOutPoints[COutPoint(txPrev.GetHash(), i)] = COutPoint();
                        }
                    }

                    //Record all txouts from this invalid zerocoin spend tx as invalid
                    for (unsigned int i = 0; i < tx.vout.size(); i++) {
                        //map to an empty outpoint to represent that this is the first in the chain of bad outs
                        mapInvalidOutPoints[COutPoint(tx.GetHash(), i)] = COutPoint();
                    }
                }
            }
        }
    }
}

// Populate global map (mapInvalidOutPoints) of invalid/fraudulent OutPoints that are banned from being used on the chain.
CAmount nFilteredThroughBittrex = 0;
bool fListPopulatedAfterLock = false;
void PopulateInvalidOutPointMap()
{
    if (fListPopulatedAfterLock)
        return;
    nFilteredThroughBittrex = 0;

    //Calculate over the entire period between the first bad tx and the tip of the chain - or the point at which this becomes enforced
    int nHeightLast = min(Params().Zerocoin_Block_RecalculateAccumulators() + 1, chainActive.Height());

    map<COutPoint, int> mapValidMixed;
    for (int i = Params().Zerocoin_Block_FirstFraudulent(); i < nHeightLast; i++) {
        CBlockIndex* pindex = chainActive[i];
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex))
            continue;

        //Find all the invalid spends for this block and record them
        AddInvalidSpendsToMap(block);

        //Any tx's that use a bad TxOut as an input is marked as invalid
        for (CTransaction tx : block.vtx) {
            for (CTxIn txIn : tx.vin) {
                if (mapInvalidOutPoints.count(txIn.prevout)) {

                    //If this is a stake transaction, masternode payments should not be considered fraudulent
                    std::list<COutPoint> listOutPoints;
                    if (tx.IsCoinStake()) {
                        CTxDestination dest;
                        if (!ExtractDestination(tx.vout[1].scriptPubKey, dest))
                            continue;

                        CBitcoinAddress addressKernel(dest);
                        for (unsigned int j = 1 ; j < tx.vout.size(); j++) { //1 because first is blank for coinstake

                            //If a payment goes to a different address, then count it as a masternode payment
                            CTxDestination destOut;
                            if (!ExtractDestination(tx.vout[j].scriptPubKey, destOut)) {
                                listOutPoints.emplace_back(COutPoint(tx.GetHash(), j));
                                continue;
                            }
                            CBitcoinAddress addressOut(destOut);
                            if (addressOut == addressKernel) {

                                //Anything past these two addresses is only guilty by association/washed funds
                                if (addressOut == addressExp1 || addressOut == addressExp2) {
                                    nFilteredThroughBittrex += tx.vout[j].nValue;
                                    continue;
                                }

                                //Mark this outpoint as invalid
                                listOutPoints.emplace_back(COutPoint(tx.GetHash(), j));
                            }
                        }
                    } else {
                        // Mark all outpoints invalid because they descend from exploited spends
                        for (COutPoint p : tx.GetOutPoints()) {
                            if (tx.vout[p.n].scriptPubKey.IsZerocoinMint()) {
                                listOutPoints.emplace_back(p);
                            } else {
                                //Anything past these two addresses is only guilty by association/washed funds
                                CTxDestination dest;
                                if (!ExtractDestination(tx.vout[p.n].scriptPubKey, dest)) {
                                    listOutPoints.emplace_back(p);
                                    continue;
                                }

                                CBitcoinAddress address(dest);
                                if (address == addressExp1 || address == addressExp2) {
                                    nFilteredThroughBittrex += tx.vout[p.n].nValue;
                                    continue;
                                }
                                //record this outpoint as invalid
                                listOutPoints.emplace_back(p);
                            }
                        }
                    }

                    //Record each fraudulent outpoint and its cause.
                    for (COutPoint o : listOutPoints)
                        mapInvalidOutPoints[o] = txIn.prevout;

                    //The entire tx set of outpoints are added, break here
                    break;
                }
            }
        }

        if (pindex->nHeight > Params().Zerocoin_Block_RecalculateAccumulators())
            fListPopulatedAfterLock = true;
    }
}

bool ValidOutPoint(const COutPoint out, int nHeight)
{
    bool isInvalid = nHeight >= Params().Block_Enforce_Invalid() && mapInvalidOutPoints.count(out);
    return !isInvalid;
}

CAmount GetInvalidUTXOValue()
{
    CAmount nValue = 0;
    for (auto it : mapInvalidOutPoints) {
        const COutPoint out = it.first;
        bool fSpent = false;
        CCoinsViewCache cache(pcoinsTip);
        const CCoins *coins = cache.AccessCoins(out.hash);
        if(!coins || !coins->IsAvailable(out.n))
            fSpent = true;

        if (!fSpent)
            nValue += coins->vout[out.n].nValue;
    }

    return nValue;
}

bool CheckInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, bool fScriptChecks, unsigned int flags, bool cacheStore, std::vector<CScriptCheck>* pvChecks)
{
    if (!tx.IsCoinBase() && !tx.IsZerocoinSpend()) {
        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
        if (!inputs.HaveInputs(tx))
            return state.Invalid(error("CheckInputs() : %s inputs unavailable", tx.GetHash().ToString()));

        // While checking, GetBestBlock() refers to the parent block.
        // This is also true for mempool checks.
        CBlockIndex* pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
        int nSpendHeight = pindexPrev->nHeight + 1;
        CAmount nValueIn = 0;
        CAmount nFees = 0;
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            const COutPoint& prevout = tx.vin[i].prevout;
            const CCoins* coins = inputs.AccessCoins(prevout.hash);
            assert(coins);

            // If prev is coinbase, check that it's matured
            if (coins->IsCoinBase() || coins->IsCoinStake()) {
                if (nSpendHeight - coins->nHeight < Params().COINBASE_MATURITY())
                    return state.Invalid(
                        error("CheckInputs() : tried to spend coinbase at depth %d, coinstake=%d", nSpendHeight - coins->nHeight, coins->IsCoinStake()),
                        REJECT_INVALID, "bad-txns-premature-spend-of-coinbase");
            }

            // Check for negative or overflow input values
            nValueIn += coins->vout[prevout.n].nValue;
            if (!MoneyRange(coins->vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return state.DoS(100, error("CheckInputs() : txin values out of range"),
                    REJECT_INVALID, "bad-txns-inputvalues-outofrange");
        }

        if (!tx.IsCoinStake()) {
            if (nValueIn < tx.GetValueOut())
                return state.DoS(100, error("CheckInputs() : %s value in (%s) < value out (%s)",
                                          tx.GetHash().ToString(), FormatMoney(nValueIn), FormatMoney(tx.GetValueOut())),
                    REJECT_INVALID, "bad-txns-in-belowout");

            // Tally transaction fees
            CAmount nTxFee = nValueIn - tx.GetValueOut();
            if (nTxFee < 0)
                return state.DoS(100, error("CheckInputs() : %s nTxFee < 0", tx.GetHash().ToString()),
                    REJECT_INVALID, "bad-txns-fee-negative");
            nFees += nTxFee;
            if (!MoneyRange(nFees))
                return state.DoS(100, error("CheckInputs() : nFees out of range"),
                    REJECT_INVALID, "bad-txns-fee-outofrange");
        }
        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks
        // before the last block chain checkpoint. This is safe because block merkle hashes are
        // still computed and checked, and any change will be caught at the next checkpoint.
        if (fScriptChecks) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                const COutPoint& prevout = tx.vin[i].prevout;
                const CCoins* coins = inputs.AccessCoins(prevout.hash);
                assert(coins);

                // Verify signature
                CScriptCheck check(*coins, tx, i, flags, cacheStore);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-standard DER encodings or non-null dummy
                        // arguments; if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        CScriptCheck check(*coins, tx, i,
                            flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheStore);
                        if (check())
                            return state.Invalid(false, REJECT_NONSTANDARD, strprintf("non-mandatory-script-verify-flag (%s)", ScriptErrorString(check.GetScriptError())));
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after a soft-fork
                    // super-majority vote has passed.
                    return state.DoS(100, false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)", ScriptErrorString(check.GetScriptError())));
                }
            }
        }
    }

    return true;
}

bool DisconnectBlock(CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool* pfClean)
{
    if (pindex->GetBlockHash() != view.GetBestBlock())
        LogPrintf("%s : pindex=%s view=%s\n", __func__, pindex->GetBlockHash().GetHex(), view.GetBestBlock().GetHex());
    assert(pindex->GetBlockHash() == view.GetBestBlock());

    if (pfClean)
        *pfClean = false;

    bool fClean = true;

    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull())
        return error("DisconnectBlock() : no undo data available");
    if (!blockUndo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
        return error("DisconnectBlock() : failure reading undo data");

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size())
        return error("DisconnectBlock() : block and undo data inconsistent");

    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction& tx = block.vtx[i];

        /** UNDO ZEROCOIN DATABASING
         * note we only undo zerocoin databasing in the following statement, value to and from PIVX
         * addresses should still be handled by the typical bitcoin based undo code
         * */
        if (tx.ContainsZerocoins()) {
            if (tx.IsZerocoinSpend()) {
                //erase all zerocoinspends in this transaction
                for (const CTxIn txin : tx.vin) {
                    if (txin.scriptSig.IsZerocoinSpend()) {
                        CoinSpend spend = TxInToZerocoinSpend(txin);
                        if (!zerocoinDB->EraseCoinSpend(spend.getCoinSerialNumber()))
                            return error("failed to erase spent zerocoin in block");
                    }
                }
            }
            if (tx.IsZerocoinMint()) {
                //erase all zerocoinmints in this transaction
                for (const CTxOut txout : tx.vout) {
                    if (txout.scriptPubKey.empty() || !txout.scriptPubKey.IsZerocoinMint())
                        continue;

                    PublicCoin pubCoin(Params().Zerocoin_Params());
                    if (!TxOutToPublicCoin(txout, pubCoin, state))
                        return error("DisconnectBlock(): TxOutToPublicCoin() failed");

                    if(!zerocoinDB->EraseCoinMint(pubCoin.getValue()))
                        return error("DisconnectBlock(): Failed to erase coin mint");
                }
            }
        }

        uint256 hash = tx.GetHash();

        // Check that all outputs are available and match the outputs in the block itself
        // exactly. Note that transactions with only provably unspendable outputs won't
        // have outputs available even in the block itself, so we handle that case
        // specially with outsEmpty.
        {
            CCoins outsEmpty;
            CCoinsModifier outs = view.ModifyCoins(hash);
            outs->ClearUnspendable();

            CCoins outsBlock(tx, pindex->nHeight);
            // The CCoins serialization does not serialize negative numbers.
            // No network rules currently depend on the version here, so an inconsistency is harmless
            // but it must be corrected before txout nversion ever influences a network rule.
            if (outsBlock.nVersion < 0)
                outs->nVersion = outsBlock.nVersion;
            if (*outs != outsBlock)
                fClean = fClean && error("DisconnectBlock() : added transaction mismatch? database corrupted");

            // remove outputs
            outs->Clear();
        }

        // restore inputs
        if (!tx.IsCoinBase() && !tx.IsZerocoinSpend()) { // not coinbases or zerocoinspend because they dont have traditional inputs
            const CTxUndo& txundo = blockUndo.vtxundo[i - 1];
            if (txundo.vprevout.size() != tx.vin.size())
                return error("DisconnectBlock() : transaction and undo data inconsistent - txundo.vprevout.siz=%d tx.vin.siz=%d", txundo.vprevout.size(), tx.vin.size());
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint& out = tx.vin[j].prevout;
                const CTxInUndo& undo = txundo.vprevout[j];
                CCoinsModifier coins = view.ModifyCoins(out.hash);
                if (undo.nHeight != 0) {
                    // undo data contains height: this is the last output of the prevout tx being spent
                    if (!coins->IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data overwriting existing transaction");
                    coins->Clear();
                    coins->fCoinBase = undo.fCoinBase;
                    coins->nHeight = undo.nHeight;
                    coins->nVersion = undo.nVersion;
                } else {
                    if (coins->IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data adding output to missing transaction");
                }
                if (coins->IsAvailable(out.n))
                    fClean = fClean && error("DisconnectBlock() : undo data overwriting existing output");
                if (coins->vout.size() < out.n + 1)
                    coins->vout.resize(out.n + 1);
                coins->vout[out.n] = undo.txout;
            }
        }
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash());

    if (!fVerifyingBlocks) {
        //if block is an accumulator checkpoint block, remove checkpoint and checksums from db
        uint256 nCheckpoint = pindex->nAccumulatorCheckpoint;
        if(nCheckpoint != pindex->pprev->nAccumulatorCheckpoint) {
            if(!EraseAccumulatorValues(nCheckpoint, pindex->pprev->nAccumulatorCheckpoint))
                return error("DisconnectBlock(): failed to erase checkpoint");
        }
    }

    if (pfClean) {
        *pfClean = fClean;
        return true;
    } else {
        return fClean;
    }
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE* fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool FindUndoPos(CValidationState& state, int nFile, CDiskBlockPos& pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck()
{
    // DRAGAN: changed, naming
    //RenameThread("pivx-scriptch");
    RenameThread("colx-scriptch");
    scriptcheckqueue.Thread();
}

void RecalculateZPIVMinted()
{
    CBlockIndex *pindex = chainActive[Params().Zerocoin_StartHeight()];
    int nHeightEnd = chainActive.Height();
    while (true) {
        if (pindex->nHeight % 1000 == 0)
            LogPrintf("%s : block %d...\n", __func__, pindex->nHeight);

        //overwrite possibly wrong vMintsInBlock data
        CBlock block;
        assert(ReadBlockFromDisk(block, pindex));

        std::list<CZerocoinMint> listMints;
        BlockToZerocoinMintList(block, listMints, true);

        vector<libzerocoin::CoinDenomination> vDenomsBefore = pindex->vMintDenominationsInBlock;
        pindex->vMintDenominationsInBlock.clear();
        for (auto mint : listMints)
            pindex->vMintDenominationsInBlock.emplace_back(mint.GetDenomination());

        if (pindex->nHeight < nHeightEnd)
            pindex = chainActive.Next(pindex);
        else
            break;
    }
}

void RecalculateZPIVSpent()
{
    CBlockIndex* pindex = chainActive[Params().Zerocoin_StartHeight()];
    while (true) {
        if (pindex->nHeight % 1000 == 0)
            LogPrintf("%s : block %d...\n", __func__, pindex->nHeight);

        //Rewrite zCOLX supply
        CBlock block;
        assert(ReadBlockFromDisk(block, pindex));

        list<libzerocoin::CoinDenomination> listDenomsSpent = ZerocoinSpendListFromBlock(block, true);

        //Reset the supply to previous block
        pindex->mapZerocoinSupply = pindex->pprev->mapZerocoinSupply;

        //Add mints to zCOLX supply
        for (auto denom : libzerocoin::zerocoinDenomList) {
            long nDenomAdded = count(pindex->vMintDenominationsInBlock.begin(), pindex->vMintDenominationsInBlock.end(), denom);
            pindex->mapZerocoinSupply.at(denom) += nDenomAdded;
        }

        //Remove spends from zCOLX supply
        for (auto denom : listDenomsSpent)
            pindex->mapZerocoinSupply.at(denom)--;

        //Rewrite money supply
        assert(pblocktree->WriteBlockIndex(CDiskBlockIndex(pindex)));

        if (pindex->nHeight < chainActive.Height())
            pindex = chainActive.Next(pindex);
        else
            break;
    }
}

bool RecalculatePIVSupply(int nHeightStart)
{
    if (nHeightStart > chainActive.Height())
        return false;

    CBlockIndex* pindex = chainActive[nHeightStart];
    CAmount nSupplyPrev = pindex->pprev->nMoneySupply;
    if (nHeightStart == Params().Zerocoin_StartHeight())
        nSupplyPrev = CAmount(5449796547496199);

    while (true) {
        if (pindex->nHeight % 1000 == 0)
            LogPrintf("%s : block %d...\n", __func__, pindex->nHeight);

        CBlock block;
        assert(ReadBlockFromDisk(block, pindex));

        CAmount nValueIn = 0;
        CAmount nValueOut = 0;
        for (const CTransaction tx : block.vtx) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (tx.IsCoinBase())
                    break;

                if (tx.vin[i].scriptSig.IsZerocoinSpend()) {
                    nValueIn += tx.vin[i].nSequence * COIN;
                    continue;
                }

                COutPoint prevout = tx.vin[i].prevout;
                CTransaction txPrev;
                uint256 hashBlock;
                assert(GetTransaction(prevout.hash, txPrev, hashBlock, true));
                nValueIn += txPrev.vout[prevout.n].nValue;
            }

            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                if (i == 0 && tx.IsCoinStake())
                    continue;

                nValueOut += tx.vout[i].nValue;
            }
        }

        // Rewrite money supply
        pindex->nMoneySupply = nSupplyPrev + nValueOut - nValueIn;
        nSupplyPrev = pindex->nMoneySupply;

        // Add fraudulent funds to the supply and remove any recovered funds.
        if (pindex->nHeight == Params().Zerocoin_Block_RecalculateAccumulators()) {
            PopulateInvalidOutPointMap();
            LogPrintf("%s : Original money supply=%s\n", __func__, FormatMoney(pindex->nMoneySupply));

            pindex->nMoneySupply += nFilteredThroughBittrex;
            LogPrintf("%s : Adding bittrex filtered funds to supply + %s : supply=%s\n", __func__, FormatMoney(nFilteredThroughBittrex), FormatMoney(pindex->nMoneySupply));

            CAmount nLocked = GetInvalidUTXOValue();
            pindex->nMoneySupply -= nLocked;
            LogPrintf("%s : Removing locked from supply - %s : supply=%s\n", __func__, FormatMoney(nLocked), FormatMoney(pindex->nMoneySupply));
        }

        assert(pblocktree->WriteBlockIndex(CDiskBlockIndex(pindex)));

        if (pindex->nHeight < chainActive.Height())
            pindex = chainActive.Next(pindex);
        else
            break;
    }
    return true;
}

bool ReindexAccumulators(list<uint256>& listMissingCheckpoints, string& strError)
{
    // PIVX: recalculate Accumulator Checkpoints that failed to database properly
    if (!listMissingCheckpoints.empty() && chainActive.Height() >= Params().Zerocoin_StartHeight()) {
        //uiInterface.InitMessage(_("Calculating missing accumulators..."));
        LogPrintf("%s : finding missing checkpoints\n", __func__);

        //search the chain to see when zerocoin started
        int nZerocoinStart = Params().Zerocoin_StartHeight();

        // find each checkpoint that is missing
        CBlockIndex* pindex = chainActive[nZerocoinStart];
        while (!listMissingCheckpoints.empty()) {
            if (ShutdownRequested())
                return false;

            // find checkpoints by iterating through the blockchain beginning with the first zerocoin block
            if (pindex->nAccumulatorCheckpoint != pindex->pprev->nAccumulatorCheckpoint) {

                //double dPercent = (pindex->nHeight - nZerocoinStart) / (double) (chainActive.Height() - nZerocoinStart);
                //uiInterface.ShowProgress(_("Calculating missing accumulators..."), (int) (dPercent * 100));
                if (find(listMissingCheckpoints.begin(), listMissingCheckpoints.end(), pindex->nAccumulatorCheckpoint) != listMissingCheckpoints.end()) {
                    uint256 nCheckpointCalculated = 0;
                    AccumulatorMap mapAccumulators;
                    if (!CalculateAccumulatorCheckpoint(pindex->nHeight, nCheckpointCalculated, mapAccumulators)) {
                        // GetCheckpoint could have terminated due to a shutdown request. Check this here.
                        if (ShutdownRequested())
                            break;
                        strError = _("Failed to calculate accumulator checkpoint");
                        return false;
                    }

                    //check that the calculated checkpoint is what is in the index.
                    if (nCheckpointCalculated != pindex->nAccumulatorCheckpoint) {
                        LogPrintf("%s : height=%d calculated_checkpoint=%s actual=%s\n", __func__, pindex->nHeight, nCheckpointCalculated.GetHex(), pindex->nAccumulatorCheckpoint.GetHex());
                        strError = _("Calculated accumulator checkpoint is not what is recorded by block index");
                        return false;
                    }

                    DatabaseChecksums(mapAccumulators);
                    auto it = find(listMissingCheckpoints.begin(), listMissingCheckpoints.end(), pindex->nAccumulatorCheckpoint);
                    listMissingCheckpoints.erase(it);
                }
            }

            // if we have iterated to the end of the blockchain, then checkpoints should be in sync
            if (pindex->nHeight + 1 <= chainActive.Height())
                pindex = chainActive.Next(pindex);
            else
                break;
        }
    }
    return true;
}

bool UpdateZPIVSupply(const CBlock& block, CBlockIndex* pindex)
{
    std::list<CZerocoinMint> listMints;
    bool fFilterInvalid = pindex->nHeight >= Params().Zerocoin_Block_RecalculateAccumulators();
    BlockToZerocoinMintList(block, listMints, fFilterInvalid);
    std::list<libzerocoin::CoinDenomination> listSpends = ZerocoinSpendListFromBlock(block, fFilterInvalid);

    // Initialize zerocoin supply to the supply from previous block
    if (pindex->pprev && pindex->pprev->GetBlockHeader().nVersion > CBlockHeader::VERSION4) {
        for (auto& denom : zerocoinDenomList) {
            pindex->mapZerocoinSupply.at(denom) = pindex->pprev->mapZerocoinSupply.at(denom);
        }
    }

    // Track zerocoin money supply
    CAmount nAmountZerocoinSpent = 0;
    pindex->vMintDenominationsInBlock.clear();
    if (pindex->pprev) {
        for (auto& m : listMints) {
            libzerocoin::CoinDenomination denom = m.GetDenomination();
            pindex->vMintDenominationsInBlock.push_back(m.GetDenomination());
            pindex->mapZerocoinSupply.at(denom)++;
        }

        for (auto& denom : listSpends) {
            pindex->mapZerocoinSupply.at(denom)--;
            nAmountZerocoinSpent += libzerocoin::ZerocoinDenominationToAmount(denom);

            // zerocoin failsafe
            if (pindex->mapZerocoinSupply.at(denom) < 0)
                return error("Block contains zerocoins that spend more than are in the available supply to spend");
        }
    }

    for (auto& denom : zerocoinDenomList)
        LogPrint("zero", "%s coins for denomination %d pubcoin %s\n", __func__, denom, pindex->mapZerocoinSupply.at(denom));

    return true;
}

static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool fJustCheck, bool fAlreadyChecked)
{
    AssertLockHeld(cs_main);
    // Check it again in case a previous version let a bad block in
    if (!fAlreadyChecked && !CheckBlock(block, state, !fJustCheck, !fJustCheck))
        return false;

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == NULL ? uint256(0) : pindex->pprev->GetBlockHash();
    if (hashPrevBlock != view.GetBestBlock())
        LogPrintf("%s: hashPrev=%s view=%s\n", __func__, hashPrevBlock.ToString().c_str(), view.GetBestBlock().ToString().c_str());
    assert(hashPrevBlock == view.GetBestBlock());

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (block.GetHash() == Params().HashGenesisBlock()) {
        view.SetBestBlock(pindex->GetBlockHash());
        return true;
    }

    if (pindex->nHeight > Params().LAST_POW_BLOCK() && block.IsProofOfWork())
        return state.DoS(100, error("ConnectBlock() : PoW period ended"),
            REJECT_INVALID, "PoW-ended");

    bool fScriptChecks = pindex->nHeight >= Checkpoints::GetTotalBlocksEstimate();

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
    // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely.
    // This rule was originally applied all blocks whose timestamp was after March 15, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
    // two in the chain that violate it. This prevents exploiting the issue against nodes in their
    // initial block download.
    for (const CTransaction& tx : block.vtx) {
        const CCoins* coins = view.AccessCoins(tx.GetHash());
        if (coins && !coins->IsPruned())
            return state.DoS(100, error("ConnectBlock() : tried to overwrite transaction"),
                REJECT_INVALID, "bad-txns-BIP30");
    }

    // BIP16 didn't become active until Apr 1 2012
    int64_t nBIP16SwitchTime = 1333238400;
    bool fStrictPayToScriptHash = (pindex->GetBlockTime() >= nBIP16SwitchTime);

    unsigned int flags = fStrictPayToScriptHash ? SCRIPT_VERIFY_P2SH : SCRIPT_VERIFY_NONE;
    flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY; // Start enforcing CHECKLOCKTIMEVERIFY (BIP65)

    // Start enforcing the DERSIG (BIP66) rules, for block.nVersion=3 blocks, when 75% of the network has upgraded:
    if (block.nVersion >= CBlockHeader::VERSION3 &&
        CBlockIndex::IsSuperMajority(CBlockHeader::VERSION3, pindex->pprev, Params().EnforceBlockUpgradeMajority())) {
            flags |= SCRIPT_VERIFY_DERSIG;
    }

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : NULL);

    int64_t nTimeStart = GetTimeMicros();
    CAmount nFees = 0;
    int nInputs = 0;
    unsigned int nSigOps = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    std::vector<pair<CoinSpend, uint256> > vSpends;
    vPos.reserve(block.vtx.size());
    CBlockUndo blockundo;
    blockundo.vtxundo.reserve(block.vtx.size() - 1);
    CAmount nValueOut = 0;
    CAmount nValueIn = 0;
    unsigned int nMaxBlockSigOps = MAX_BLOCK_SIGOPS_CURRENT;
    vector<uint256> vSpendsInBlock;
    for (unsigned int i = 0; i < block.vtx.size(); i++) {
        const CTransaction& tx = block.vtx[i];

        nInputs += tx.vin.size();
        nSigOps += GetLegacySigOpCount(tx);
        if (nSigOps > nMaxBlockSigOps)
            return state.DoS(100, error("ConnectBlock() : too many sigops"), REJECT_INVALID, "bad-blk-sigops");

        //Temporarily disable zerocoin transactions for maintenance
        if (block.nTime > GetSporkValue(SPORK_20_ZEROCOIN_MAINTENANCE_MODE) && !IsInitialBlockDownload() && tx.ContainsZerocoins()) {
            return state.DoS(100, error("ConnectBlock() : zerocoin transactions are currently in maintenance mode"));
        }

        if (tx.IsZerocoinSpend()) {
            int nHeightTx = 0;
            uint256 txid = tx.GetHash();
            vSpendsInBlock.emplace_back(txid);
            if (IsTransactionInChain(txid, nHeightTx)) {
                //when verifying blocks on init, the blocks are scanned without being disconnected - prevent that from causing an error
                if (!fVerifyingBlocks || (fVerifyingBlocks && pindex->nHeight > nHeightTx))
                    return state.DoS(100, error("%s : txid %s already exists in block %d , trying to include it again in block %d", __func__,
                                                tx.GetHash().GetHex(), nHeightTx, pindex->nHeight),
                                     REJECT_INVALID, "bad-txns-inputs-missingorspent");
            }

            //Check for double spending of serial #'s
            set<CBigNum> setSerials;
            for (const CTxIn& txIn : tx.vin) {
                if (!txIn.scriptSig.IsZerocoinSpend())
                    continue;
                CoinSpend spend = TxInToZerocoinSpend(txIn);
                nValueIn += spend.getDenomination() * COIN;

                //Perform checks on the spend that are based on blockchain context
                if (!ContextualCheckCoinSpend(spend, pindex, txid))
                    return state.DoS(100, error("%s: Coinspend is not valid in block %s", __func__, block.GetHash().GetHex()));

                //queue for db write after the 'justcheck' section has concluded
                vSpends.emplace_back(make_pair(spend, tx.GetHash()));
            }
        } else if (!tx.IsCoinBase()) {
            if (!view.HaveInputs(tx))
                return state.DoS(100, error("ConnectBlock() : inputs missing/spent"),
                    REJECT_INVALID, "bad-txns-inputs-missingorspent");

            // Check that the inputs are not marked as invalid/fraudulent
            for (CTxIn in : tx.vin) {
                if (!ValidOutPoint(in.prevout, pindex->nHeight)) {
                    return state.DoS(100, error("%s : tried to spend invalid input %s in tx %s", __func__, in.prevout.ToString(),
                                  tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                }
            }

            // Add in sigops done by pay-to-script-hash inputs;
            // this is to prevent a "rogue miner" from creating
            // an incredibly-expensive-to-validate block.
            nSigOps += GetP2SHSigOpCount(tx, view);
            if (nSigOps > nMaxBlockSigOps)
                return state.DoS(100, error("ConnectBlock() : too many sigops"), REJECT_INVALID, "bad-blk-sigops");

            if (!tx.IsCoinStake())
                nFees += view.GetValueIn(tx) - tx.GetValueOut();
            nValueIn += view.GetValueIn(tx);

            std::vector<CScriptCheck> vChecks;
            if (!CheckInputs(tx, state, view, fScriptChecks, flags, false, nScriptCheckThreads ? &vChecks : NULL))
                return false;
            control.Add(vChecks);
        }
        nValueOut += tx.GetValueOut();

        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.push_back(CTxUndo());
        }
        UpdateCoins(tx, state, view, i == 0 ? undoDummy : blockundo.vtxundo.back(), pindex->nHeight);

        vPos.push_back(std::make_pair(tx.GetHash(), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }

    //A one-time event where money supply counts were off and recalculated on a certain block.
    if (pindex->nHeight == Params().Zerocoin_Block_RecalculateAccumulators() + 1) {
        RecalculateZPIVMinted();
        RecalculateZPIVSpent();
        RecalculatePIVSupply(Params().Zerocoin_StartHeight());
    }

    //Track zCOLX money supply in the block index
    if (!UpdateZPIVSupply(block, pindex))
        return state.DoS(100, error("%s: Failed to calculate new zCOLX supply for block=%s height=%d", __func__,
                                    block.GetHash().GetHex(), pindex->nHeight), REJECT_INVALID);

    // track money supply and mint amount info
    CAmount nMoneySupplyPrev = pindex->pprev ? pindex->pprev->nMoneySupply : 0;
    pindex->nMoneySupply = nMoneySupplyPrev + nValueOut - nValueIn;
    pindex->nMint = pindex->nMoneySupply - nMoneySupplyPrev + nFees;

    int64_t nTime1 = GetTimeMicros();
    nTimeConnect += nTime1 - nTimeStart;
    LogPrint("bench", "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs]\n", (unsigned)block.vtx.size(), 0.001 * (nTime1 - nTimeStart), 0.001 * (nTime1 - nTimeStart) / block.vtx.size(), nInputs <= 1 ? 0 : 0.001 * (nTime1 - nTimeStart) / (nInputs - 1), nTimeConnect * 0.000001);

    // Check required payments are made (masternode payments / budgets / etc)
    // It is entierly possible that we don't have enough data and this could fail
    // (i.e. the block could indeed be valid). Store the block for later consideration
    // but issue an initial reject message.
    // The case also exists that the sending peer could not have enough data to see
    // that this block is invalid, so don't issue an outright ban.
    if (!IsInitialBlockDownload()) {
        if (!IsBlockPayeeValid(block, pindex->nHeight, nFees, pindex->pprev)) {
            mapRejectedBlocks.insert(make_pair(block.GetHash(), GetTime()));
            return state.DoS(0, error("CheckBlock() : Couldn't find masternode/budget payment"), REJECT_INVALID, "bad-cb-payee");
        }
    } else {
        DebugPrintf("%s: Masternode payment check skipped on sync - skipping IsBlockPayeeValid()\n", __func__);
    }

    // Check that the block does not overmint
    CAmount nExpectedMint = nFees + GetBlockExpectedMint(pindex->nHeight);
    if (!IsBlockValueValid(block, pindex->nHeight, nExpectedMint, pindex->nMint, pindex->pprev))
        return state.DoS(100, error("ConnectBlock() : reward pays too much (actual=%s vs limit=%s)",
            FormatMoney(pindex->nMint), FormatMoney(nExpectedMint)), REJECT_INVALID, "bad-cb-amount");
        //LogPrintf("%s : reward pays too much (actual=%s vs limit=%s)\n", __func__, FormatMoney(pindex->nMint), FormatMoney(nExpectedMint));

    // Ensure that accumulator checkpoints are valid and in the same state as this instance of the chain
    AccumulatorMap mapAccumulators;
    if (!ValidateAccumulatorCheckpoint(block, pindex, mapAccumulators))
        return state.DoS(100, error("%s: Failed to validate accumulator checkpoint for block=%s height=%d", __func__,
            block.GetHash().GetHex(), pindex->nHeight), REJECT_INVALID, "bad-acc-checkpoint");

    if (!control.Wait())
        return state.DoS(100, false);

    int64_t nTime2 = GetTimeMicros();
    nTimeVerify += nTime2 - nTimeStart;
    LogPrint("bench", "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n", nInputs - 1, 0.001 * (nTime2 - nTimeStart), nInputs <= 1 ? 0 : 0.001 * (nTime2 - nTimeStart) / (nInputs - 1), nTimeVerify * 0.000001);

    //IMPORTANT NOTE: Nothing before this point should actually store to disk (or even memory)
    if (fJustCheck)
        return true;

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || !pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos pos;
            if (!FindUndoPos(state, pindex->nFile, pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
                return error("ConnectBlock() : FindUndoPos failed");
            if (!blockundo.WriteToDisk(pos, pindex->pprev->GetBlockHash()))
                return state.Abort("Failed to write undo data");

            // update nUndoPos in block index
            pindex->nUndoPos = pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    //Record zCOLX serials
    for (pair<CoinSpend, uint256> pSpend : vSpends) {
        //record spend to database
        if (!zerocoinDB->WriteCoinSpend(pSpend.first.getCoinSerialNumber(), pSpend.second))
            return state.Abort(("Failed to record coin serial to database"));
    }

    //Record accumulator checksums
    DatabaseChecksums(mapAccumulators);

    if (fTxIndex)
        if (!pblocktree->WriteTxIndex(vPos))
            return state.Abort("Failed to write transaction index");



    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    int64_t nTime3 = GetTimeMicros();
    nTimeIndex += nTime3 - nTime2;
    LogPrint("bench", "    - Index writing: %.2fms [%.2fs]\n", 0.001 * (nTime3 - nTime2), nTimeIndex * 0.000001);

    // Watch for changes to the previous coinbase transaction.
    static uint256 hashPrevBestCoinBase;
    GetMainSignals().UpdatedTransaction(hashPrevBestCoinBase);
    hashPrevBestCoinBase = block.vtx[0].GetHash();

    int64_t nTime4 = GetTimeMicros();
    nTimeCallbacks += nTime4 - nTime3;
    LogPrint("bench", "    - Callbacks: %.2fms [%.2fs]\n", 0.001 * (nTime4 - nTime3), nTimeCallbacks * 0.000001);

    //Continue tracking possible movement of fraudulent funds until they are completely frozen
    if (pindex->nHeight >= Params().Zerocoin_Block_FirstFraudulent() && pindex->nHeight <= Params().Zerocoin_Block_RecalculateAccumulators() + 1)
        AddInvalidSpendsToMap(block);

    //Remove zerocoinspends from the pending map
    for (const uint256& txid : vSpendsInBlock) {
        auto it = mapZerocoinspends.find(txid);
        if (it != mapZerocoinspends.end())
            mapZerocoinspends.erase(it);
    }

    return true;
}

enum FlushStateMode {
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed if either they're too large, forceWrite is set, or
 * fast is not set and it's been a while since the last write.
 */
bool static FlushStateToDisk(CValidationState& state, FlushStateMode mode)
{
    LOCK(cs_main);
    static int64_t nLastWrite = 0;
    try {
        if ((mode == FLUSH_STATE_ALWAYS) ||
            ((mode == FLUSH_STATE_PERIODIC || mode == FLUSH_STATE_IF_NEEDED) && pcoinsTip->GetCacheSize() > nCoinCacheSize) ||
            (mode == FLUSH_STATE_PERIODIC && GetTimeMicros() > nLastWrite + DATABASE_WRITE_INTERVAL * 1000000)) {
            // Typical CCoins structures on disk are around 100 bytes in size.
            // Pushing a new one to the database can cause it to be written
            // twice (once in the log, and once in the tables). This is already
            // an overestimation, as most will delete an existing entry or
            // overwrite one. Still, use a conservative safety factor of 2.
            if (!CheckDiskSpace(100 * 2 * 2 * pcoinsTip->GetCacheSize()))
                return state.Error("out of disk space");
            // First make sure all block and undo data is flushed to disk.
            FlushBlockFile();
            // Then update all block file information (which may refer to block and undo files).
            bool fileschanged = false;
            for (set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end();) {
                if (!pblocktree->WriteBlockFileInfo(*it, vinfoBlockFile[*it])) {
                    return state.Abort("Failed to write to block index");
                }
                fileschanged = true;
                setDirtyFileInfo.erase(it++);
            }
            if (fileschanged && !pblocktree->WriteLastBlockFile(nLastBlockFile)) {
                return state.Abort("Failed to write to block index");
            }
            for (set<CBlockIndex*>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end();) {
                if (!pblocktree->WriteBlockIndex(CDiskBlockIndex(*it))) {
                    return state.Abort("Failed to write to block index");
                }
                setDirtyBlockIndex.erase(it++);
            }
            pblocktree->Sync();
            // Finally flush the chainstate (which may refer to block index entries).
            if (!pcoinsTip->Flush())
                return state.Abort("Failed to write to coin database");
            // Update best block in wallet (so we can detect restored wallets).
            if (mode != FLUSH_STATE_IF_NEEDED) {
                GetMainSignals().SetBestChain(chainActive.GetLocator());
            }
            nLastWrite = GetTimeMicros();
        }
    } catch (const std::runtime_error& e) {
        return state.Abort(std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk()
{
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
}

/** Update chainActive and related internal data structures. */
void static UpdateTip(CBlockIndex* pindexNew)
{
    chainActive.SetTip(pindexNew);

    // If turned on AutoZeromint will automatically convert COLX to zCOLX
    bool fZerocoinActive = chainActive.Height() >= Params().Zerocoin_StartHeight();
    if (pwalletMain->isZeromintEnabled () && fZerocoinActive)
        pwalletMain->AutoZeromint ();

    // New best block
    nTimeBestReceived = GetTime();
    mempool.AddTransactionsUpdated(1);

    LogPrintf("UpdateTip: new best=%s  height=%d  log2_work=%.8g  tx=%lu  date=%s progress=%f  cache=%u\n",
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(), log(chainActive.Tip()->nChainWork.getdouble()) / log(2.0), (unsigned long)chainActive.Tip()->nChainTx,
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
        Checkpoints::GuessVerificationProgress(chainActive.Tip()), (unsigned int)pcoinsTip->GetCacheSize());

    cvBlockChange.notify_all();

    // Check the version of the last 100 blocks to see if we need to upgrade:
    static bool fWarned = false;
    if (!IsInitialBlockDownload() && !fWarned) {
        int nUpgraded = 0;
        const CBlockIndex* pindex = chainActive.Tip();
        for (int i = 0; i < 100 && pindex != NULL; i++) {
            if (pindex->nVersion > CBlockHeader::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            LogPrintf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, (int)CBlockHeader::CURRENT_VERSION);
        if (nUpgraded > 100 / 2) {
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
            CAlert::Notify(strMiscWarning, true);
            fWarned = true;

            // Force user update wallet if new version is available
            if (GetContext().GetAutoUpdateModel()->IsUpdateAvailable()) {
                string msg = strprintf("%s New version is available, please update your wallet! Go to: %s", strMiscWarning, GetContext().GetAutoUpdateModel()->GetUpdateUrlTag());
                AbortNode(msg, msg);
            }
        }
    }
}

/** Disconnect chainActive's tip. */
bool static DisconnectTip(CValidationState& state)
{
    CBlockIndex* pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    mempool.check(pcoinsTip);
    // Read block from disk.
    CBlock block;
    if (!ReadBlockFromDisk(block, pindexDelete))
        return state.Abort("Failed to read block");
    // Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();
    {
        CCoinsViewCache view(pcoinsTip);
        if (!DisconnectBlock(block, state, pindexDelete, view))
            return error("DisconnectTip() : DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
        assert(view.Flush());
    }
    LogPrint("bench", "- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_ALWAYS))
        return false;
    // Resurrect mempool transactions from the disconnected block.
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        // ignore validation errors in resurrected transactions
        list<CTransaction> removed;
        CValidationState stateDummy;
        if (tx.IsCoinBase() || tx.IsCoinStake() || !AcceptToMemoryPool(mempool, stateDummy, tx, false, NULL))
            mempool.remove(tx, removed, true);
    }
    mempool.removeCoinbaseSpends(pcoinsTip, pindexDelete->nHeight);
    mempool.check(pcoinsTip);
    // Update chainActive and related variables.
    UpdateTip(pindexDelete->pprev);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        SyncWithWallets(tx, NULL);
    }
    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

/**
 * Connect a new block to chainActive. pblock is either NULL or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 */
bool static ConnectTip(CValidationState& state, CBlockIndex* pindexNew, CBlock* pblock, bool fAlreadyChecked)
{
    assert(pindexNew->pprev == chainActive.Tip());
    mempool.check(pcoinsTip);
    CCoinsViewCache view(pcoinsTip);

    if (pblock == NULL)
        fAlreadyChecked = false;

    // Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    CBlock block;
    if (!pblock) {
        if (!ReadBlockFromDisk(block, pindexNew))
            return state.Abort("Failed to read block");
        pblock = &block;
    }
    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros();
    nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;
    LogPrint("bench", "  - Load block from disk: %.2fms [%.2fs]\n", (nTime2 - nTime1) * 0.001, nTimeReadFromDisk * 0.000001);
    {
        CInv inv(MSG_BLOCK, pindexNew->GetBlockHash());
        bool rv = ConnectBlock(*pblock, state, pindexNew, view, false, fAlreadyChecked);
        GetMainSignals().BlockChecked(*pblock, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state);
            return error("ConnectTip() : ConnectBlock %s failed", pindexNew->GetBlockHash().ToString());
        }
        mapBlockSource.erase(inv.hash);
        nTime3 = GetTimeMicros();
        nTimeConnectTotal += nTime3 - nTime2;
        LogPrint("bench", "  - Connect total: %.2fms [%.2fs]\n", (nTime3 - nTime2) * 0.001, nTimeConnectTotal * 0.000001);
        assert(view.Flush());
    }
    int64_t nTime4 = GetTimeMicros();
    nTimeFlush += nTime4 - nTime3;
    LogPrint("bench", "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001, nTimeFlush * 0.000001);

    // Write the chain state to disk, if necessary. Always write to disk if this is the first of a new file.
    FlushStateMode flushMode = FLUSH_STATE_IF_NEEDED;
    if (pindexNew->pprev && (pindexNew->GetBlockPos().nFile != pindexNew->pprev->GetBlockPos().nFile))
        flushMode = FLUSH_STATE_ALWAYS;
    if (!FlushStateToDisk(state, flushMode))
        return false;
    int64_t nTime5 = GetTimeMicros();
    nTimeChainState += nTime5 - nTime4;
    LogPrint("bench", "  - Writing chainstate: %.2fms [%.2fs]\n", (nTime5 - nTime4) * 0.001, nTimeChainState * 0.000001);

    // Remove conflicting transactions from the mempool.
    list<CTransaction> txConflicted;
    mempool.removeForBlock(pblock->vtx, pindexNew->nHeight, txConflicted);
    mempool.check(pcoinsTip);
    // Update chainActive & related variables.
    UpdateTip(pindexNew);
    // Tell wallet about transactions that went from mempool
    // to conflicted:
    BOOST_FOREACH (const CTransaction& tx, txConflicted) {
        SyncWithWallets(tx, NULL);
    }
    // ... and about transactions that got confirmed:
    BOOST_FOREACH (const CTransaction& tx, pblock->vtx) {
        SyncWithWallets(tx, pblock);
    }

    int64_t nTime6 = GetTimeMicros();
    nTimePostConnect += nTime6 - nTime5;
    nTimeTotal += nTime6 - nTime1;
    LogPrint("bench", "  - Connect postprocess: %.2fms [%.2fs]\n", (nTime6 - nTime5) * 0.001, nTimePostConnect * 0.000001);
    LogPrint("bench", "- Connect block: %.2fms [%.2fs]\n", (nTime6 - nTime1) * 0.001, nTimeTotal * 0.000001);
    return true;
}

bool DisconnectBlocksAndReprocess(int blocks)
{
    LOCK(cs_main);

    CValidationState state;

    LogPrintf("DisconnectBlocksAndReprocess: Got command to replay %d blocks\n", blocks);
    for (int i = 0; i <= blocks; i++)
        DisconnectTip(state);

    return true;
}

/*
    DisconnectBlockAndInputs

    Remove conflicting blocks for successful SwiftX transaction locks
    This should be very rare (Probably will never happen)
*/
// ***TODO*** clean up here
bool DisconnectBlockAndInputs(CValidationState& state, CTransaction txLock)
{
    // All modifications to the coin state will be done in this cache.
    // Only when all have succeeded, we push it to pcoinsTip.
    //    CCoinsViewCache view(*pcoinsTip, true);

    CBlockIndex* BlockReading = chainActive.Tip();
    CBlockIndex* pindexNew = NULL;

    bool foundConflictingTx = false;

    //remove anything conflicting in the memory pool
    list<CTransaction> txConflicted;
    mempool.removeConflicts(txLock, txConflicted);


    // List of what to disconnect (typically nothing)
    vector<CBlockIndex*> vDisconnect;

    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0 && !foundConflictingTx && i < 6; i++) {
        vDisconnect.push_back(BlockReading);
        pindexNew = BlockReading->pprev; //new best block

        CBlock block;
        if (!ReadBlockFromDisk(block, BlockReading))
            return state.Abort(_("Failed to read block"));

        // Queue memory transactions to resurrect.
        // We only do this for blocks after the last checkpoint (reorganisation before that
        // point should only happen with -reindex/-loadblock, or a misbehaving peer.
        BOOST_FOREACH (const CTransaction& tx, block.vtx) {
            if (!tx.IsCoinBase()) {
                BOOST_FOREACH (const CTxIn& in1, txLock.vin) {
                    BOOST_FOREACH (const CTxIn& in2, tx.vin) {
                        if (in1.prevout == in2.prevout) foundConflictingTx = true;
                    }
                }
            }
        }

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    if (!foundConflictingTx) {
        LogPrintf("DisconnectBlockAndInputs: Can't find a conflicting transaction to inputs\n");
        return false;
    }

    if (vDisconnect.size() > 0) {
        LogPrintf("REORGANIZE: Disconnect Conflicting Blocks %lli blocks; %s..\n", vDisconnect.size(), pindexNew->GetBlockHash().ToString());
        BOOST_FOREACH (CBlockIndex* pindex, vDisconnect) {
            LogPrintf(" -- disconnect %s\n", pindex->GetBlockHash().ToString());
            DisconnectTip(state);
        }
    }

    return true;
}


/**
 * Return the tip of the chain with the most work in it, that isn't
 * known to be invalid (it's however far from certain to be valid).
 */
static CBlockIndex* FindMostWorkChain()
{
    do {
        CBlockIndex* pindexNew = NULL;

        // Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return NULL;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        CBlockIndex* pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted.  Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_MASK;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);
            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing data)
                if (fFailedChain && (pindexBestInvalid == NULL || pindexNew->nChainWork > pindexBestInvalid->nChainWork))
                    pindexBestInvalid = pindexNew;
                CBlockIndex* pindexFailed = pindexNew;
                // Remove the entire chain from the set.
                while (pindexTest != pindexFailed) {
                    if (fFailedChain) {
                        pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    } else if (fMissingData) {
                        // If we're missing data, then add back to mapBlocksUnlinked,
                        // so that if the block arrives in the future we can try adding
                        // to setBlockIndexCandidates again.
                        mapBlocksUnlinked.insert(std::make_pair(pindexFailed->pprev, pindexFailed));
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }
            pindexTest = pindexTest->pprev;
        }
        if (!fInvalidAncestor)
            return pindexNew;
    } while (true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
static void PruneBlockIndexCandidates()
{
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either NULL or a pointer to a CBlock corresponding to pindexMostWork.
 */
static bool ActivateBestChainStep(CValidationState& state, CBlockIndex* pindexMostWork, CBlock* pblock, bool fAlreadyChecked)
{
    AssertLockHeld(cs_main);
    if (pblock == NULL)
        fAlreadyChecked = false;
    bool fInvalidFound = false;
    const CBlockIndex* pindexOldTip = chainActive.Tip();
    const CBlockIndex* pindexFork = chainActive.FindFork(pindexMostWork);

    // Disconnect active blocks which are no longer in the best chain.
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(state))
            return false;
    }

    // Build list of new blocks to connect.
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
        // a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex* pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;

        // Connect new blocks.
        BOOST_REVERSE_FOREACH (CBlockIndex* pindexConnect, vpindexToConnect) {
            if (!ConnectTip(state, pindexConnect, pindexConnect == pindexMostWork ? pblock : NULL, fAlreadyChecked)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible())
                        InvalidChainFound(vpindexToConnect.back());
                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip || chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    // Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back());
    else
        CheckForkWarningConditions();

    return true;
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either NULL or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 */
bool ActivateBestChain(CValidationState& state, CBlock* pblock, bool fAlreadyChecked)
{
    CBlockIndex* pindexMostWork = nullptr;
    do {
        boost::this_thread::interruption_point();
        CBlockIndex *pindexNewTip = nullptr;
        const CBlockIndex *pindexFork = nullptr;
        bool fInitialDownload = false;

        while (true) {
            // I2PDEADLOCK: this isn't really a TRY_LOCK as we're looping around
            TRY_LOCK(cs_main, lockMain);
            if (!lockMain) {
                MilliSleep(50);
                continue;
            }

            CBlockIndex *pindexOldTip = chainActive.Tip();
            pindexMostWork = FindMostWorkChain();

            // Whether we have anything to do at all.
            if (pindexMostWork == NULL || pindexMostWork == chainActive.Tip())
                return true;

            if (!ActivateBestChainStep(state, pindexMostWork, pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : NULL, fAlreadyChecked))
                return false;

            pindexNewTip = chainActive.Tip();
            pindexFork = chainActive.FindFork(pindexOldTip);
            fInitialDownload = IsInitialBlockDownload();
            break;
        }
        // When we reach this point, we switched to a new tip (stored in pindexNewTip).

        // Notifications/callbacks that can run without cs_main
        if (!fInitialDownload) {
            // Find the hashes of all blocks that weren't previously in the best chain.
            std::vector<uint256> vHashes;
            CBlockIndex *pindexToAnnounce = pindexNewTip;
            while (pindexToAnnounce != pindexFork) {
                vHashes.push_back(pindexToAnnounce->GetBlockHash());
                pindexToAnnounce = pindexToAnnounce->pprev;
                if (vHashes.size() == MAX_BLOCKS_TO_ANNOUNCE) {
                    // Limit announcements in case of a huge reorganization.
                    // Rely on the peer's synchronization mechanism in that case.
                    break;
                }
            }

            // Relay inventory, but don't relay old inventory during initial block download.
            int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CI2pdNode* pnode, vNodes) {
                    if (chainActive.Height() > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate)) {
                        BOOST_REVERSE_FOREACH(const uint256& hash, vHashes) {
                            pnode->PushBlockHash(hash);
                        }
                    }
                }
            }

            // Notify external listeners about the new tip.
            // Note: uiInterface, should switch main signals.
            if (!vHashes.empty()) {
                GetMainSignals().UpdatedBlockTip(pindexNewTip);
                uiInterface.NotifyBlockTip(vHashes.front());

                if (pblock) {
                    unsigned size = GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION);
                    // If the size is over 1 MB notify external listeners, and it is within the last 5 minutes
                    // ZC: why MAX_BLOCK_SIZE_LEGACY, a bug (PIVX) or on purpose to notify of?
                    if (size > MAX_BLOCK_SIZE_LEGACY && pblock->GetBlockTime() > GetAdjustedTime() - 300) {
                        uiInterface.NotifyBlockSize(static_cast<int>(size), vHashes.front());
                    }
                }
            }
        }
    } while (pindexMostWork != chainActive.Tip());
    CheckBlockIndex();

    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    return true;
}

bool InvalidateBlock(CValidationState& state, CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);

    while (chainActive.Contains(pindex)) {
        CBlockIndex* pindexWalk = chainActive.Tip();
        pindexWalk->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(pindexWalk);
        setBlockIndexCandidates.erase(pindexWalk);
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state)) {
            return false;
        }
    }

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add them again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && !setBlockIndexCandidates.value_comp()(it->second, chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);
    return true;
}

bool ReconsiderBlock(CValidationState& state, CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && setBlockIndexCandidates.value_comp()(chainActive.Tip(), it->second)) {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to one of those.
                pindexBestInvalid = NULL;
            }
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != NULL) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

CBlockIndex* AddToBlockIndex(const CBlock& block)
{
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end())
        return it->second;

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;

    //mark as PoS seen
    if (pindexNew->IsProofOfStake())
        setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));

    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end()) {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();

        //update previous block pointer
        pindexNew->pprev->pnext = pindexNew;

        // ppcoin: compute chain trust score
        pindexNew->bnChainTrust = (pindexNew->pprev ? pindexNew->pprev->bnChainTrust : 0) + pindexNew->GetBlockTrust();

        // ppcoin: compute stake entropy bit for stake modifier
        if (!pindexNew->SetStakeEntropyBit(pindexNew->GetStakeEntropyBit()))
            LogPrintf("AddToBlockIndex() : SetStakeEntropyBit() failed \n");

        // ppcoin: record proof-of-stake hash value
        if (pindexNew->IsProofOfStake()) {
            if (!mapProofOfStake.count(hash))
                LogPrintf("AddToBlockIndex() : hashProofOfStake not found in map \n");
            pindexNew->hashProofOfStake = mapProofOfStake[hash];
        }

        // ppcoin: compute stake modifier
        uint64_t nStakeModifier = 0;
        bool fGeneratedStakeModifier = false;
        if (!ComputeNextStakeModifier(pindexNew->pprev, nStakeModifier, fGeneratedStakeModifier))
            LogPrintf("AddToBlockIndex() : ComputeNextStakeModifier() failed \n");
        pindexNew->SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
        pindexNew->nStakeModifierChecksum = GetStakeModifierChecksum(pindexNew);
        if (!CheckStakeModifierCheckpoints(pindexNew->nHeight, pindexNew->nStakeModifierChecksum))
            LogPrintf("AddToBlockIndex() : Rejected by stake modifier checkpoint height=%d, modifier=%s \n", pindexNew->nHeight, boost::lexical_cast<std::string>(nStakeModifier));
    }
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (pindexBestHeader == NULL || pindexBestHeader->nChainWork < pindexNew->nChainWork)
        pindexBestHeader = pindexNew;

    //update previous block pointer
    if (pindexNew->nHeight)
        pindexNew->pprev->pnext = pindexNew;

    setDirtyBlockIndex.insert(pindexNew);

    return pindexNew;
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
bool ReceivedBlockTransactions(const CBlock& block, CValidationState& state, CBlockIndex* pindexNew, const CDiskBlockPos& pos)
{
    if (block.IsProofOfStake())
        pindexNew->SetProofOfStake();
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    if (pindexNew->pprev == NULL || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex* pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (chainActive.Tip() == NULL || !setBlockIndexCandidates.value_comp()(pindex, chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator it = range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    } else {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            mapBlocksUnlinked.insert(std::make_pair(pindexNew->pprev, pindexNew));
        }
    }

    return true;
}

bool FindBlockPos(CValidationState& state, CDiskBlockPos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            LogPrintf("Leaving block file %i: %s\n", nFile, vinfoBlockFile[nFile].ToString());
            FlushBlockFile(true);
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    nLastBlockFile = nFile;
    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE* file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            } else
                return state.Error("out of disk space");
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

bool FindUndoPos(CValidationState& state, int nFile, CDiskBlockPos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    nNewSize = vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE* file = OpenUndoFile(pos);
            if (file) {
                LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        } else
            return state.Error("out of disk space");
    }

    return true;
}

bool CheckBlockHeader(const CBlockHeader& block, CValidationState& state, bool fCheckPOW)
{
    if (block.GetHash() == Params().HashGenesisBlock())
        return true;

    // Check proof of work matches claimed amount
    if (fCheckPOW && !CheckProofOfWork(block.GetHash(), block.nBits))
        return state.DoS(50, error("CheckBlockHeader() : proof of work failed"), REJECT_INVALID, "high-hash");

    return true;
}

bool CheckBlock(const CBlock& block, CValidationState& state, bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckSig)
{
    // These are checks that are independent of context.

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, block.IsProofOfWork()))
        return state.DoS(100, error("CheckBlock() : CheckBlockHeader failed"),
            REJECT_INVALID, "bad-header", true);

    // Check timestamp
    LogPrint("debug", "%s: block=%s  is proof of stake=%d\n", __func__, block.GetHash().ToString().c_str(), block.IsProofOfStake());
    if (block.GetBlockTime() > GetAdjustedTime() + (block.IsProofOfStake() ? 60 : 7200)) // 1 minute future drift for PoS
        return state.Invalid(error("CheckBlock() : block timestamp too far in the future"),
            REJECT_INVALID, "time-too-new");

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = block.BuildMerkleTree(&mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2) {
            return state.DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"),
                REJECT_INVALID, "bad-txnmrklroot", true);
        }

        // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the merkle root of a block,
        // while still invalidating it.
        if (mutated)
            return state.DoS(100, error("CheckBlock() : duplicate transaction"),
                REJECT_INVALID, "bad-txns-duplicate", true);
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.

    // Size limits
    unsigned int nMaxBlockSize = MAX_BLOCK_SIZE_CURRENT;
    if (block.vtx.empty() || block.vtx.size() > nMaxBlockSize || ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > nMaxBlockSize) {
        return state.DoS(100, error("CheckBlock() : size limits failed"),
            REJECT_INVALID, "bad-blk-length");
    }

    // First transaction must be coinbase, the rest must not be
    if (block.vtx.empty() || !block.vtx[0].IsCoinBase()) {
        return state.DoS(100, error("CheckBlock() : first tx is not coinbase"),
            REJECT_INVALID, "bad-cb-missing");
    }
    for (unsigned int i = 1; i < block.vtx.size(); i++)
        if (block.vtx[i].IsCoinBase())
            return state.DoS(100, error("CheckBlock() : more than one coinbase"),
                REJECT_INVALID, "bad-cb-multiple");

    if (block.IsProofOfStake()) {
        // Coinbase output should be empty if proof-of-stake block
        if (block.vtx[0].vout.size() != 1 || !block.vtx[0].vout[0].IsEmpty())
            return state.DoS(100, error("CheckBlock() : coinbase output not empty for proof-of-stake block"));

        // Second transaction must be coinstake, the rest must not be
        if (block.vtx.empty() || !block.vtx[1].IsCoinStake())
            return state.DoS(100, error("CheckBlock() : second tx is not coinstake"));
        for (unsigned int i = 2; i < block.vtx.size(); i++)
            if (block.vtx[i].IsCoinStake())
                return state.DoS(100, error("CheckBlock() : more than one coinstake"));
    }

    // ----------- swiftTX transaction scanning -----------
    if (IsSporkActive(SPORK_3_SWIFTTX_BLOCK_FILTERING)) {
        BOOST_FOREACH (const CTransaction& tx, block.vtx) {
            if (!tx.IsCoinBase()) {
                //only reject blocks when it's based on complete consensus
                BOOST_FOREACH (const CTxIn& in, tx.vin) {
                    if (mapLockedInputs.count(in.prevout)) {
                        if (mapLockedInputs[in.prevout] != tx.GetHash()) {
                            mapRejectedBlocks.insert(make_pair(block.GetHash(), GetTime()));
                            LogPrintf("CheckBlock() : found conflicting transaction with transaction lock %s %s\n", mapLockedInputs[in.prevout].ToString(), tx.GetHash().ToString());
                            return state.DoS(0, error("CheckBlock() : found conflicting transaction with transaction lock"),
                                REJECT_INVALID, "conflicting-tx-ix");
                        }
                    }
                }
            }
        }
    } else {
        LogPrintf("CheckBlock() : skipping transaction locking checks\n");
    }

    // Check transactions
    bool fZerocoinActive = block.GetVersion() >= CBlockHeader::VERSION5;
    vector<CBigNum> vBlockSerials;
    for (const CTransaction& tx : block.vtx) {
        if (!CheckTransaction(tx, fZerocoinActive, state))
            return error("CheckBlock() : CheckTransaction failed");

        // double check that there are no double spent zCOLX spends in this block
        if (tx.IsZerocoinSpend()) {
            for (const CTxIn txIn : tx.vin) {
                if (txIn.scriptSig.IsZerocoinSpend()) {
                    libzerocoin::CoinSpend spend = TxInToZerocoinSpend(txIn);
                    if (count(vBlockSerials.begin(), vBlockSerials.end(), spend.getCoinSerialNumber()))
                        return state.DoS(100, error("%s : Double spending of zCOLX serial %s in block\n Block: %s",
                                                    __func__, spend.getCoinSerialNumber().GetHex(), block.ToString()));
                    vBlockSerials.emplace_back(spend.getCoinSerialNumber());
                }
            }
        }
    }


    unsigned int nSigOps = 0;
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        nSigOps += GetLegacySigOpCount(tx);
    }

    unsigned int nMaxBlockSigOps = fZerocoinActive ? MAX_BLOCK_SIGOPS_CURRENT : MAX_BLOCK_SIGOPS_LEGACY;
    if (nSigOps > nMaxBlockSigOps)
        return state.DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"),
            REJECT_INVALID, "bad-blk-sigops", true);

    return true;
}

bool CheckWork(const CBlock block, CBlockIndex* const pindexPrev)
{
    if (pindexPrev == NULL)
        return error("%s : null pindexPrev for block %s", __func__, block.GetHash().ToString().c_str());

    if (Params().SkipProofOfWorkCheck())
        return true;

    unsigned int nBitsRequired = GetNextWorkRequired(pindexPrev, &block);

    if (block.IsProofOfWork() && (pindexPrev->nHeight + 1 <= 68589)) {
        double n1 = ConvertBitsToDouble(block.nBits);
        double n2 = ConvertBitsToDouble(nBitsRequired);

        if (abs(n1 - n2) > n1 * 0.5) {
            return error("%s : incorrect proof of work (DGW pre-fork) - %f %f %f at %d", __func__, abs(n1 - n2), n1, n2, pindexPrev->nHeight + 1);
        }

        return true;
    }

    if (block.nBits != nBitsRequired)
        return error("%s : incorrect proof of work at %d", __func__, pindexPrev->nHeight + 1);

    if (block.IsProofOfStake()) {
        uint256 hashProofOfStake;
        uint256 hash = block.GetHash();

        if(!CheckProofOfStake(block, hashProofOfStake)) {
            LogPrintf("WARNING: ProcessBlock(): check proof-of-stake failed for block %s\n", hash.ToString().c_str());
            return false;
        }
        if(!mapProofOfStake.count(hash)) // add to mapProofOfStake
            mapProofOfStake.insert(make_pair(hash, hashProofOfStake));
    }

    return true;
}

bool ContextualCheckBlockHeader(const CBlockHeader& block, CValidationState& state, CBlockIndex* const pindexPrev)
{
    const uint256 hash = block.GetHash();

    if (hash == Params().HashGenesisBlock())
        return true;

    if (!pindexPrev)
        return error("%s : null pindexPrev for block %s", __func__, hash.ToString());

    const int nHeight = pindexPrev->nHeight + 1;

    //If this is a reorg, check that it is not too deep
    int64_t nMaxReorganizationDepth = GetSporkValue(SPORK_19_MAX_REORGANIZATION_DEPTH);
    if (nMaxReorganizationDepth > 0 && chainActive.Height() - nHeight >= nMaxReorganizationDepth)
        return state.DoS(1, error("%s: forked chain older than max reorganization depth (height %d)", __func__, nHeight));

    // Check timestamp against prev
    if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast()) {
        LogPrintf("Block time = %d , GetMedianTimePast = %d \n", block.GetBlockTime(), pindexPrev->GetMedianTimePast());
        return state.Invalid(error("%s : block's timestamp is too early", __func__),
            REJECT_INVALID, "time-too-old");
    }

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!Checkpoints::CheckBlock(nHeight, hash))
        return state.DoS(100, error("%s : rejected by checkpoint lock-in at %d", __func__, nHeight),
            REJECT_CHECKPOINT, "checkpoint mismatch");

    // Don't accept any forks from the main chain prior to last checkpoint
    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint();
    if (pcheckpoint && nHeight < pcheckpoint->nHeight)
        return state.DoS(0, error("%s : forked chain older than last checkpoint (height %d)", __func__, nHeight));

    // Reject previous block.nVersion blocks when 95% (75% on testnet) of the network has upgraded
    for (int i = 2; i <= CBlockHeader::CURRENT_VERSION; ++i) {
        if (block.nVersion < i &&
            CBlockIndex::IsSuperMajority(i, pindexPrev, Params().RejectBlockOutdatedMajority())) {
                return state.Invalid(error("%s : rejected nVersion=%d block", __func__, block.nVersion), REJECT_OBSOLETE, "bad-version");
        }
    }

    // Zerocoin version header must be used after Params().Zerocoin_StartHeight(). And never before.
    const int32_t nZCver = CBlockHeader::VERSION5;
    if (nHeight >= Params().Zerocoin_StartHeight()) {
        if (block.GetVersion() < nZCver)
            return state.DoS(50, error("%s : block version must be %d+ after ZerocoinStartHeight", __func__, nZCver),
                REJECT_INVALID, "block-version");
    } else {
        if (block.GetVersion() >= nZCver)
            return state.DoS(50, error("%s : block version must be below %d before ZerocoinStartHeight", __func__, nZCver),
                REJECT_INVALID, "block-version");
    }

    return true;
}

bool IsBlockHashInChain(const uint256& hashBlock)
{
    if (hashBlock == 0 || !mapBlockIndex.count(hashBlock))
        return false;

    return chainActive.Contains(mapBlockIndex[hashBlock]);
}

bool IsTransactionInChain(uint256 txId, int& nHeightTx)
{
    uint256 hashBlock;
    CTransaction tx;
    GetTransaction(txId, tx, hashBlock, true);
    if (!IsBlockHashInChain(hashBlock))
        return false;

    nHeightTx = mapBlockIndex.at(hashBlock)->nHeight;
    return true;
}

bool ContextualCheckBlock(const CBlock& block, CValidationState& state, CBlockIndex* const pindexPrev)
{
    const int nHeight = pindexPrev == NULL ? 0 : pindexPrev->nHeight + 1;

    // Check that all transactions are finalized
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        string reason;
        if (!IsFinalTx(tx, reason, nHeight, block.GetBlockTime())) {
            return state.DoS(10, error("%s : contains a non-final transaction, %s", __func__, reason), REJECT_INVALID, "bad-txns-nonfinal");
        }
    }

    // Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
    // if 750 of the last 1,000 blocks are version 2 or greater (51/100 if testnet):
    if (block.nVersion >= CBlockHeader::VERSION2 &&
        CBlockIndex::IsSuperMajority(CBlockHeader::VERSION2, pindexPrev, Params().EnforceBlockUpgradeMajority())) {
        const CScript expect = CScript() << nHeight;
        const CScript scriptSig = block.vtx[0].vin[0].scriptSig;
        if (scriptSig.size() < expect.size() || !std::equal(expect.begin(), expect.end(), scriptSig.begin())) {
            return state.DoS(100, error("%s : block height(%d) mismatch in coinbase(%s)", __func__, nHeight, HexStr(scriptSig)), REJECT_INVALID, "bad-cb-height");
        }
    }

    return true;
}

bool AcceptBlockHeader(const CBlock& block, CValidationState& state, CBlockIndex** ppindex)
{
    AssertLockHeld(cs_main);
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex* pindex = NULL;

    // TODO : ENABLE BLOCK CACHE IN SPECIFIC CASES
    if (miSelf != mapBlockIndex.end()) {
        // Block header is already known.
        pindex = miSelf->second;
        if (ppindex)
            *ppindex = pindex;
        if (pindex->nStatus & BLOCK_FAILED_MASK)
            return state.Invalid(error("%s : block is marked invalid", __func__), 0, "duplicate");
        return true;
    }

    if (!CheckBlockHeader(block, state, false)) {
        LogPrintf("AcceptBlockHeader(): CheckBlockHeader failed \n");
        return false;
    }

    // Get prev block index
    CBlockIndex* pindexPrev = NULL;
    if (hash != Params().HashGenesisBlock()) {
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(0, error("%s : prev block %s not found", __func__, block.hashPrevBlock.ToString().c_str()), 0, "bad-prevblk");
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK) {
            //If this "invalid" block is an exact match from the checkpoints, then reconsider it
            if (pindex && Checkpoints::CheckBlock(pindex->nHeight - 1, block.hashPrevBlock, true)) {
                LogPrintf("%s : Reconsidering block %s height %d\n", __func__, pindexPrev->GetBlockHash().GetHex(), pindexPrev->nHeight);
                CValidationState statePrev;
                ReconsiderBlock(statePrev, pindexPrev);
                if (statePrev.IsValid()) {
                    ActivateBestChain(statePrev);
                    return true;
                }
            }

            return state.DoS(100, error("%s : prev block height=%d hash=%s is invalid, unable to add block %s", __func__, pindexPrev->nHeight, block.hashPrevBlock.GetHex(), block.GetHash().GetHex()),
                             REJECT_INVALID, "bad-prevblk");
        }

    }

    if (!ContextualCheckBlockHeader(block, state, pindexPrev))
        return false;

    if (pindex == NULL)
        pindex = AddToBlockIndex(block);

    if (ppindex)
        *ppindex = pindex;

    return true;
}

bool AcceptBlock(CBlock& block, CValidationState& state, CBlockIndex** ppindex, CDiskBlockPos* dbp, bool fAlreadyCheckedBlock)
{
    AssertLockHeld(cs_main);

    CBlockIndex*& pindex = *ppindex;

    // Get prev block index
    CBlockIndex* pindexPrev = NULL;
    if (block.GetHash() != Params().HashGenesisBlock()) {
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(0, error("%s : prev block %s not found", __func__, block.hashPrevBlock.ToString().c_str()), 0, "bad-prevblk");
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK) {
            //If this "invalid" block is an exact match from the checkpoints, then reconsider it
            if (Checkpoints::CheckBlock(pindexPrev->nHeight, block.hashPrevBlock, true)) {
                LogPrintf("%s : Reconsidering block %s height %d\n", __func__, pindexPrev->GetBlockHash().GetHex(), pindexPrev->nHeight);
                CValidationState statePrev;
                ReconsiderBlock(statePrev, pindexPrev);
                if (statePrev.IsValid()) {
                    ActivateBestChain(statePrev);
                    return true;
                }
            }
            return state.DoS(100, error("%s : prev block %s is invalid, unable to add block %s", __func__, block.hashPrevBlock.GetHex(), block.GetHash().GetHex()),
                             REJECT_INVALID, "bad-prevblk");
        }
    }

    if (block.GetHash() != Params().HashGenesisBlock() && !CheckWork(block, pindexPrev))
        return false;

    if (!AcceptBlockHeader(block, state, &pindex))
        return false;

    if (pindex->nStatus & BLOCK_HAVE_DATA) {
        // TODO: deal better with duplicate blocks.
        // return state.DoS(20, error("AcceptBlock() : already have block %d %s", pindex->nHeight, pindex->GetBlockHash().ToString()), REJECT_DUPLICATE, "duplicate");
        LogPrintf("AcceptBlock() : already have block %d %s", pindex->nHeight, pindex->GetBlockHash().ToString());
        return true;
    }

    if ((!fAlreadyCheckedBlock && !CheckBlock(block, state)) || !ContextualCheckBlock(block, state, pindex->pprev)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return false;
    }

    if (block.IsProofOfStake()) {
        AssertLockHeld(cs_main);

        // Blocks arrives in order, so if prev block is not the tip then we are on a fork.
        // Extra info: duplicated blocks are skipping this checks, so we don't have to worry about those here.
        const bool isBlockFromFork = pindexPrev != nullptr && chainActive.Tip() != pindexPrev;

        // Coin stake
        CTransaction &stakeTxIn = block.vtx[1];

        // Inputs
        std::vector<CTxIn> pivInputs;
        std::vector<CTxIn> zPIVInputs;

        for (const CTxIn& stakeIn : stakeTxIn.vin) {
            if(stakeIn.scriptSig.IsZerocoinSpend()){
                zPIVInputs.push_back(stakeIn);
            }else{
                pivInputs.push_back(stakeIn);
            }
        }
        const bool hasPIVInputs = !pivInputs.empty();
        const bool hasZPIVInputs = !zPIVInputs.empty();

        // ZC started after PoS.
        // Check for serial double spent on the same block, TODO: Move this to the proper method..

        vector<CBigNum> inBlockSerials;
        for (const CTransaction& tx : block.vtx) {
            for (const CTxIn& in: tx.vin) {
                if(pindex->nHeight >= Params().Zerocoin_StartHeight()) {
                    if (in.scriptSig.IsZerocoinSpend()) {
                        CoinSpend spend = TxInToZerocoinSpend(in);
                        // Check for serials double spending in the same block
                        if (std::find(inBlockSerials.begin(), inBlockSerials.end(), spend.getCoinSerialNumber()) !=
                            inBlockSerials.end()) {
                            return state.DoS(100, error("%s: serial double spent on the same block", __func__));
                        }
                        inBlockSerials.push_back(spend.getCoinSerialNumber());
                    }
                }
                if(tx.IsCoinStake()) continue;
                if(hasPIVInputs)
                    // Check if coinstake input is double spent inside the same block
                    for (const CTxIn& pivIn : pivInputs){
                        if(pivIn.prevout == in.prevout){
                            // double spent coinstake input inside block
                            return error("%s: double spent coinstake input inside block", __func__);
                        }
                    }
            }
        }
        inBlockSerials.clear();

        int splitHeight = -1;
        int64_t nMaxReorganizationDepth = GetSporkValue(SPORK_19_MAX_REORGANIZATION_DEPTH);
        // Check whether is a fork or not
        if (isBlockFromFork) {
            // Start at the block we're adding on to
            CBlockIndex *prev = pindexPrev;

            int readBlock = 0;
            vector<CBigNum> vBlockSerials;
            CBlock bl;
            // Go backwards on the forked chain up to the split
            do {
                // Check if the forked chain is longer than the max reorg limit
                if(readBlock >= nMaxReorganizationDepth){
                    // TODO: Remove this chain from disk.
                    return error("%s: forked chain longer than maximum reorg limit", __func__);
                }

                if(!ReadBlockFromDisk(bl, prev))
                    // Previous block not on disk
                    return error("%s: previous block %s not on disk", __func__, prev->GetBlockHash().GetHex());
                // Increase amount of read blocks
                readBlock++;
                // Loop through every input from said block
                for (const CTransaction& t : bl.vtx) {
                    for (const CTxIn& in: t.vin) {
                        // Loop through every input of the staking tx
                        for (const CTxIn& stakeIn : pivInputs) {
                            // if it's already spent

                            // First regular staking check
                            if(hasPIVInputs) {
                                if (stakeIn.prevout == in.prevout) {
                                    return state.DoS(100, error("%s: input already spent on a previous block", __func__));
                                }

                                // Second, if there is zPoS staking then store the serials for later check
                                if(in.scriptSig.IsZerocoinSpend()){
                                    vBlockSerials.push_back(TxInToZerocoinSpend(in).getCoinSerialNumber());
                                }
                            }
                        }
                    }
                }

                prev = prev->pprev;

            } while (!chainActive.Contains(prev));

            // Split height
            splitHeight = prev->nHeight;

            // Now that this loop if completed. Check if we have zPIV inputs.
            if(hasZPIVInputs){
                for (const CTxIn& zPivInput : zPIVInputs) {
                    CoinSpend spend = TxInToZerocoinSpend(zPivInput);

                    // First check if the serials were not already spent on the forked blocks.
                    CBigNum coinSerial = spend.getCoinSerialNumber();
                    for(const CBigNum& serial : vBlockSerials){
                        if(serial == coinSerial){
                            return state.DoS(100, error("%s: serial double spent on fork", __func__));
                        }
                    }

                    // Now check if the serial exists before the chain split.
                    int nHeightTx = 0;
                    if (IsSerialInBlockchain(spend.getCoinSerialNumber(), nHeightTx)){
                        // if the height is nHeightTx > chainSplit means that the spent occurred after the chain split
                        if(nHeightTx <= splitHeight)
                            return state.DoS(100, error("%s: serial double spent on main chain", __func__));
                    }

                    if (!ContextualCheckCoinSpend(spend, pindex, stakeTxIn.GetHash(), true))
                        return state.DoS(100,error("%s: forked chain ContextualCheckZerocoinSpend failed for tx %s", __func__,
                                                   stakeTxIn.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-zpiv");

                    // Now only the ZKP left..
                    // As the spend maturity is 200, the acc value must be accumulated, otherwise it's not ready to be spent
                    CBigNum bnAccumulatorValue = 0;
                    if (!zerocoinDB->ReadAccumulatorValue(spend.getAccumulatorChecksum(), bnAccumulatorValue)) {
                        return state.DoS(100, error("%s: stake zerocoinspend not ready to be spent", __func__));
                    }

                    Accumulator accumulator(Params().Zerocoin_Params(), spend.getDenomination(), bnAccumulatorValue);
                    //Check that the coinspend is valid
                    if(!spend.Verify(accumulator))
                        return state.DoS(100, error("%s: zerocoin spend did not verify", __func__));
                }
            }
        }

        // If the stake is not a zPoS then let's check if the inputs were spent on the main chain
        const CCoinsViewCache coins(pcoinsTip);
        if(!stakeTxIn.IsZerocoinSpend()) {
            for (const CTxIn& in: stakeTxIn.vin) {
                const CCoins* coin = coins.AccessCoins(in.prevout.hash);

                if(!coin && !isBlockFromFork){
                    // No coins on the main chain
                    return error("%s: coin stake inputs not available on main chain, received height %d vs current %d", __func__, pindex->nHeight, chainActive.Height());
                }
                if(coin && !coin->IsAvailable(in.prevout.n)){
                    // If this is not available get the height of the spent and validate it with the forked height
                    // Check if this occurred before the chain split
                    if(!(isBlockFromFork && coin->nHeight > splitHeight)){
                        // Coins not available
                        return error("%s: coin stake inputs already spent in main chain", __func__);
                    }
                }
            }
        } else {
            if(!isBlockFromFork)
                for (const CTxIn& zPivInput : zPIVInputs) {
                        CoinSpend spend = TxInToZerocoinSpend(zPivInput);
                        if (!ContextualCheckCoinSpend(spend, pindex, stakeTxIn.GetHash()))
                            return state.DoS(100,error("%s: main chain ContextualCheckCoinSpend failed for tx %s", __func__,
                                    stakeTxIn.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-zpiv");
                }
        }
    }

    // Write block to history file
    int nHeight = pindex->nHeight;
    try {
        unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != NULL)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize + 8, nHeight, block.GetBlockTime(), dbp != NULL))
            return error("AcceptBlock() : FindBlockPos failed");
        if (dbp == NULL)
            if (!WriteBlockToDisk(block, blockPos))
                return state.Abort("Failed to write block");
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
            return error("AcceptBlock() : ReceivedBlockTransactions failed");
    } catch (std::runtime_error& e) {
        return state.Abort(std::string("System error: ") + e.what());
    }

    return true;
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired)
{
    unsigned int nToCheck = Params().ToCheckBlockUpgradeMajority();
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++) {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}

/** Turn the lowest '1' bit in the binary representation of a number into a '0'. */
int static inline InvertLowestOne(int n) { return n & (n - 1); }

/** Compute what height to jump back to with the CBlockIndex::pskip pointer. */
int static inline GetSkipHeight(int height)
{
    if (height < 2)
        return 0;

    // Determine which height to jump back to. Any number strictly lower than height is acceptable,
    // but the following expression seems to perform well in simulations (max 110 steps to go back
    // up to 2**18 blocks).
    return (height & 1) ? InvertLowestOne(InvertLowestOne(height - 1)) + 1 : InvertLowestOne(height);
}

CBlockIndex* CBlockIndex::GetAncestor(int height)
{
    if (height > nHeight || height < 0)
        return NULL;

    CBlockIndex* pindexWalk = this;
    int heightWalk = nHeight;
    while (heightWalk > height) {
        int heightSkip = GetSkipHeight(heightWalk);
        int heightSkipPrev = GetSkipHeight(heightWalk - 1);
        if (heightSkip == height ||
            (heightSkip > height && !(heightSkipPrev < heightSkip - 2 && heightSkipPrev >= height))) {
            // Only follow pskip if pprev->pskip isn't better than pskip->pprev.
            pindexWalk = pindexWalk->pskip;
            heightWalk = heightSkip;
        } else {
            pindexWalk = pindexWalk->pprev;
            heightWalk--;
        }
    }
    return pindexWalk;
}

const CBlockIndex* CBlockIndex::GetAncestor(int height) const
{
    return const_cast<CBlockIndex*>(this)->GetAncestor(height);
}

void CBlockIndex::BuildSkip()
{
    if (pprev)
        pskip = pprev->GetAncestor(GetSkipHeight(nHeight));
}

bool ProcessNewBlock(CValidationState& state, CI2pdNode* pfrom, CBlock* pblock, CDiskBlockPos* dbp)
{
    // Preliminary checks
    int64_t nStartTime = GetTimeMillis();
    bool checked = CheckBlock(*pblock, state);

    int64_t nCheckBlock = GetTimeMillis();
    LogPrintf("%s : CheckBlock: done in %ld msecs (%s)\n", __func__, GetTimeMillis() - nStartTime, pblock->GetHash().ToString());

    int nMints = 0;
    int nSpends = 0;
    for (const CTransaction tx : pblock->vtx) {
        if (tx.ContainsZerocoins()) {
            for (const CTxIn in : tx.vin) {
                if (in.scriptSig.IsZerocoinSpend())
                    nSpends++;
            }
            for (const CTxOut out : tx.vout) {
                if (out.IsZerocoinMint())
                    nMints++;
            }
        }
    }
    if (nMints || nSpends)
        LogPrintf("%s : block contains %d zCOLX mints and %d zCOLX spends\n", __func__, nMints, nSpends);

    // ppcoin: check proof-of-stake
    // Limited duplicity on stake: prevents block flood attack
    // Duplicate stake allowed only when there is orphan child block
    //if (pblock->IsProofOfStake() && setStakeSeen.count(pblock->GetProofOfStake())/* && !mapOrphanBlocksByPrev.count(hash)*/)
    //    return error("ProcessNewBlock() : duplicate proof-of-stake (%s, %d) for block %s", pblock->GetProofOfStake().first.ToString().c_str(), pblock->GetProofOfStake().second, pblock->GetHash().ToString().c_str());

    // NovaCoin: check proof-of-stake block signature
    if (!pblock->CheckBlockSignature())
        return error("ProcessNewBlock() : bad proof-of-stake block signature");

    int64_t nCheckBlockSignature = GetTimeMillis();
    LogPrintf("%s : CheckBlockSignature: %ld msecs (%s)\n", __func__, GetTimeMillis() - nCheckBlock, pblock->GetHash().ToString());

    if (pblock->GetHash() != Params().HashGenesisBlock() && pfrom != NULL) {
        //if we get this far, check if the prev block is our prev block, if not then request sync and return false
        BlockMap::iterator mi = mapBlockIndex.find(pblock->hashPrevBlock);
        if (mi == mapBlockIndex.end()) {
            LogPrintf("%s : req getblocks: %ld msecs\n", __func__, GetTimeMillis() - nStartTime);
            pfrom->PushMessage("getblocks", chainActive.GetLocator(), uint256(0));
            return false;
        }
    }

    TRY_LOCK(cs_main, lockMain);
    if (lockMain) {
        TRY_LOCK(mempool.cs, lockMempool);
        if (!lockMempool) {
            LogPrintf("%s : mempool.cs failed: %s\n", __func__, ""); //LocksHeld());
        }
    } else {
        LogPrintf("%s : cs_main failed: %s\n", __func__, ""); // LocksHeld());
    }

    // DLOCKSFIX: TRY_LOCK is safer here, the only piece doing an unconditional lock
    LOCK2(cs_main, mempool.cs);
    //LOCK(cs_main);   // Replaces the former TRY_LOCK loop because busy waiting wastes too much resources

    int64_t nLockCsMain = GetTimeMillis();
    LogPrintf("%s : cs_main: done in %ld msecs\n", __func__, GetTimeMillis() - nCheckBlockSignature);

    if (nLockCsMain - nCheckBlockSignature > 200)
        LogPrintf("%s : cs_main lock taking too long: %ld msecs\n", __func__, nLockCsMain - nCheckBlockSignature);

    MarkBlockAsReceived(pblock->GetHash());
    if (!checked) {
        return error("%s : CheckBlock FAILED for block %s", __func__, pblock->GetHash().GetHex());
    }

    int64_t nMarkBlockAsReceived = GetTimeMillis();
    LogPrintf("%s : MarkBlockAsReceived: done in %ld msecs\n", __func__, GetTimeMillis() - nLockCsMain);

    // Store to disk
    CBlockIndex* pindex = nullptr;
    bool ret = AcceptBlock(*pblock, state, &pindex, dbp, checked);
    if (pindex && pfrom) {
        mapBlockSource[pindex->GetBlockHash()] = pfrom->GetId();
    }

    int64_t nAcceptBlock = GetTimeMillis();
    LogPrintf("%s : AcceptBlock: done in %ld milliseconds, height=%d (%s)\n",
              __func__, GetTimeMillis() - nMarkBlockAsReceived, pindex ? pindex->nHeight : 0, pblock->GetHash().ToString());

    CheckBlockIndex();

    int64_t nCheckBlockIndex = GetTimeMillis();
    LogPrintf("%s : CheckBlockIndex: done in %ld milliseconds, height=%d (%s)\n",
              __func__, GetTimeMillis() - nAcceptBlock, pindex ? pindex->nHeight : 0, pblock->GetHash().ToString());

    if (!ret) {
        // Check spamming
        if (pindex && pfrom && GetBoolArg("-blockspamfilter", DEFAULT_BLOCK_SPAM_FILTER)) {
            CNodeState *nodestate = State(pfrom->GetId());
            if (nodestate != nullptr) {
                nodestate->nodeBlocks.onBlockReceived(pindex->nHeight);
                bool nodeStatus = true;
                // UpdateState will return false if the node is attacking us or update the score and return true.
                nodeStatus = nodestate->nodeBlocks.updateState(state, nodeStatus);
                int nDoS = 0;
                if (state.IsInvalid(nDoS)) {
                    if (nDoS > 0)
                        Misbehaving(pfrom->GetId(), nDoS);
                    nodeStatus = false;
                }
                if (!nodeStatus)
                    return error("%s : AcceptBlock FAILED - block spam protection", __func__);
            }
        }
        return error("%s : AcceptBlock FAILED", __func__);
    }
    // END_LOCK(cs_main); // previous, revisit

    if (!ActivateBestChain(state, pblock, checked))
        return error("%s : ActivateBestChain failed", __func__);

    int64_t nActivateBestChain = GetTimeMillis();
    LogPrintf("%s : ActivateBestChain: done in %ld milliseconds, height=%d (%s)\n",
              __func__, GetTimeMillis() - nCheckBlockIndex, pindex ? pindex->nHeight : 0, pblock->GetHash().ToString());

    if (!fLiteMode) {
        if (masternodeSync.RequestedMasternodeAssets > MASTERNODE_SYNC_LIST) {
            obfuScationPool.NewBlock();
            masternodePayments.ProcessBlock(GetHeight() + 10);
            budget.NewBlock();
        }
    }

    int64_t nRequestedMasternodeAssets = GetTimeMillis();
    LogPrintf("%s : RequestedMasternodeAssets: done in %ld milliseconds, height=%d (%s)\n",
              __func__, GetTimeMillis() - nActivateBestChain, pindex ? pindex->nHeight : 0, pblock->GetHash().ToString());

    if (pwalletMain) {
        // If turned on MultiSend will send a transaction (or more) on the after maturity of a stake
        if (pwalletMain->isMultiSendEnabled())
            pwalletMain->MultiSend();

        // If turned on Auto Combine will scan wallet for dust to combine
        if (pwalletMain->fCombineDust)
            pwalletMain->AutoCombineDust();
    }

    int64_t nMultiSend = GetTimeMillis();
    LogPrintf("%s : MultiSend: done in %ld milliseconds, height=%d (%s)\n",
              __func__, GetTimeMillis() - nRequestedMasternodeAssets, pindex ? pindex->nHeight : 0, pblock->GetHash().ToString());

    LogPrintf("%s : ACCEPTED in %ld milliseconds with size=%d, height=%d, hash=%s\n",
              __func__, GetTimeMillis() - nStartTime, pblock->GetSerializeSize(SER_DISK, CLIENT_VERSION),
              pindex ? pindex->nHeight : 0, pblock->GetHash().ToString());

    return true;
}

bool TestBlockValidity(CValidationState& state, const CBlock& block, CBlockIndex* const pindexPrev, bool fCheckPOW, bool fCheckMerkleRoot)
{
    AssertLockHeld(cs_main);
    assert(pindexPrev == chainActive.Tip());

    CCoinsViewCache viewNew(pcoinsTip);
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;

    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(block, state, pindexPrev))
        return false;
    if (!CheckBlock(block, state, fCheckPOW, fCheckMerkleRoot))
        return false;
    if (!ContextualCheckBlock(block, state, pindexPrev))
        return false;
    if (!ConnectBlock(block, state, &indexDummy, viewNew, true))
        return false;
    assert(state.IsValid());

    return true;
}


bool AbortNode(const std::string& strMessage, const std::string& userMessage)
{
    strMiscWarning = strMessage;
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occured, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

FILE* OpenDiskFile(const CDiskBlockPos& pos, const char* prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    boost::filesystem::path path = GetBlockPosFilename(pos, prefix);
    boost::filesystem::create_directories(path.parent_path());
    FILE* file = fopen(path.string().c_str(), "rb+");
    if (!file && !fReadOnly)
        file = fopen(path.string().c_str(), "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos& pos, bool fReadOnly)
{
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* OpenUndoFile(const CDiskBlockPos& pos, bool fReadOnly)
{
    return OpenDiskFile(pos, "rev", fReadOnly);
}

boost::filesystem::path GetBlockPosFilename(const CDiskBlockPos& pos, const char* prefix)
{
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex* InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;

    //mark as PoS seen
    if (pindexNew->IsProofOfStake())
        setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));

    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB(string& strError)
{
    if (!pblocktree->LoadBlockIndexGuts())
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const PAIRTYPE(uint256, CBlockIndex*) & item : mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH (const PAIRTYPE(int, CBlockIndex*) & item, vSortedByHeight) {
        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    mapBlocksUnlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && (pindex->nChainTx || pindex->pprev == NULL))
            setBlockIndexCandidates.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK && (!pindexBestInvalid || pindex->nChainWork > pindexBestInvalid->nChainWork))
            pindexBestInvalid = pindex;
        if (pindex->pprev)
            pindex->BuildSkip();
        if (pindex->IsValid(BLOCK_VALID_TREE) && (pindexBestHeader == NULL || CBlockIndexWorkComparator()(pindexBestHeader, pindex)))
            pindexBestHeader = pindex;
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, vinfoBlockFile[nLastBlockFile].ToString());
    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    set<int> setBlkDataFiles;
    for (const PAIRTYPE(uint256, CBlockIndex*) & item : mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        CDiskBlockPos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull()) {
            return false;
        }
    }

    //Check if the shutdown procedure was followed on last client exit
    bool fLastShutdownWasPrepared = true;
    pblocktree->ReadFlag("shutdown", fLastShutdownWasPrepared);
    LogPrintf("%s: Last shutdown was prepared: %s\n", __func__, fLastShutdownWasPrepared);

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    fReindex |= fReindexing;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    LogPrintf("LoadBlockIndexDB(): transaction index %s\n", fTxIndex ? "enabled" : "disabled");

    // If this is written true before the next client init, then we know the shutdown process failed
    pblocktree->WriteFlag("shutdown", false);

    // Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end())
        return true;
    chainActive.SetTip(it->second);

    PruneBlockIndexCandidates();

    LogPrintf("LoadBlockIndexDB(): hashBestChain=%s height=%d date=%s progress=%f\n",
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
        Checkpoints::GuessVerificationProgress(chainActive.Tip()));

    return true;
}

CVerifyDB::CVerifyDB()
{
    uiInterface.ShowProgress(_("Verifying blocks..."), 0);
}

CVerifyDB::~CVerifyDB()
{
    uiInterface.ShowProgress("", 100);
}

bool CVerifyDB::VerifyDB(CCoinsView* coinsview, int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (chainActive.Tip() == NULL || chainActive.Tip()->pprev == NULL)
        return true;

    // Verify blocks in the best chain
    if (nCheckDepth <= 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > chainActive.Height())
        nCheckDepth = chainActive.Height();
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    CValidationState state;
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev) {
        boost::this_thread::interruption_point();
        uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100)))));
        if (pindex->nHeight < chainActive.Height() - nCheckDepth)
            break;
        CBlock block;
        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex))
            return error("VerifyDB() : *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(block, state))
            return error("VerifyDB() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!undo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
                    return error("VerifyDB() : *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.GetCacheSize() + pcoinsTip->GetCacheSize()) <= nCoinCacheSize) {
            bool fClean = true;
            if (!DisconnectBlock(block, state, pindex, coins, &fClean))
                return error("VerifyDB() : *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            pindexState = pindex->pprev;
            if (!fClean) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else
                nGoodTransactions += block.vtx.size();
        }
        if (ShutdownRequested())
            return true;
    }
    if (pindexFailure)
        return error("VerifyDB() : *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", chainActive.Height() - pindexFailure->nHeight + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex* pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex))
                return error("VerifyDB() : *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            if (!ConnectBlock(block, state, pindex, coins, false))
                return error("VerifyDB() : *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        }
    }

    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainActive.Height() - pindexState->nHeight, nGoodTransactions);

    return true;
}

void UnloadBlockIndex()
{
    mapBlockIndex.clear();
    setBlockIndexCandidates.clear();
    chainActive.SetTip(NULL);
    pindexBestInvalid = NULL;
}

bool LoadBlockIndex(string& strError)
{
    // Load block index from databases
    if (!fReindex && !LoadBlockIndexDB(strError))
        return false;
    return true;
}


bool InitBlockIndex()
{
    LOCK(cs_main);
    // Check whether we're already initialized
    if (chainActive.Genesis() != NULL)
        return true;

    // Use the provided setting for -txindex in the new database
    fTxIndex = GetBoolArg("-txindex", true);
    pblocktree->WriteFlag("txindex", fTxIndex);
    LogPrintf("Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
    if (!fReindex) {
        try {
            CBlock& block = const_cast<CBlock&>(Params().GenesisBlock());
            // Start new block file
            unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize + 8, 0, block.GetBlockTime()))
                return error("LoadBlockIndex() : FindBlockPos failed");
            if (!WriteBlockToDisk(block, blockPos))
                return error("LoadBlockIndex() : writing genesis block to disk failed");
            CBlockIndex* pindex = AddToBlockIndex(block);
            if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
                return error("LoadBlockIndex() : genesis block not accepted");
            if (!ActivateBestChain(state, &block))
                return error("LoadBlockIndex() : genesis block cannot be activated");
            // Force a chainstate write so that when we VerifyDB in a moment, it doesnt check stale data
            return FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
        } catch (std::runtime_error& e) {
            return error("LoadBlockIndex() : failed to initialize block database: %s", e.what());
        }
    }

    return true;
}


bool LoadExternalBlockFile(FILE* fileIn, CDiskBlockPos* dbp)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2 * MAX_BLOCK_SIZE_CURRENT, MAX_BLOCK_SIZE_CURRENT + 8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++;         // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(Params().MessageStart()[0]);
                nRewind = blkdat.GetPos() + 1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, Params().MessageStart(), MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SIZE_CURRENT)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != Params().HashGenesisBlock() && mapBlockIndex.find(block.hashPrevBlock) == mapBlockIndex.end()) {
                    LogPrint("reindex", "%s: Out of order block %s, parent %s not known\n", __func__, hash.ToString(),
                        block.hashPrevBlock.ToString());
                    if (dbp)
                        mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                    continue;
                }

                // process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 || (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                    CValidationState state;
                    if (ProcessNewBlock(state, NULL, &block, dbp))
                        nLoaded++;
                    if (state.IsError())
                        break;
                } else if (hash != Params().HashGenesisBlock() && mapBlockIndex[hash]->nHeight % 1000 == 0) {
                    LogPrintf("Block Import: already had block %s at height %d\n", hash.ToString(), mapBlockIndex[hash]->nHeight);
                }

                // Recursively process earlier encountered successors of this block
                deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator, std::multimap<uint256, CDiskBlockPos>::iterator> range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it = range.first;
                        if (ReadBlockFromDisk(block, it->second)) {
                            LogPrintf("%s: Processing out of order child %s of %s\n", __func__, block.GetHash().ToString(),
                                head.ToString());
                            CValidationState dummy;
                            if (ProcessNewBlock(dummy, NULL, &block, &it->second)) {
                                nLoaded++;
                                queue.push_back(block.GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                    }
                }
            } catch (std::exception& e) {
                LogPrintf("%s : Deserialize or I/O error - %s", __func__, e.what());
            }
        }
    } catch (std::runtime_error& e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

void static CheckBlockIndex()
{
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    // so we have the genesis block in mapBlockIndex but no active chain.  (A few of the tests when
    // iterating the block tree require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex*, CBlockIndex*> forward;
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        forward.insert(std::make_pair(it->second->pprev, it->second));
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangeGenesis = forward.equal_range(NULL);
    CBlockIndex* pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    assert(rangeGenesis.first == rangeGenesis.second); // There is only one index entry with parent NULL.

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    CBlockIndex* pindexFirstInvalid = NULL;         // Oldest ancestor of pindex which is invalid.
    CBlockIndex* pindexFirstMissing = NULL;         // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex* pindexFirstNotTreeValid = NULL;    // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    CBlockIndex* pindexFirstNotChainValid = NULL;   // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not).
    CBlockIndex* pindexFirstNotScriptsValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not).
    while (pindex != NULL) {
        nNodes++;
        if (pindexFirstInvalid == NULL && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == NULL && !(pindex->nStatus & BLOCK_HAVE_DATA)) pindexFirstMissing = pindex;
        if (pindex->pprev != NULL && pindexFirstNotTreeValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) pindexFirstNotTreeValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotChainValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) pindexFirstNotChainValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotScriptsValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) pindexFirstNotScriptsValid = pindex;

        // Begin: actual consistency checks.
        if (pindex->pprev == NULL) {
            // Genesis block checks.
            assert(pindex->GetBlockHash() == Params().HashGenesisBlock()); // Genesis block's hash must match.
            assert(pindex == chainActive.Genesis());                       // The current active chain's genesis block must be this block.
        }
        // HAVE_DATA is equivalent to VALID_TRANSACTIONS and equivalent to nTx > 0 (we stored the number of transactions in the block)
        assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0));
        if (pindex->nChainTx == 0) assert(pindex->nSequenceId == 0); // nSequenceId can't be set for blocks that aren't linked
        // All parents having data is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to nChainTx being set.
        assert((pindexFirstMissing != NULL) == (pindex->nChainTx == 0));                                             // nChainTx == 0 is used to signal that all parent block's transaction data is available.
        assert(pindex->nHeight == nHeight);                                                                          // nHeight must be consistent.
        assert(pindex->pprev == NULL || pindex->nChainWork >= pindex->pprev->nChainWork);                            // For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight < nHeight)));                                // The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid == NULL);                                                                     // All mapBlockIndex entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) assert(pindexFirstNotTreeValid == NULL);       // TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) assert(pindexFirstNotChainValid == NULL);     // CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) assert(pindexFirstNotScriptsValid == NULL); // SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == NULL) {
            // Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) == 0); // The failed mask cannot be set for blocks without invalid parents.
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && pindexFirstMissing == NULL) {
            if (pindexFirstInvalid == NULL) { // If this block sorts at least as good as the current tip and is valid, it must be in setBlockIndexCandidates.
                assert(setBlockIndexCandidates.count(pindex));
            }
        } else { // If this block sorts worse than the current tip, it cannot be in setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && pindex->nStatus & BLOCK_HAVE_DATA && pindexFirstMissing != NULL) {
            if (pindexFirstInvalid == NULL) { // If this block has block data available, some parent doesn't, and has no invalid parents, it must be in mapBlocksUnlinked.
                assert(foundInUnlinked);
            }
        } else { // If this block does not have block data available, or all parents do, it cannot be in mapBlocksUnlinked.
            assert(!foundInUnlinked);
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        // End: actual consistency checks.

        // Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = forward.equal_range(pindex);
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node.
        // Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = NULL;
            if (pindex == pindexFirstMissing) pindexFirstMissing = NULL;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = NULL;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = NULL;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = NULL;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                assert(rangePar.first != rangePar.second); // Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}

//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (!CLIENT_VERSION_IS_RELEASE)
        strStatusBar = _("This is a pre-release test build - use at your own risk - do not use for staking or merchant applications!");

    if (GetBoolArg("-testsafemode", false))
        strStatusBar = strRPC = "testsafemode enabled";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "") {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    if (fLargeWorkForkFound) {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.");
    } else if (fLargeWorkInvalidChainFound) {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH (PAIRTYPE(const uint256, CAlert) & item, mapAlerts) {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority) {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}


//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(const CInv& inv)
{
    switch (inv.type) {
    case MSG_TX: {
        bool txInMap = false;
        txInMap = mempool.exists(inv.hash);
        return txInMap || mapOrphanTransactions.count(inv.hash) ||
               pcoinsTip->HaveCoins(inv.hash);
    }
    case MSG_DSTX:
        return mapObfuscationBroadcastTxes.count(inv.hash);
    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash);
    case MSG_TXLOCK_REQUEST:
        return mapTxLockReq.count(inv.hash) ||
               mapTxLockReqRejected.count(inv.hash);
    case MSG_TXLOCK_VOTE:
        return mapTxLockVote.count(inv.hash);
    case MSG_SPORK:
        return mapSporks.count(inv.hash);
    case MSG_MASTERNODE_WINNER:
        if (masternodePayments.mapMasternodePayeeVotes.count(inv.hash)) {
            masternodeSync.AddedMasternodeWinner(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_VOTE:
        if (budget.mapSeenMasternodeBudgetVotes.count(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_PROPOSAL:
        if (budget.mapSeenMasternodeBudgetProposals.count(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_FINALIZED_VOTE:
        if (budget.mapSeenFinalizedBudgetVotes.count(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_FINALIZED:
        if (budget.mapSeenFinalizedBudgets.count(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_MASTERNODE_ANNOUNCE:
        if (mnodeman.mapSeenMasternodeBroadcast.count(inv.hash)) {
            masternodeSync.AddedMasternodeList(inv.hash);
            return true;
        }
        return false;
    case MSG_MASTERNODE_PING:
        return mnodeman.mapSeenMasternodePing.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}


void static ProcessGetData(CI2pdNode* pfrom)
{
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    // DLOCKSFIX: order of locks: 
    // cs_vRecvMsg (net thread), cs_main (main: ProcessGetData), cs_vSend (net: PushMessage)
    // cs_vSend (net thread: SendMessages), cs_main (main: SendMessages) 
    {
    LOCK(cs_main);

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv& inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK) {
                bool send = false;
                BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end()) {
                    if (chainActive.Contains(mi->second)) {
                        send = true;
                    } else {
                        // To prevent fingerprinting attacks, only send blocks outside of the active
                        // chain if they are valid, and no more than a max reorg depth than the best header
                        // chain we know about.
                        int64_t nMaxReorganizationDepth = GetSporkValue(SPORK_19_MAX_REORGANIZATION_DEPTH);
                        send = mi->second->IsValid(BLOCK_VALID_SCRIPTS) && (pindexBestHeader != NULL);
                        if (nMaxReorganizationDepth > 0)
                            send = send && (chainActive.Height() - mi->second->nHeight < nMaxReorganizationDepth);
                        if (!send)
                            LogPrintf("ProcessGetData(): ignoring request from peer=%i (%s) for old block that isn't in the main chain\n", pfrom->GetId(), pfrom->GetIdentity());
                    }
                }
                // Don't send not-validated blocks
                if (send && (mi->second->nStatus & BLOCK_HAVE_DATA)) {
                    // Send block from disk
                    CBlock block;
                    if (!ReadBlockFromDisk(block, (*mi).second))
                        assert(!"cannot load block from disk");
                    if (inv.type == MSG_BLOCK)
                        pfrom->PushMessage("block", block);
                    else // MSG_FILTERED_BLOCK)
                    {
                        LOCK(pfrom->cs_filter);
                        if (pfrom->pfilter) {
                            CMerkleBlock merkleBlock(block, *pfrom->pfilter);
                            pfrom->PushMessage("merkleblock", merkleBlock);
                            // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                            // This avoids hurting performance by pointlessly requiring a round-trip
                            // Note that there is currently no way for a node to request any single transactions we didnt send here -
                            // they must either disconnect and retry or request the full block.
                            // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                            // however we MUST always provide at least what the remote peer needs
                            typedef std::pair<unsigned int, uint256> PairType;
                            BOOST_FOREACH (PairType& pair, merkleBlock.vMatchedTxn)
                                if (!pfrom->setInventoryKnown.count(CInv(MSG_TX, pair.second)))
                                    pfrom->PushMessage("tx", block.vtx[pair.first]);
                        }
                        // else
                        // no response
                    }

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue) {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, chainActive.Tip()->GetBlockHash()));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            } else if (inv.IsKnownType()) {
                // Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_TX) {
                    CTransaction tx;
                    if (mempool.lookup(inv.hash, tx)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage("tx", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_VOTE) {
                    if (mapTxLockVote.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockVote[inv.hash];
                        pfrom->PushMessage("txlvote", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_REQUEST) {
                    if (mapTxLockReq.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockReq[inv.hash];
                        pfrom->PushMessage("ix", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_SPORK) {
                    if (mapSporks.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapSporks[inv.hash];
                        pfrom->PushMessage("spork", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_MASTERNODE_WINNER) {
                    if (masternodePayments.mapMasternodePayeeVotes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << masternodePayments.mapMasternodePayeeVotes[inv.hash];
                        pfrom->PushMessage("mnw", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_BUDGET_VOTE) {
                    if (budget.mapSeenMasternodeBudgetVotes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << budget.mapSeenMasternodeBudgetVotes[inv.hash];
                        pfrom->PushMessage("mvote", ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_BUDGET_PROPOSAL) {
                    if (budget.mapSeenMasternodeBudgetProposals.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << budget.mapSeenMasternodeBudgetProposals[inv.hash];
                        pfrom->PushMessage("mprop", ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_BUDGET_FINALIZED_VOTE) {
                    if (budget.mapSeenFinalizedBudgetVotes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << budget.mapSeenFinalizedBudgetVotes[inv.hash];
                        pfrom->PushMessage("fbvote", ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_BUDGET_FINALIZED) {
                    if (budget.mapSeenFinalizedBudgets.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << budget.mapSeenFinalizedBudgets[inv.hash];
                        pfrom->PushMessage("fbs", ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_MASTERNODE_ANNOUNCE) {
                    if (mnodeman.mapSeenMasternodeBroadcast.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mnodeman.mapSeenMasternodeBroadcast[inv.hash];
                        pfrom->PushMessage("mnb", ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_MASTERNODE_PING) {
                    if (mnodeman.mapSeenMasternodePing.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mnodeman.mapSeenMasternodePing[inv.hash];
                        pfrom->PushMessage("mnp", ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_DSTX) {
                    if (mapObfuscationBroadcastTxes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapObfuscationBroadcastTxes[inv.hash].tx << mapObfuscationBroadcastTxes[inv.hash].vin << mapObfuscationBroadcastTxes[inv.hash].vchSig << mapObfuscationBroadcastTxes[inv.hash].sigTime;

                        pfrom->PushMessage("dstx", ss);
                        pushed = true;
                    }
                }


                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            GetMainSignals().Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);
    }

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage("notfound", vNotFound);
    }
}

bool fRequestedSporksIDB = false;
bool static ProcessMessage(CI2pdNode* pfrom, string strCommand, CDataStream& vRecv, int64_t nTimeReceived)
{
    RandAddSeedPerfmon();
    LogPrint("net", "received: %s (%u bytes) peer=%d (%s)\n", SanitizeString(strCommand), vRecv.size(), pfrom->id, pfrom->GetIdentity());

    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0) {
        LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }

    if (strCommand == "version") {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0) {
            pfrom->PushMessage("reject", strCommand, REJECT_DUPLICATE, string("Duplicate version message"));
            Misbehaving(pfrom->GetId(), 1);
            return false;
        }

        // PIVX: We use certain sporks during IBD, so check to see if they are
        // available. If not, ask the first peer connected for them.
        bool fMissingSporks = !pSporkDB->SporkExists(SPORK_14_NEW_PROTOCOL_ENFORCEMENT) &&
                !pSporkDB->SporkExists(SPORK_15_NEW_PROTOCOL_ENFORCEMENT_2) &&
                !pSporkDB->SporkExists(SPORK_20_ZEROCOIN_MAINTENANCE_MODE);

        if (fMissingSporks || !fRequestedSporksIDB){
            LogPrintf("asking peer for sporks\n");
            pfrom->PushMessage("getsporks");
            fRequestedSporksIDB = true;
        }

        int64_t nTime;
        CI2PAddress addrMe;
        CI2PAddress addrFrom;
        uint64_t nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (pfrom->DisconnectOldProtocol(ActiveProtocol(), strCommand))
            return false;

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> LIMITED_STRING(pfrom->strSubVer, 256);
            pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);
        }
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;
        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1) {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        pfrom->addrLocal = addrMe;
        if (pfrom->fInbound && addrMe.IsRoutable()) {
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        // Potentially mark this peer as a preferred download peer.
        UpdatePreferredDownload(pfrom, State(pfrom->GetId()));

        // Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound) {
            // Advertise our address
            if (fListen && !IsInitialBlockDownload()) {
                CI2PAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable()) {
                    LogPrintf("ProcessMessages: advertizing address %s\n", addr.ToString());
                    pfrom->PushAddress(addr);
                } else if (IsPeerAddrLocalGood(pfrom)) {
                    addr.SetIP(pfrom->addrLocal);
                    LogPrintf("ProcessMessages: advertizing address %s\n", addr.ToString());
                    pfrom->PushAddress(addr);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000) {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CI2pUrl)pfrom->addr) == (CI2pUrl)addrFrom) {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH (PAIRTYPE(const uint256, CAlert) & item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        string remoteAddr;
        if (fLogIPs)
            remoteAddr = ", peeraddr=" + pfrom->addr.ToString();

        LogPrintf("receive version message: %s: version %d, blocks=%d, us=%s, peer=%d%s\n",
            pfrom->cleanSubVer, pfrom->nVersion,
            pfrom->nStartingHeight, addrMe.ToString(), pfrom->id,
            remoteAddr);

        int64_t nTimeOffset = nTime - GetTime();
        pfrom->nTimeOffset = nTimeOffset;
        AddTimeData(pfrom->addr, nTimeOffset);
    }


    else if (pfrom->nVersion == 0) {
        // Must have a version message before anything else
        Misbehaving(pfrom->GetId(), 1);
        return false;
    }


    else if (strCommand == "verack") {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        // Mark this node as currently connected, so we update its timestamp later.
        if (pfrom->fNetworkNode) {
            LOCK(cs_main);
            State(pfrom->GetId())->fCurrentlyConnected = true;
        }

        if (pfrom->nVersion >= SENDHEADERS_VERSION) {
            // Tell our peer we prefer to receive headers rather than inv's
            // We send this to non-NODE NETWORK peers as well, because even
            // non-NODE NETWORK peers can announce blocks (such as pruning
            // nodes)
            pfrom->PushMessage("sendheaders");
        }
    }


    else if (strCommand == "sendheaders")
    {
        LOCK(cs_main);
        State(pfrom->GetId())->fPreferHeaders = true;
    }


    else if (strCommand == "addr") {
        vector<CI2PAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000) {
            Misbehaving(pfrom->GetId(), 20);
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        vector<CI2PAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        BOOST_FOREACH (CI2PAddress& addr, vAddr) {
            boost::this_thread::interruption_point();

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable()) {
                // Relay to a limited number of other nodes
                {
                    // I2PDEADLOCK: TRY_LOCK(cs_vRecvMsg), LOCK(cs_vNodes). Tunnel thread: LOCK(cs_vNodes), TRY_LOCK(cs_vRecvMsg)
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr << 32) ^ ((GetTime() + hashAddr) / (24 * 60 * 60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CI2pdNode*> mapMix;
                    BOOST_FOREACH (CI2pdNode* pnode, vNodes) {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CI2pdNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }


    else if (strCommand == "inv") {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            Misbehaving(pfrom->GetId(), 20);
            return error("message inv size() = %u", vInv.size());
        }

        std::vector<CInv> vToFetch;

        // DLOCKSFIX: order of locks: 
        // cs_vRecvMsg (net thread), cs_main (main: ProcessMessage), cs_vSend (PushMessage)
        // cs_vSend (net thread: SendMessages), cs_main (main: SendMessages) 
        {
        LOCK(cs_main);

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            const CInv& inv = vInv[nInv];

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(inv);
            LogPrint("net", "got inv: %s  %s peer=%d (%s)\n", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom->id, pfrom->GetIdentity());

            if (!fAlreadyHave && !fImporting && !fReindex && inv.type != MSG_BLOCK)
                pfrom->AskFor(inv);


            if (inv.type == MSG_BLOCK) {
                UpdateBlockAvailability(pfrom->GetId(), inv.hash);
                if (!fAlreadyHave && !fImporting && !fReindex && !mapBlocksInFlight.count(inv.hash) && CanDirectFetch()) {
                    // Add this to the list of blocks to request
                    vToFetch.push_back(inv);
                    LogPrint("net", "getblocks (%d) %s to peer=%d (%s)\n", pindexBestHeader->nHeight, inv.hash.ToString(), pfrom->id, pfrom->GetIdentity());
                }
            }

            // Track requests for our stuff
            GetMainSignals().Inventory(inv.hash);

            if (pfrom->nSendSize > (SendBufferSize() * 2)) {
                Misbehaving(pfrom->GetId(), 50);
                return error("send buffer size() = %u", pfrom->nSendSize);
            }
        }
        }

        if (!vToFetch.empty())
            pfrom->PushMessage("getdata", vToFetch);
    }


    else if (strCommand == "getdata") {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            Misbehaving(pfrom->GetId(), 20);
            return error("message getdata size() = %u", vInv.size());
        }

        if (fDebug || (vInv.size() != 1))
            LogPrint("net", "received getdata (%u invsz) peer=%d (%s)\n", 
                vInv.size(), pfrom->id, pfrom->GetIdentity());

        if ((fDebug && vInv.size() > 0) || (vInv.size() == 1))
            LogPrint("net", "received getdata for: %s peer=%d (%s)\n", 
                vInv[0].ToString(), pfrom->id, pfrom->GetIdentity());

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
    }


    else if (strCommand == "getblocks") {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = FindForkInGlobalIndex(chainActive, locator);

        // Send the rest of the chain
        if (pindex)
            pindex = chainActive.Next(pindex);
        int nLimit = 500;
        LogPrint("net", "getblocks %d to %s limit %d from peer=%d (%s)\n", (pindex ? pindex->nHeight : -1), hashStop == uint256(0) ? "end" : hashStop.ToString(), nLimit, pfrom->id, pfrom->GetIdentity());
        for (; pindex; pindex = chainActive.Next(pindex)) {
            if (pindex->GetBlockHash() == hashStop) {
                LogPrint("net", "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0) {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                LogPrint("net", "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == "getheaders") {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        if (IsInitialBlockDownload() && !pfrom->fWhitelisted) {
            LogPrint("net", "Ignoring getheaders from peer=%d because node is in initial block download\n", pfrom->id);
            return true;
        }

        CBlockIndex* pindex = NULL;
        if (locator.IsNull()) {
            // If locator is null, return the hashStop block
            BlockMap::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        } else {
            // Find the last block the caller has in the main chain
            pindex = FindForkInGlobalIndex(chainActive, locator);
            if (pindex)
                pindex = chainActive.Next(pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrint("net", "getheaders %d to %s from peer=%d (%s)\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), pfrom->id, pfrom->GetIdentity());
        for (; pindex; pindex = chainActive.Next(pindex)) {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }

        // pindex can be NULL either if we sent chainActive.Tip() OR
        // if our peer has chainActive.Tip() (and thus we are sending an empty
        // headers message). In both cases it's safe to update
        // pindexBestHeaderSent to be our tip.
        CNodeState *nodestate = State(pfrom->GetId());
        if (nodestate)
            nodestate->pindexBestHeaderSent = pindex ? pindex : chainActive.Tip();

        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx" || strCommand == "dstx") {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CTransaction tx;

        //masternode signed transaction
        bool ignoreFees = false;
        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;

        if (strCommand == "tx") {
            vRecv >> tx;
        } else if (strCommand == "dstx") {
            //these allow masternodes to publish a limited amount of free transactions
            vRecv >> tx >> vin >> vchSig >> sigTime;

            CMasternode* pmn = mnodeman.Find(vin);
            if (pmn != NULL) {
                if (!pmn->allowFreeTx) {
                    //multiple peers can send us a valid masternode transaction
                    if (fDebug) LogPrintf("dstx: Masternode sending too many transactions %s\n", tx.GetHash().ToString());
                    return true;
                }

                std::string strMessage = tx.GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);

                std::string errorMessage = "";
                if (!obfuScationSigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
                    LogPrintf("dstx: Got bad masternode address signature %s \n", vin.ToString());
                    //pfrom->Misbehaving(20);
                    return false;
                }

                LogPrintf("dstx: Got Masternode transaction %s\n", tx.GetHash().ToString());

                ignoreFees = true;
                pmn->allowFreeTx = false;

                if (!mapObfuscationBroadcastTxes.count(tx.GetHash())) {
                    CObfuscationBroadcastTx dstx;
                    dstx.tx = tx;
                    dstx.vin = vin;
                    dstx.vchSig = vchSig;
                    dstx.sigTime = sigTime;

                    mapObfuscationBroadcastTxes.insert(make_pair(tx.GetHash(), dstx));
                }
            }
        }

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);

        bool fMissingInputs = false;
        bool fMissingZerocoinInputs = false;
        CValidationState state;

        mapAlreadyAskedFor.erase(inv);

        if (!tx.IsZerocoinSpend() && AcceptToMemoryPool(mempool, state, tx, true, &fMissingInputs, false, ignoreFees)) {
            mempool.check(pcoinsTip);
            RelayTransaction(tx);
            vWorkQueue.push_back(inv.hash);

            LogPrint("mempool", "AcceptToMemoryPool: peer=%d %s : accepted %s (poolsz %u)\n",
                     pfrom->id, pfrom->cleanSubVer,
                     tx.GetHash().ToString(),
                     mempool.mapTx.size());

            // Recursively process any orphan transactions that depended on this one
            set<NodeId> setMisbehaving;
            for(unsigned int i = 0; i < vWorkQueue.size(); i++) {
                map<uint256, set<uint256> >::iterator itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue[i]);
                if(itByPrev == mapOrphanTransactionsByPrev.end())
                    continue;
                for(set<uint256>::iterator mi = itByPrev->second.begin();
                    mi != itByPrev->second.end();
                    ++mi) {
                    const uint256 &orphanHash = *mi;
                    const CTransaction &orphanTx = mapOrphanTransactions[orphanHash].tx;
                    NodeId fromPeer = mapOrphanTransactions[orphanHash].fromPeer;
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;


                    if(setMisbehaving.count(fromPeer))
                        continue;
                    if(AcceptToMemoryPool(mempool, stateDummy, orphanTx, true, &fMissingInputs2)) {
                        LogPrint("mempool", "   accepted orphan tx %s\n", orphanHash.ToString());
                        RelayTransaction(orphanTx);
                        vWorkQueue.push_back(orphanHash);
                        vEraseQueue.push_back(orphanHash);
                    } else if(!fMissingInputs2) {
                        int nDos = 0;
                        if(stateDummy.IsInvalid(nDos) && nDos > 0) {
                            // Punish peer that gave us an invalid orphan tx
                            Misbehaving(fromPeer, nDos);
                            setMisbehaving.insert(fromPeer);
                            LogPrint("mempool", "   invalid orphan tx %s\n", orphanHash.ToString());
                        }
                        // Has inputs but not accepted to mempool
                        // Probably non-standard or insufficient fee/priority
                        LogPrint("mempool", "   removed orphan tx %s\n", orphanHash.ToString());
                        vEraseQueue.push_back(orphanHash);
                    }
                    mempool.check(pcoinsTip);
                }
            }

            BOOST_FOREACH (uint256 hash, vEraseQueue)EraseOrphanTx(hash);
        } else if (tx.IsZerocoinSpend() && AcceptToMemoryPool(mempool, state, tx, true, &fMissingZerocoinInputs, false, ignoreFees)) {
            //Presstab: ZCoin has a bunch of code commented out here. Is this something that should have more going on?
            //Also there is nothing that handles fMissingZerocoinInputs. Does there need to be?
            RelayTransaction(tx);
            LogPrint("mempool", "AcceptToMemoryPool: Zerocoinspend peer=%d %s : accepted %s (poolsz %u)\n",
                     pfrom->id, pfrom->cleanSubVer,
                     tx.GetHash().ToString(),
                     mempool.mapTx.size());
        } else if (fMissingInputs) {
            AddOrphanTx(tx, pfrom->GetId());

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0, GetArg("-maxorphantx", DEFAULT_MAX_ORPHAN_TRANSACTIONS));
            unsigned int nEvicted = LimitOrphanTxSize(nMaxOrphanTx);
            if (nEvicted > 0)
                LogPrint("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
        } else if (pfrom->fWhitelisted) {
            // Always relay transactions received from whitelisted peers, even
            // if they are already in the mempool (allowing the node to function
            // as a gateway for nodes hidden behind it).

            RelayTransaction(tx);
        }

        if (strCommand == "dstx") {
            CInv inv(MSG_DSTX, tx.GetHash());
            RelayInv(inv);
        }

        int nDoS = 0;
        if (state.IsInvalid(nDoS)) {
            LogPrint("mempool", "%s from peer=%d %s was not accepted into the memory pool: %s\n", tx.GetHash().ToString(),
                pfrom->id, pfrom->cleanSubVer,
                state.GetRejectReason());
            pfrom->PushMessage("reject", strCommand, state.GetRejectCode(),
                state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
            if (nDoS > 0)
                Misbehaving(pfrom->GetId(), nDoS);
        }
    }


    else if (strCommand == "headers" && !fImporting && !fReindex) // Ignore headers received while importing
    {
        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            Misbehaving(pfrom->GetId(), 20);
            return error("headers message size = %u", nCount);
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
        }

        LOCK(cs_main);

        if (nCount == 0) {
            // Nothing interesting. Stop asking this peers for more headers.
            return true;
        }
        CBlockIndex* pindexLast = NULL;
        BOOST_FOREACH (const CBlockHeader& header, headers) {
            CValidationState state;
            if (pindexLast != NULL && header.hashPrevBlock != pindexLast->GetBlockHash()) {
                Misbehaving(pfrom->GetId(), 20);
                return error("non-continuous headers sequence");
            }

            /*TODO: this has a CBlock cast on it so that it will compile. There should be a solution for this
             * before headers are reimplemented on mainnet
             */
            if (!AcceptBlockHeader((CBlock)header, state, &pindexLast)) {
                int nDoS;
                if (state.IsInvalid(nDoS)) {
                    if (nDoS > 0)
                        Misbehaving(pfrom->GetId(), nDoS);
                    std::string strError = "invalid header received " + header.GetHash().ToString();
                    return error(strError.c_str());
                }
            }
        }

        if (pindexLast)
            UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        if (nCount == MAX_HEADERS_RESULTS && pindexLast) {
            // Headers message had its maximum size; the peer may have more headers.
            // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
            // from there instead.
            LogPrintf("more getheaders (%d) to end to peer=%d (startheight:%d, %s)\n", pindexLast->nHeight, pfrom->id, pfrom->nStartingHeight, pfrom->GetIdentity());
            pfrom->PushMessage("getheaders", chainActive.GetLocator(pindexLast), uint256(0));
        }

        bool fCanDirectFetch = CanDirectFetch();
        CNodeState *nodestate = State(pfrom->GetId());

        NodeId nodeid = pfrom->GetId();
        bool isstalling = IsStalling(nodeid); // || WasStallingRecently(nodeid);
        if (isstalling) {
            LogPrint("blockdown", "%s : headers, is stalling, skip: %d \n", 
                __func__, nodeid);
        }
        // If this set of headers is valid and ends in a block with at least as
        // much work as our tip, download as much as possible.
        if (fCanDirectFetch && pindexLast->IsValid(BLOCK_VALID_TREE) && chainActive.Tip()->nChainWork <= pindexLast->nChainWork && !isstalling) {
            vector<CBlockIndex *> vToFetch;
            CBlockIndex *pindexWalk = pindexLast;
            // Calculate all the blocks we'd need to switch to pindexLast, up to a limit.
            while (pindexWalk && !chainActive.Contains(pindexWalk) && vToFetch.size() <= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                if (!(pindexWalk->nStatus & BLOCK_HAVE_DATA) &&
                        !mapBlocksInFlight.count(pindexWalk->GetBlockHash())) {
                    // We don't have this block, and it's not yet in flight.
                    vToFetch.push_back(pindexWalk);
                }
                pindexWalk = pindexWalk->pprev;
            }
            // If pindexWalk still isn't on our main chain, we're looking at a
            // very large reorg at a time we think we're close to caught up to
            // the main chain -- this shouldn't really happen.  Bail out on the
            // direct fetch and rely on parallel download instead.
            if (!chainActive.Contains(pindexWalk)) {
                LogPrint("net", "Large reorg, won't direct fetch to %s (%d)\n",
                        pindexLast->GetBlockHash().ToString(),
                        pindexLast->nHeight);
            } else {
                vector<CInv> vGetData;
                // Download as much as possible, from earliest to latest.
                BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vToFetch) {
                    if (nodestate && nodestate->nBlocksInFlight >= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                        // Can't download any more from this peer
                        break;
                    }
                    vGetData.push_back(CInv(MSG_BLOCK, pindex->GetBlockHash()));
                    MarkBlockAsInFlight(pfrom->GetId(), pindex->GetBlockHash(), pindex);
                    LogPrint("net", "Requesting block %s from  peer=%d (%s)\n",
                            pindex->GetBlockHash().ToString(), pfrom->id, pfrom->GetIdentity());
                }
                if (vGetData.size() > 1) {
                    if (LogAcceptCategory("blockdown") && vToFetch.size() > 0) 
                        LogPrint("blockdown", "%s : MarkBlockAsInFlight: %d, %d, %s \n", 
                            __func__, vGetData.size(), pindexLast->nHeight, Join(vToFetch));
                    
                    LogPrint("net", "Downloading blocks toward %s (%d) via headers direct fetch\n",
                            pindexLast->GetBlockHash().ToString(), pindexLast->nHeight);
                }
                if (vGetData.size() > 0) {
                    pfrom->PushMessage("getdata", vGetData);
                }
            }
        }

        CheckBlockIndex();
    }

    else if (strCommand == "block" && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlock block;
        vRecv >> block;
        uint256 hashBlock = block.GetHash();
        CInv inv(MSG_BLOCK, hashBlock);
        LogPrint("net", "received block %s peer=%d (%s)\n", 
            inv.hash.ToString(), pfrom->id, pfrom->GetIdentity());

        //sometimes we will be sent their most recent block and its not the one we want, in that case tell where we are
        if (!mapBlockIndex.count(block.hashPrevBlock)) {
            if (find(pfrom->vBlockRequested.begin(), pfrom->vBlockRequested.end(), hashBlock) != pfrom->vBlockRequested.end()) {
                //we already asked for this block, so lets work backwards and ask for the previous block
                pfrom->PushMessage("getblocks", chainActive.GetLocator(), block.hashPrevBlock);
                pfrom->vBlockRequested.push_back(block.hashPrevBlock);
            } else {
                //ask to sync to this block
                pfrom->PushMessage("getblocks", chainActive.GetLocator(), hashBlock);
                pfrom->vBlockRequested.push_back(hashBlock);
            }
        } else {
            pfrom->AddInventoryKnown(inv);

            // Try to process all blocks that we don't have
            BlockMap::const_iterator pindex = mapBlockIndex.find(block.GetHash());
            if (pindex != mapBlockIndex.end() && 0 != (pindex->second->nStatus & BLOCK_HAVE_DATA) && pindex->second->nHeight <= chainActive.Height()) {
                LogPrint("net", "%s : Already processed block (%d) %s, skipping ProcessNewBlock()\n", __func__, pindex->second->nHeight, block.GetHash().GetHex());
            } else {
                CValidationState state;
                // TRY_FIX: FIX: threads are stalling when syncing, give it a bit of extra time here?
                // MilliSleep(10);
                LogPrintf("%s : block cmd: %ld, (%s)\n", __func__, GetTimeMillis(), block.GetHash().ToString());
                ProcessNewBlock(state, pfrom, &block);

                int nDoS;
                if(state.IsInvalid(nDoS)) {
                    pfrom->PushMessage("reject", strCommand, state.GetRejectCode(),
                                       state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
                    if(nDoS > 0) {
                        TRY_LOCK(cs_main, lockMain);
                        if(lockMain) Misbehaving(pfrom->GetId(), nDoS);
                    }
                }
            }

            //disconnect this node if its old protocol version
            pfrom->DisconnectOldProtocol(ActiveProtocol(), strCommand);
        }
    }


    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages.
    // Making users (which are behind NAT and can only make outgoing connections) ignore
    // getaddr message mitigates the attack.
    else if ((strCommand == "getaddr") && (pfrom->fInbound)) {
        pfrom->vAddrToSend.clear();
        vector<CI2PAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH (const CI2PAddress& addr, vAddr)
            pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool") {
        LOCK2(cs_main, pfrom->cs_filter);

        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        BOOST_FOREACH (uint256& hash, vtxid) {
            CInv inv(MSG_TX, hash);
            CTransaction tx;
            bool fInMemPool = mempool.lookup(hash, tx);
            if (!fInMemPool) continue; // another thread removed since queryHashes, maybe...
            if ((pfrom->pfilter && pfrom->pfilter->IsRelevantAndUpdate(tx)) ||
                (!pfrom->pfilter))
                vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ) {
                pfrom->PushMessage("inv", vInv);
                vInv.clear();
            }
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "ping") {
        if (pfrom->nVersion > BIP0031_VERSION) {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "pong") {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere, cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere, cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint("net", "pong peer=%d %s: %s, %x expected, %x received, %u bytes\n",
                pfrom->id,
                pfrom->cleanSubVer,
                sProblem,
                pfrom->nPingNonceSent,
                nonce,
                nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (fAlerts && strCommand == "alert") {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0) {
            if (alert.ProcessAlert()) {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH (CI2pdNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            } else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                Misbehaving(pfrom->GetId(), 10);
            }
        }
    }

    else if (!(nLocalServices & NODE_BLOOM) &&
             (strCommand == "filterload" ||
                 strCommand == "filteradd" ||
                 strCommand == "filterclear")) {
        LogPrintf("bloom message=%s\n", strCommand);
        Misbehaving(pfrom->GetId(), 100);
    }

    else if (strCommand == "filterload") {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
            // There is no excuse for sending a too-large filter
            Misbehaving(pfrom->GetId(), 100);
        else {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "filteradd") {
        vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            Misbehaving(pfrom->GetId(), 100);
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
                pfrom->pfilter->insert(vData);
            else
                Misbehaving(pfrom->GetId(), 100);
        }
    }


    else if (strCommand == "filterclear") {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "reject") {
        if (fDebug) {
            try {
                string strMsg;
                unsigned char ccode;
                string strReason;
                vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode >> LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);

                ostringstream ss;
                ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

                if (strMsg == "block" || strMsg == "tx") {
                    uint256 hash;
                    vRecv >> hash;
                    ss << ": hash " << hash.ToString();
                }
                LogPrint("net", "Reject %s\n", SanitizeString(ss.str()));
            } catch (std::ios_base::failure& e) {
                // Avoid feedback loops by preventing reject messages from triggering a new reject message.
                LogPrint("net", "Unparseable reject message received\n");
            }
        }
    } else {
        //probably one the extensions
        obfuScationPool.ProcessMessageObfuscation(pfrom, strCommand, vRecv);
        mnodeman.ProcessMessage(pfrom, strCommand, vRecv);
        budget.ProcessMessage(pfrom, strCommand, vRecv);
        masternodePayments.ProcessMessageMasternodePayments(pfrom, strCommand, vRecv);
        ProcessMessageSwiftTX(pfrom, strCommand, vRecv);
        ProcessSpork(pfrom, strCommand, vRecv);
        masternodeSync.ProcessMessage(pfrom, strCommand, vRecv);
    }


    return true;
}

// Note: whenever a protocol update is needed toggle between both implementations (comment out the formerly active one)
//       so we can leave the existing clients untouched (old SPORK will stay on so they don't see even older clients).
//       Those old clients won't react to the changes of the other (new) SPORK because at the time of their implementation
//       it was the one which was commented out
int ActiveProtocol()
{
    if (IsSporkActive(SPORK_14_NEW_PROTOCOL_ENFORCEMENT))
        return MIN_PEER_PROTO_VERSION_AFTER_ENFORCEMENT;
    else
        return MIN_PEER_PROTO_VERSION_BEFORE_ENFORCEMENT;
/*
    if (IsSporkActive(SPORK_15_NEW_PROTOCOL_ENFORCEMENT_2))
        return MIN_PEER_PROTO_VERSION_AFTER_ENFORCEMENT;
    else
        return MIN_PEER_PROTO_VERSION_BEFORE_ENFORCEMENT;
*/
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CI2pdNode* pfrom)
{
    //if (fDebug)
    //    LogPrintf("ProcessMessages(%u messages)\n", pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom);

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    int64_t nStartTime = GetTimeMillis();
    int iMsg = 0;
    // LogPrintf("%s : start: %ld, (%d)\n", __func__, GetTimeMillis(), pfrom->vRecvMsg.size());

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;

        LogPrintf("%s : message: %ld, %d(%d)\n", __func__, GetTimeMillis() - nStartTime, iMsg++, pfrom->vRecvMsg.size());

        //if (fDebug)
        //    LogPrintf("ProcessMessages(message %u msgsz, %u bytes, complete:%s)\n",
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, Params().MessageStart(), MESSAGE_START_SIZE) != 0) {
            LogPrintf("PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n", SanitizeString(msg.hdr.GetCommand()), pfrom->id);
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid()) {
            LogPrintf("PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d\n", SanitizeString(hdr.GetCommand()), pfrom->id);
            continue;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum) {
            LogPrintf("ProcessMessages(%s, %u bytes): CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
                SanitizeString(strCommand), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try {
            fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime);
            boost::this_thread::interruption_point();
        } catch (std::ios_base::failure& e) {
            pfrom->PushMessage("reject", strCommand, REJECT_MALFORMED, string("error parsing message"));
            if (strstr(e.what(), "end of data")) {
                // Allow exceptions from under-length message on vRecv
                LogPrintf("ProcessMessages(%s, %u bytes): Exception '%s' caught, normally caused by a message being shorter than its stated length\n", SanitizeString(strCommand), nMessageSize, e.what());
            } else if (strstr(e.what(), "size too large")) {
                // Allow exceptions from over-long size
                LogPrintf("ProcessMessages(%s, %u bytes): Exception '%s' caught\n", SanitizeString(strCommand), nMessageSize, e.what());
            } else {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        } catch (boost::thread_interrupted) {
            throw;
        } catch (std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            LogPrintf("ProcessMessage(%s, %u bytes) FAILED peer=%d (%s)\n", SanitizeString(strCommand), nMessageSize, pfrom->id, pfrom->GetIdentity());

        break;
    }

    // LogPrintf("%s : end: %ld, (%d)\n", __func__, GetTimeMillis() - nStartTime, pfrom->vRecvMsg.size());

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}

bool AddressRefreshBroadcast()
{
    // Address refresh broadcast
    static int64_t nLastRebroadcast;
    if (!IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60)) {
        // DLOCKSFIX: order of locks: cs_main, cs_vSend, cs_vNodes
        // masternode-sync.cpp: cs_vNodes, cs_vSend
        // ...there's apparently no easy way to handle this?
        LOCK(cs_vNodes);
        BOOST_FOREACH (CI2pdNode* pnode, vNodes) {
            // Periodically clear setAddrKnown to allow refresh broadcasts
            if (nLastRebroadcast)
                pnode->setAddrKnown.clear();

            // Rebroadcast our address
            AdvertizeLocal(pnode);
        }
        if (!vNodes.empty())
            nLastRebroadcast = GetTime();

        return true;
    }
    return false;
}

bool SendMessages(CI2pdNode* pto, bool fSendTrickle)
{
    {
        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued) {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend) {
            uint64_t nonce = 0;
            while (nonce == 0) {
                GetRandBytes((unsigned char*)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION) {
                pto->nPingNonceSent = nonce;
                pto->PushMessage("ping", nonce);
            } else {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                pto->PushMessage("ping");
            }
        }

        TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        if (!lockMain)
            return true;

        //
        // Message: addr
        //
        if (fSendTrickle) {
            vector<CI2PAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH (const CI2PAddress& addr, pto->vAddrToSend) {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second) {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000) {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }

        CNodeState& state = *State(pto->GetId());
        if (state.fShouldBan) {
            if (pto->fWhitelisted)
                LogPrintf("Warning: not punishing whitelisted peer %s!\n", pto->addr.ToString());
            else {
                pto->fDisconnect = true;
                if (pto->addr.IsLocal())
                    LogPrintf("Warning: not banning local peer %s!\n", pto->addr.ToString());
                else {
                    CI2pdNode::Ban(pto->addr, BanReasonNodeMisbehaving);
                }
            }
            state.fShouldBan = false;
        }

        BOOST_FOREACH (const CBlockReject& reject, state.rejects)
            pto->PushMessage("reject", (string) "block", reject.chRejectCode, reject.strRejectReason, reject.hashBlock);
        state.rejects.clear();

        // Start block sync
        if (pindexBestHeader == NULL)
            pindexBestHeader = chainActive.Tip();
        bool fFetch = state.fPreferredDownload || (nPreferredDownload == 0 && !pto->fClient && !pto->fOneShot); // Download if this is a nice peer, or we have no nice peers and this one might do.
        if (!state.fSyncStarted && !pto->fClient && fFetch /*&& !fImporting*/ && !fReindex) {
            // Only actively request headers from a single peer, unless we're close to end of initial download.

            // I2PTESTNETFIX:
            int _initialIntervalSeconds = 6 * 60 * 60;
            if (Params().NetworkID() == CBaseChainParams::TESTNET && Params().IsBlockchainLateSynced())
                _initialIntervalSeconds = Params().GetBlockchainSyncedSeconds();

            // I2PTESTNET:
            if (nSyncStarted == 0 || pindexBestHeader->GetBlockTime() > GetAdjustedTime() - _initialIntervalSeconds) {
            // if (nSyncStarted == 0 || pindexBestHeader->GetBlockTime() > GetAdjustedTime() - 6 * 60 * 60) { // NOTE: was "close to today" and 24h in Bitcoin
                state.fSyncStarted = true;
                nSyncStarted++;
                //CBlockIndex *pindexStart = pindexBestHeader->pprev ? pindexBestHeader->pprev : pindexBestHeader;
                //LogPrint("net", "initial getheaders (%d) to peer=%d (startheight:%d)\n", pindexStart->nHeight, pto->id, pto->nStartingHeight);
                //pto->PushMessage("getheaders", chainActive.GetLocator(pindexStart), uint256(0));
                pto->PushMessage("getblocks", chainActive.GetLocator(chainActive.Tip()), uint256(0));
            }
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex /*&& !fImporting && !IsInitialBlockDownload()*/) {
            // AssertLockHeld(cs_main, mempool.cs, cs_wallet);
            // ...and LOCK(cs_vNodes) at start
            // => CWallet::ResendWalletTransactions() => RelayWalletTransaction
            GetMainSignals().Broadcast();
        }

        //
        // Try sending block announcements via headers
        //
        {
            // If we have less than MAX_BLOCKS_TO_ANNOUNCE in our
            // list of block hashes we're relaying, and our peer wants
            // headers announcements, then find the first header
            // not yet known to our peer but would connect, and send.
            // If no header would connect, or if we have too many
            // blocks, or if the peer doesn't want headers, just
            // add all to the inv queue.
            LOCK(pto->cs_inventory);
            vector<CBlock> vHeaders;
            bool fRevertToInv = (!state.fPreferHeaders || pto->vBlockHashesToAnnounce.size() > MAX_BLOCKS_TO_ANNOUNCE);
            CBlockIndex *pBestIndex = NULL; // last header queued for delivery
            ProcessBlockAvailability(pto->id); // ensure pindexBestKnownBlock is up-to-date
            if (!fRevertToInv) {
                bool fFoundStartingHeader = false;
                // Try to find first header that our peer doesn't have, and
                // then send all headers past that one.  If we come across any
                // headers that aren't on chainActive, give up.
                BOOST_FOREACH(const uint256 &hash, pto->vBlockHashesToAnnounce) {
                    BlockMap::iterator mi = mapBlockIndex.find(hash);
                    assert(mi != mapBlockIndex.end());
                    CBlockIndex *pindex = mi->second;
                    if (chainActive[pindex->nHeight] != pindex) {
                        // Bail out if we reorged away from this block
                        fRevertToInv = true;
                        break;
                    }
                    assert(pBestIndex == NULL || pindex->pprev == pBestIndex);
                    pBestIndex = pindex;
                    if (fFoundStartingHeader) {
                        // add this to the headers message
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else if (PeerHasHeader(&state, pindex)) {
                        continue; // keep looking for the first new block
                    } else if (pindex->pprev == NULL || PeerHasHeader(&state, pindex->pprev)) {
                        // Peer doesn't have this header but they do have the prior one.
                        // Start sending headers.
                        fFoundStartingHeader = true;
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else {
                        // Peer doesn't have this header or the prior one -- nothing will
                        // connect, so bail out.
                        fRevertToInv = true;
                        break;
                    }
                }
            }
            if (fRevertToInv) {
                // If falling back to using an inv, just try to inv the tip.
                // The last entry in vBlockHashesToAnnounce was our tip at some point
                // in the past.
                if (!pto->vBlockHashesToAnnounce.empty()) {
                    const uint256 &hashToAnnounce = pto->vBlockHashesToAnnounce.back();
                    BlockMap::iterator mi = mapBlockIndex.find(hashToAnnounce);
                    assert(mi != mapBlockIndex.end());
                    CBlockIndex *pindex = mi->second;
                    // Warn if we're announcing a block that is not on the main chain.
                    // This should be very rare and could be optimized out.
                    // Just log for now.
                    if (chainActive[pindex->nHeight] != pindex) {
                        LogPrint("net", "Announcing block %s not on main chain (tip=%s)\n",
                            hashToAnnounce.ToString(), chainActive.Tip()->GetBlockHash().ToString());
                    }
                    // If the peer announced this block to us, don't inv it back.
                    // (Since block announcements may not be via inv's, we can't solely rely on
                    // setInventoryKnown to track this.)
                    if (!PeerHasHeader(&state, pindex)) {
                        pto->PushInventory(CInv(MSG_BLOCK, hashToAnnounce));
                        LogPrint("net", "%s: sending inv peer=%d hash=%s (%s)\n", __func__,
                            pto->id, hashToAnnounce.ToString(), pto->GetIdentity());
                    }
                }
            } else if (!vHeaders.empty()) {
                if (vHeaders.size() > 1) {
                    LogPrint("net", "%s: %u headers, range (%s, %s), to peer=%d (%s)\n", __func__,
                            vHeaders.size(),
                            vHeaders.front().GetHash().ToString(),
                            vHeaders.back().GetHash().ToString(), pto->id, pto->GetIdentity());
                } else {
                    LogPrint("net", "%s: sending header %s to peer=%d\n", __func__,
                            vHeaders.front().GetHash().ToString(), pto->id);
                }
                pto->PushMessage("headers", vHeaders);
                state.pindexBestHeaderSent = pBestIndex;
            }
            pto->vBlockHashesToAnnounce.clear();
        }

        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            BOOST_FOREACH (const CInv& inv, pto->vInventoryToSend) {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle) {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    if (fTrickleWait) {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second) {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000) {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);

        // Detect whether we're stalling
        int64_t nNow = GetTimeMicros();
        unsigned int timeout = BLOCK_STALLING_TIMEOUT * 15;
        if (!pto->fDisconnect && state.nStallingSince && state.nStallingSince < nNow - 1000000 * timeout) {
            // Stalling only triggers when the block download window cannot move. During normal steady state,
            // the download window should be much larger than the to-be-downloaded set of blocks, so disconnection
            // should only happen during initial block download.
            LogPrintf("Peer=%d (%s) is stalling block download, disconnecting\n", 
                pto->id, pto->GetIdentity());
            pto->fDisconnect = true;
        }
        // In case there is a block that has been in flight from this peer for (2 + 0.5 * N) times the block interval
        // (with N the number of validated blocks that were in flight at the time it was requested), disconnect due to
        // timeout. We compensate for in-flight blocks to prevent killing off peers due to our own downstream link
        // being saturated. We only count validated in-flight blocks so peers can't advertize nonexisting block hashes
        // to unreasonably increase our timeout.
        if (!pto->fDisconnect && state.vBlocksInFlight.size() > 0 && state.vBlocksInFlight.front().nTime < nNow - 500000 * Params().TargetSpacing() * (4 + state.vBlocksInFlight.front().nValidatedQueuedBefore)) {
            LogPrintf("Timeout downloading block %s from peer=%d (%s), disconnecting\n", state.vBlocksInFlight.front().hash.ToString(), pto->id, pto->GetIdentity());
            pto->fDisconnect = true;
        }

        //
        // Message: getdata (blocks)
        //
        vector<CInv> vGetData;
        NodeId nodeid = pto->GetId();
        bool isstalling = IsStalling(nodeid) || WasStallingRecently(nodeid);
        if (isstalling) {
            LogPrint("blockdown", "%s : FindNextBlocksToDownload, was stalling, skip: %d \n", 
                __func__, nodeid);
        }
        if (!pto->fDisconnect && !pto->fClient && fFetch && !isstalling && 
            state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            vector<CBlockIndex*> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(pto->GetId(), MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload, staller);
            // if (vToDownload.size() > 0 || staller == pto->GetId())
            //     staller = -1;

            // repeat the check as we might be stalling now
            isstalling = IsStalling(nodeid); // || WasStallingRecently(nodeid);

            if (!isstalling) {
                if (LogAcceptCategory("blockdown") && vToDownload.size() > 0) 
                    LogPrint("blockdown", "%s : FindNextBlocksToDownload: %d, %d, %s \n", 
                        __func__, MAX_BLOCKS_IN_TRANSIT_PER_PEER, state.nBlocksInFlight, Join(vToDownload));

                BOOST_FOREACH (CBlockIndex* pindex, vToDownload) {
                    vGetData.push_back(CInv(MSG_BLOCK, pindex->GetBlockHash()));
                    MarkBlockAsInFlight(pto->GetId(), pindex->GetBlockHash(), pindex);
                    LogPrintf("Requesting block %s (%d) peer=%d (%s)\n", 
                        pindex->GetBlockHash().ToString(), pindex->nHeight, pto->id, pto->GetIdentity());
                }
                if (state.nBlocksInFlight == 0 && staller != -1) {
                    if (State(staller)->nStallingSince == 0) {
                        State(staller)->nStallingSince = nNow;
                        LogPrint("net", "Stall started peer=%d (%s)\n", 
                            staller, State(staller)->address.ToString());
                    }
                }
            }
        }

        //
        // Message: getdata (non-blocks)
        //
        while (!pto->fDisconnect && !pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow) {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv)) {
                if (fDebug)
                    LogPrint("net", "Requesting %s peer=%d\n", inv.ToString(), pto->id);
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000) {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);
    }
    return true;
}


bool CBlockUndo::WriteToDisk(CDiskBlockPos& pos, const uint256& hashBlock)
{
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("CBlockUndo::WriteToDisk : OpenUndoFile failed");

    // Write index header
    unsigned int nSize = fileout.GetSerializeSize(*this);
    fileout << FLATDATA(Params().MessageStart()) << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("CBlockUndo::WriteToDisk : ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << *this;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << *this;
    fileout << hasher.GetHash();

    return true;
}

bool CBlockUndo::ReadFromDisk(const CDiskBlockPos& pos, const uint256& hashBlock)
{
    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("CBlockUndo::ReadFromDisk : OpenBlockFile failed");

    // Read block
    uint256 hashChecksum;
    try {
        filein >> *this;
        filein >> hashChecksum;
    } catch (std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << *this;
    if (hashChecksum != hasher.GetHash())
        return error("CBlockUndo::ReadFromDisk : Checksum mismatch");

    return true;
}

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst, nHeightLast, DateTimeStrFormat("%Y-%m-%d", nTimeFirst), DateTimeStrFormat("%Y-%m-%d", nTimeLast));
}


class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup()
    {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();

        // orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanTransactionsByPrev.clear();
    }
} instance_of_cmaincleanup;
