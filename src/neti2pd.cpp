// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define BOOST_BIND_NO_PLACEHOLDERS

#if defined(HAVE_CONFIG_H)
#include "config/colx-config.h"
#endif

#include "neti2pd.h"

#include "addrman.h"
#include "chainparams.h"
#include "clientversion.h"
#include "miner.h"
#include "netbase.h"
#include "netdestination.h"
#include "obfuscation.h"
#include "primitives/transaction.h"
#include "scheduler.h"
#include "ui_interface.h"
#include "wallet.h"
#include "curl.h"
#include "context.h"
#include "autoupdatemodel.h"

#include <I2PService.h>
#include <I2PTunnel.h>
#include <I2PPureTunnel.h>
#include <ClientContext.h>
#include <Destination.h>
#include <Identity.h>

#ifdef WIN32
#include <string.h>
#else
#include <fcntl.h>
#endif

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/miniwget.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

//#define BOOST_BIND_NO_PLACEHOLDERS
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string/replace.hpp>

// Dump addresses to peers.dat every 15 minutes (900s)
#define DUMP_ADDRESSES_INTERVAL 900

#if !defined(HAVE_MSG_NOSIGNAL) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

// Fix for ancient MinGW versions, that don't have defined these in ws2tcpip.h.
// Todo: Can be removed when our pull-tester is upgraded to a modern MinGW version.
#ifdef WIN32
#ifndef PROTECTION_LEVEL_UNRESTRICTED
#define PROTECTION_LEVEL_UNRESTRICTED 10
#endif
#ifndef IPV6_PROTECTION_LEVEL
#define IPV6_PROTECTION_LEVEL 23
#endif
#endif

using namespace boost;
using namespace std;
using namespace i2p::client;

namespace
{
struct ListenTunnel {
    std::shared_ptr<I2PPureServerTunnel> tunnel;
    bool whitelisted;

    ListenTunnel(std::shared_ptr<I2PPureServerTunnel> inTunnel, bool whitelisted) : tunnel(inTunnel), whitelisted(whitelisted) {}
};
}

//
// Global state variables
//
bool fDiscover = true;
bool fListen = true;
uint64_t nLocalServices = NODE_NETWORK;
CCriticalSection cs_mapLocalHost;
map<CI2pUrl, LocalServiceInfo> mapLocalHost;
// ZC: this shouldn't be used any more? review // Q:
static bool vfReachable[NET_MAX] = {};
static bool vfLimited[NET_MAX] = {};
static CI2pdNode* pnodeLocalHost = NULL;
uint64_t nLocalHostNonce = 0;
static std::vector<ListenTunnel> vhListenTunnel;
CAddrMan addrman;
int nMaxConnections = 125;
int MAX_OUTBOUND_CONNECTIONS = 16;
bool fAddressesInitialized = false;

vector<CI2pdNode*> vNodes;
CCriticalSection cs_vNodes;
map<CInv, CDataStream> mapRelay;
deque<pair<int64_t, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;
limitedmap<CInv, int64_t> mapAlreadyAskedFor(MAX_INV_SZ);

static deque<string> vOneShots;
CCriticalSection cs_vOneShots;

set<CI2pUrl> setservAddNodeAddresses;
CCriticalSection cs_setservAddNodeAddresses;

vector<std::string> vAddedNodes;
CCriticalSection cs_vAddedNodes;

NodeId nLastNodeId = 0;
CCriticalSection cs_nLastNodeId;

static CSemaphore* semOutbound = NULL;
boost::condition_variable messageHandlerCondition;

// Signals for message handling
static CNodeSignals g_signals;
CNodeSignals& GetNodeSignals() { return g_signals; }

void AddOneShot(string strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(GetArg("-port", Params().GetDefaultPort()));
}

// find 'best' local address for a particular peer
bool GetLocal(CDestination& addr, const CI2pUrl* paddrPeer)
{
    if (!fListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (map<CI2pUrl, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); it++) {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore)) {
                addr = CDestination((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

// I2PDK: this is something that may still be needed alongside i2p stuff.

// get best local address for a particular peer as a CI2PAddress
// Otherwise, return the unroutable 0.0.0.0 but filled in with
// the normal parameters, since the IP may be changed to a useful
// one by discovery.
CI2PAddress GetLocalAddress(const CI2pUrl* paddrPeer)
{
    CI2PAddress ret(CDestination("0.0.0.0", GetListenPort()), 0);
    CDestination addr;
    if (GetLocal(addr, paddrPeer)) {
        ret = CI2PAddress(addr);
    }
    ret.nServices = nLocalServices;
    ret.nTime = GetAdjustedTime();
    return ret;
}

// I2PDK: not used any more?
//bool RecvLine(SOCKET hSocket, string& strLine)
//{
//    strLine = "";
//    while (true) {
//        char c;
//        int nBytes = recv(hSocket, &c, 1, 0);
//        if (nBytes > 0) {
//            if (c == '\n')
//                continue;
//            if (c == '\r')
//                return true;
//            strLine += c;
//            if (strLine.size() >= 9000)
//                return true;
//        } else if (nBytes <= 0) {
//            boost::this_thread::interruption_point();
//            if (nBytes < 0) {
//                int nErr = WSAGetLastError();
//                if (nErr == WSAEMSGSIZE)
//                    continue;
//                if (nErr == WSAEWOULDBLOCK || nErr == WSAEINTR || nErr == WSAEINPROGRESS) {
//                    MilliSleep(10);
//                    continue;
//                }
//            }
//            if (!strLine.empty())
//                return true;
//            if (nBytes == 0) {
//                // socket closed
//                LogPrintf("net: socket closed\n");
//                return false;
//            } else {
//                // socket error
//                int nErr = WSAGetLastError();
//                LogPrintf("net: recv failed: %s\n", NetworkErrorString(nErr));
//                return false;
//            }
//        }
//    }
//}

int GetnScore(const CDestination& addr)
{
    LOCK(cs_mapLocalHost);
    if (mapLocalHost.count(addr) == LOCAL_NONE)
        return 0;
    return mapLocalHost[addr].nScore;
}

// Is our peer's addrLocal potentially useful as an external IP source?
bool IsPeerAddrLocalGood(CI2pdNode* pnode)
{
    return fDiscover && pnode->addr.IsRoutable() && pnode->addrLocal.IsRoutable() &&
           !IsLimited(pnode->addrLocal.GetNetwork());
}

// pushes our own address to a peer
void AdvertizeLocal(CI2pdNode* pnode)
{
    if (fListen && pnode->fSuccessfullyConnected) {
        CI2PAddress addrLocal = GetLocalAddress(&pnode->addr);
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
        // I2PDK: this won't happen w/ i2p.

        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
                                              GetRand((GetnScore(addrLocal) > LOCAL_MANUAL) ? 8 : 2) == 0)) {
            // I2PDK: ...won't happen, we know our address the best, as we're creating the keys, priv./pub. etc.
            addrLocal.SetIP(pnode->addrLocal);
        }
        if (addrLocal.IsRoutable()) {
            LogPrintf("AdvertizeLocal: advertizing address %s\n", addrLocal.ToString());
            pnode->PushAddress(addrLocal);
        }
    }
}

// ZC: this shouldn't be used any more? review // Q:
void SetReachable(enum Network net, bool fFlag)
{
    LOCK(cs_mapLocalHost);
    vfReachable[net] = fFlag;
    if (net == NET_IPV6 && fFlag)
        vfReachable[NET_IPV4] = true;
}

// learn a new local address
bool AddLocal(const CDestination& addr, int nScore)
{
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    LogPrintf("AddLocal(%s,%i)\n", addr.ToString(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo& info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
        // ZC: this shouldn't be used any more? review // Q:
        SetReachable(addr.GetNetwork());
    }

    return true;
}

bool AddLocal(const CI2pUrl& addr, int nScore)
{
    return AddLocal(CDestination(addr, GetListenPort()), nScore);
}

bool RemoveLocal(const CDestination& addr)
{
    LOCK(cs_mapLocalHost);
    LogPrintf("RemoveLocal(%s)\n", addr.ToString());
    mapLocalHost.erase(addr);
    return true;
}

/** Make a particular network entirely off-limits (no automatic connects to it) */
void SetLimited(enum Network net, bool fLimited)
{
    if (net == NET_UNROUTABLE)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CI2pUrl& addr)
{
    return IsLimited(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CDestination& addr)
{
    {
        LOCK(cs_mapLocalHost);
        if (mapLocalHost.count(addr) == 0)
            return false;
        mapLocalHost[addr].nScore++;
    }
    return true;
}


/** check whether a given address is potentially local */
bool IsLocal(const CDestination& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

/** check whether a given network is one we can probably connect to */
bool IsReachable(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return !vfLimited[net];
    // ZC: off - this is where we turn it on/off to test, review // Q:
    //return vfReachable[net] && !vfLimited[net];
}

/** check whether a given address is in a network we can probably connect to */
bool IsReachable(const CI2pUrl& addr)
{
    enum Network net = addr.GetNetwork();
    return IsReachable(net);
}

void AddressCurrentlyConnected(const CDestination& addr)
{
    addrman.Connected(addr);
}


uint64_t CI2pdNode::nTotalBytesRecv = 0;
uint64_t CI2pdNode::nTotalBytesSent = 0;
CCriticalSection CI2pdNode::cs_totalBytesRecv;
CCriticalSection CI2pdNode::cs_totalBytesSent;

CI2pdNode* FindNode(const CI2pUrl& ip)
{
    LOCK(cs_vNodes);
    for (CI2pdNode* pnode : vNodes)
        if ((CI2pUrl)pnode->addr == ip)
            return (pnode);
    return NULL;
}

CI2pdNode* FindNode(const CI2pSubNet& subNet)
{
    LOCK(cs_vNodes);
    for (CI2pdNode* pnode : vNodes)
    if (subNet.Match((CI2pUrl)pnode->addr))
        return (pnode);
    return NULL;
}

CI2pdNode* FindNode(const std::string& addrName)
{
    LOCK(cs_vNodes);
    for (CI2pdNode* pnode : vNodes)
        if (pnode->addrName == addrName)
            return (pnode);
    return NULL;
}

CI2pdNode* FindNode(const CDestination& addr)
{
    LOCK(cs_vNodes);
    for (CI2pdNode* pnode : vNodes) {
        if (Params().NetworkID() == CBaseChainParams::REGTEST) {
            //if using regtest, just check the IP
            if ((CI2pUrl)pnode->addr == (CI2pUrl)addr)
                return (pnode);
        } else {
            if (pnode->addr == addr)
                return (pnode);
        }
    }
    return NULL;
}

void static HandleStreamCreated(std::shared_ptr<I2PPureClientTunnel> tunnel)
{
}

CI2pdNode* ConnectNode(CI2PAddress addrConnect, const char* pszDest, bool obfuScationMaster)
{
    using namespace std::placeholders;    // adds visibility of _1, _2, _3,...

    //CDestination addr = CDestination(CI2pUrl(hostname), port);

    if (pszDest == NULL) {
        // we clean masternode connections in CMasternodeMan::ProcessMasternodeConnections()
        // so should be safe to skip this and connect to local Hot MN on CActiveMasternode::ManageStatus()
        if (IsLocal(addrConnect) && !obfuScationMaster)
            return NULL;

        // Look for an existing connection
        CI2pdNode* pnode = FindNode((CDestination)addrConnect);
        if (pnode) {
            pnode->fObfuScationMaster = obfuScationMaster;

            pnode->AddRef();
            return pnode;
        }
    }

    /// debug print
    LogPrintf("net: trying connection %s lastseen=%.1fhrs\n",
        pszDest ? pszDest : addrConnect.ToString(),
        pszDest ? 0.0 : (double)(GetAdjustedTime() - addrConnect.nTime) / 3600.0);

    // Connect
    //I2PService tunnel;
    std::shared_ptr<I2PPureClientTunnel> tunnel;
    //bool proxyConnectionFailed = false;
    auto streamCreatedCallback = std::bind(HandleStreamCreated, _1); // , addrBind, fWhitelisted);
    if (pszDest ? ConnectClientTunnelByName(addrConnect, tunnel, pszDest, Params().GetDefaultPort(), nConnectTimeout, streamCreatedCallback) :
                  ConnectClientTunnel(addrConnect, tunnel, nConnectTimeout, streamCreatedCallback)) {
        // I2PDK: this doesn't apply any more, if anything similar will be handled internally within i2pd.
        //if (!IsSelectableSocket(tunnel)) {
        //    LogPrintf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
        //    CloseTunnel(tunnel);
        //    return NULL;
        //}

        addrman.Attempt(addrConnect);

        // Add node
        // I2PDK: outbound node/socket, i.e. the 'client' in our terms, it has a valid target 'addrName'
        // socket is created via ConnectSocket* variant.
        CI2pdNode* pnode = new CI2pdNode(tunnel, addrConnect, pszDest ? pszDest : "", false);
        pnode->AddRef();

        {
            LOCK(cs_vNodes);
            vNodes.push_back(pnode);
        }

        // this should be better, have some generic interface (but aint' easy as it's specific, we need to make tunnels have some base class that makes more sense, put it there).
        //auto clientTunnel = std::static_pointer_cast<I2PPureClientTunnel>(tunnel);
        //shared_from_this()
        //void HandleClientConnectionCreated(std::shared_ptr<i2p::client::I2PPureTunnelConnection> connection);
        // auto node_shared = std::shared_ptr<CI2pdNode>(pnode);
        // auto clientConnectionCreatedCallback1 = std::bind(&CI2pdNode::HandleClientConnectionCreated, node_shared, std::placeholders::_1);
        auto clientConnectionCreatedCallback = std::bind(&CI2pdNode::HandleClientConnectionCreated, pnode, std::placeholders::_1);
        auto clientConnectionCreatedCallback2 = std::bind(&CI2pdNode::HandleClientConnectionCreated, pnode, _1);
        tunnel->SetConnectionCreatedCallback(clientConnectionCreatedCallback);
        tunnel->SetConnectedCallback(std::bind(&CI2pdNode::HandleClientConnected, pnode, _1));
        tunnel->SetReceivedCallback(std::bind(&CI2pdNode::HandleClientReceived, pnode, _1, _2, _3));

        // this moved from ConnectNode here so we can have callbacks hooked to node properly
        auto clientEndpoint = tunnel->GetLocalEndpoint();
        // AcceptClientTunnel(clientEndpoint, pnode->i2pTunnel);
        AcceptClientTunnel(clientEndpoint, tunnel);

        pnode->nTimeConnected = GetTime();
        if (obfuScationMaster) pnode->fObfuScationMaster = true;

        return pnode;
    } else {
        // If connecting to the node failed, and failure is not caused by a problem connecting to
        // the proxy, mark this as an attempt.
        addrman.Attempt(addrConnect);
    }

    return NULL;
}

void CI2pdNode::CloseTunnelDisconnect()
{
    fDisconnect = true;
    if (i2pTunnel != nullptr) { //INVALID_SOCKET) {
        LogPrintf("net: disconnecting peer=%d\n", id);
        CloseTunnel(i2pTunnel, fInbound);
    }

    // in case this fails, we'll empty the recv buffer when the CI2pdNode is deleted
    TRY_LOCK(cs_vRecvMsg, lockRecv);
    if (lockRecv)
        vRecvMsg.clear();
}

bool CI2pdNode::DisconnectOldProtocol(int nVersionRequired, string strLastCommand)
{
    fDisconnect = false;
    if (nVersion < nVersionRequired) {
        LogPrintf("%s : peer=%d using obsolete version %i; disconnecting\n", __func__, id, nVersion);
        PushMessage("reject", strLastCommand, REJECT_OBSOLETE, strprintf("Version must be %d or greater", ActiveProtocol()));
        fDisconnect = true;
    }

    return fDisconnect;
}

void CI2pdNode::PushVersion()
{
    int nBestHeight = g_signals.GetHeight().get_value_or(0);

    /// when NTP implemented, change to just nTime = GetAdjustedTime()
    int64_t nTime = (fInbound ? GetAdjustedTime() : GetTime());

    // I2PDK: IsProxy is always going to be false as it doesn't make sense, check that out.
    CI2PAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CI2PAddress(CDestination("0.0.0.0", 0)));
    CI2PAddress addrMe = GetLocalAddress(&addr);
    GetRandBytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));
    if (fLogIPs)
        LogPrintf("net: send version message: version %d, blocks=%d, us=%s, them=%s, peer=%d\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), addrYou.ToString(), id);
    else
        LogPrintf("net: send version message: version %d, blocks=%d, us=%s, peer=%d\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), id);
    PushMessage("version", PROTOCOL_VERSION, nLocalServices, nTime, addrYou, addrMe,
        nLocalHostNonce, FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<string>()), nBestHeight, true);
}


banmap_t CI2pdNode::setBanned;
CCriticalSection CI2pdNode::cs_setBanned;
bool CI2pdNode::setBannedIsDirty;

void CI2pdNode::ClearBanned()
{
    {
        LOCK(cs_setBanned);
        setBanned.clear();
        setBannedIsDirty = true;
    }
    DumpBanlist(); // store banlist to Disk
    uiInterface.BannedListChanged();
}

bool CI2pdNode::IsBanned(CI2pUrl ip)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        for (banmap_t::iterator it = setBanned.begin(); it != setBanned.end(); it++)
        {
            CI2pSubNet subNet = (*it).first;
            CBanEntry banEntry = (*it).second;

            if(subNet.Match(ip) && GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }
    return fResult;
}

// I2PDK: this doesn't make much sense w/ the i2p-style-addresses, but maybe we should keep it for now to
// see how it's going to play out, if any other uses and if we could adapt this to .i2p-s somehow (routers?)
bool CI2pdNode::IsBanned(CI2pSubNet subnet)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        banmap_t::iterator i = setBanned.find(subnet);
        if (i != setBanned.end()) {
            CBanEntry banEntry = (*i).second;
            if (GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }
    return fResult;
}

void CI2pdNode::Ban(const CI2pUrl& addr, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch)
{
    CI2pSubNet subNet(addr);
    Ban(subNet, banReason, bantimeoffset, sinceUnixEpoch);
}

void CI2pdNode::Ban(const CI2pSubNet& subNet, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch)
{
    CBanEntry banEntry(GetTime());
    banEntry.banReason = banReason;
    if (bantimeoffset <= 0)
    {
        bantimeoffset = GetArg("-bantime", 60*60*24); // Default 24-hour ban
        sinceUnixEpoch = false;
    }
    banEntry.nBanUntil = (sinceUnixEpoch ? 0 : GetTime() )+bantimeoffset;

    {
        LOCK(cs_setBanned);
        if (setBanned[subNet].nBanUntil < banEntry.nBanUntil) {
            setBanned[subNet] = banEntry;
            setBannedIsDirty = true;
        }
        else
            return;
    }
    uiInterface.BannedListChanged();
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CI2pdNode* pnode, vNodes) {
            if (subNet.Match((CI2pUrl)pnode->addr))
                pnode->fDisconnect = true;
        }
    }
    if(banReason == BanReasonManuallyAdded)
        DumpBanlist(); //store banlist to disk immediately if user requested ban
}

bool CI2pdNode::Unban(const CI2pUrl &addr)
{
    CI2pSubNet subNet(addr);
    return Unban(subNet);
}

bool CI2pdNode::Unban(const CI2pSubNet &subNet)
{
    {
        LOCK(cs_setBanned);
        if (!setBanned.erase(subNet))
            return false;
        setBannedIsDirty = true;
    }
    uiInterface.BannedListChanged();
    DumpBanlist(); //store banlist to disk immediately
    return true;
}

void CI2pdNode::GetBanned(banmap_t &banMap)
{
    LOCK(cs_setBanned);
    banMap = setBanned; //create a thread safe copy
}

void CI2pdNode::SetBanned(const banmap_t &banMap)
{
    LOCK(cs_setBanned);
    setBanned = banMap;
    setBannedIsDirty = true;
}

void CI2pdNode::SweepBanned()
{
    int64_t now = GetTime();

    bool notifyUI = false;
    {
        LOCK(cs_setBanned);
        banmap_t::iterator it = setBanned.begin();
        while(it != setBanned.end())
        {
            CI2pSubNet subNet = (*it).first;
            CBanEntry banEntry = (*it).second;
            if(now > banEntry.nBanUntil)
            {
                setBanned.erase(it++);
                setBannedIsDirty = true;
                notifyUI = true;
                LogPrintf("net: %s: Removed banned node ip/subnet from banlist.dat: %s\n", __func__, subNet.ToString());
            }
            else
                ++it;
        }
    }
    // update UI
    if(notifyUI) {
        uiInterface.BannedListChanged();
    }
}

bool CI2pdNode::BannedSetIsDirty()
{
    LOCK(cs_setBanned);
    return setBannedIsDirty;
}

void CI2pdNode::SetBannedSetDirty(bool dirty)
{
    LOCK(cs_setBanned); //reuse setBanned lock for the isDirty flag
    setBannedIsDirty = dirty;
}


std::vector<CI2pSubNet> CI2pdNode::vWhitelistedRange;
CCriticalSection CI2pdNode::cs_vWhitelistedRange;

bool CI2pdNode::IsWhitelistedRange(const CI2pUrl& addr)
{
    LOCK(cs_vWhitelistedRange);
    BOOST_FOREACH (const CI2pSubNet& subnet, vWhitelistedRange) {
        if (subnet.Match(addr))
            return true;
    }
    return false;
}

void CI2pdNode::AddWhitelistedRange(const CI2pSubNet& subnet)
{
    LOCK(cs_vWhitelistedRange);
    vWhitelistedRange.push_back(subnet);
}

#undef X
#define X(name) stats.name = name
void CI2pdNode::copyStats(CNodeStats& stats)
{
    stats.nodeid = this->GetId();
    X(nServices);
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(nTimeOffset);
    X(addrName);
    X(nVersion);
    X(cleanSubVer);
    X(fInbound);
    X(nStartingHeight);
    X(nSendBytes);
    X(nRecvBytes);
    X(fWhitelisted);

    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node was still responsive,
    // since pingtime does not update until the ping is complete, which might take a while.
    // So, if a ping is taking an unusually long time in flight,
    // the caller can immediately detect that this is happening.
    int64_t nPingUsecWait = 0;
    if ((0 != nPingNonceSent) && (0 != nPingUsecStart)) {
        nPingUsecWait = GetTimeMicros() - nPingUsecStart;
    }

    // Raw ping time is in microseconds, but show it to user as whole seconds (COLX users should be well used to small numbers with many decimal places by now :)
    stats.dPingTime = (((double)nPingUsecTime) / 1e6);
    stats.dPingWait = (((double)nPingUsecWait) / 1e6);

    // Leave string empty if addrLocal invalid (not filled in yet)
    stats.addrLocal = addrLocal.IsValid() ? addrLocal.ToString() : "";
}
#undef X

// requires LOCK(cs_vRecvMsg)
bool CI2pdNode::ReceiveMsgBytes(const char* pch, unsigned int nBytes)
{
    while (nBytes > 0) {
        // get current incomplete message, or create a new one
        if (vRecvMsg.empty() ||
            vRecvMsg.back().complete())
            vRecvMsg.push_back(CNetMessage(SER_NETWORK, nRecvVersion));

        CNetMessage& msg = vRecvMsg.back();

        // absorb network data
        int handled;
        if (!msg.in_data)
            handled = msg.readHeader(pch, nBytes);
        else
            handled = msg.readData(pch, nBytes);

        if (handled < 0)
            return false;

        if (msg.in_data && msg.hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
            LogPrintf("net: Oversized message from peer=%i, disconnecting", GetId());
            return false;
        }

        pch += handled;
        nBytes -= handled;

        if (msg.complete()) {
            msg.nTime = GetTimeMicros();
            messageHandlerCondition.notify_one();
        }
    }

    return true;
}

int CNetMessage::readHeader(const char* pch, unsigned int nBytes)
{
    // copy data to temporary parsing buffer
    unsigned int nRemaining = 24 - nHdrPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    memcpy(&hdrbuf[nHdrPos], pch, nCopy);
    nHdrPos += nCopy;

    // if header incomplete, exit
    if (nHdrPos < 24)
        return nCopy;

    // deserialize to CMessageHeader
    try {
        hdrbuf >> hdr;
    } catch (const std::exception&) {
        return -1;
    }

    // reject messages larger than MAX_SIZE
    if (hdr.nMessageSize > MAX_SIZE)
        return -1;

    // switch state to reading message data
    in_data = true;

    return nCopy;
}

int CNetMessage::readData(const char* pch, unsigned int nBytes)
{
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    if (vRecv.size() < nDataPos + nCopy) {
        // Allocate up to 256 KiB ahead, but never more than the total message size.
        vRecv.resize(std::min(hdr.nMessageSize, nDataPos + nCopy + 256 * 1024));
    }

    memcpy(&vRecv[nDataPos], pch, nCopy);
    nDataPos += nCopy;

    return nCopy;
}


// requires LOCK(cs_vSend)
void TunnelSendData(CI2pdNode* pnode)
{
    using namespace std::placeholders;    // adds visibility of _1, _2, _3,...

    std::deque<CSerializeData>::iterator it = pnode->vSendMsg.begin();

    static int64_t nLastLogTime = 0; //GetTime();

    while (it != pnode->vSendMsg.end()) {
        const CSerializeData& data = *it;
        assert(data.size() > pnode->nSendOffset);

        // Just do a regular Send (AsyncSend underneath), there's no difference in between Send or AsyncSend, both are
        // async and just add to the buffer (to be processed later in the back). But since we're doing this in one go
        // (from the main processing thread) and under locks (i.e. no one else is able to send while we're not done)
        // it doesn't really matter, important thing is that stuff is added to the buffer in the right order, which is
        // enforced w all this. Anyone sending after we're done (unlocked) doesn't matter as it'll go at the end.
        //pnode->i2pTunnel
        //auto clientTunnel = std::static_pointer_cast<I2PPureClientTunnel>(pnode->i2pTunnel)
        //auto node_shared = std::make_shared<CI2pdNode>(pnode);
        //if (!pnode->_sendCallback || !pnode->_sendMoreCallback) {
        if (!pnode->_connection) {
            // MilliSleep(500);
            int64_t nTime = GetTime();
            if (nTime - nLastLogTime > 60) {
                nLastLogTime = nTime;
                LogPrintf("net: tunnel connection not set yet, wait a bit... %s\n", pnode->addr.ToString());
            }
            return;
            // continue;
        }

        std::string identity = pnode->addr.ToString();
        if (pnode->fInbound)
            identity = pnode->_connection->GetRemoteIdentity();

        LogPrintf("net: tunnel sending some data... %s\n", identity);

        //auto readyCallback = std::bind(&CI2pdNode::HandleReadyToSend, pnode);
        //auto errorCallback = std::bind(&CI2pdNode::HandleErrorSend, pnode, _1);
        //_sendMoreCallback("", nullptr, errorCallback);
        //const uint8_t* msg = (const uint8_t*)&data[pnode->nSendOffset];
        //size_t len = data.size() - pnode->nSendOffset;
        //pnode->_sendMoreCallback(&data[pnode->nSendOffset], data.size() - pnode->nSendOffset);
        //pnode->_sendMoreCallback(msg, len);

        // std::string message(&data[pnode->nSendOffset], data.size() - pnode->nSendOffset);
        std::string message(&data[pnode->nSendOffset], std::min((int)(data.size() - pnode->nSendOffset), 30));
        LogPrintf("TunnelSendData: sending message...'%s' (%d) \n", message, data.size() - pnode->nSendOffset);

        // I2PDK: there's an issue here, we're (seems) only made to serve one client at the time?
        // not sure how this was translated from what was sockets, or maybe not? 
        pnode->_connection->HandleSendReadyRawSigned(
            &data[pnode->nSendOffset], 
            data.size() - pnode->nSendOffset, 
            (ReadyToSendCallback)nullptr, 
            (ErrorSendCallback)pnode->_errorCallback);

        // we have no way of doing what 'send' does, our send is always async, and no actual bytes returned.
        // on the other side actual send and errors if any are returned async (in a callback we registered), i.e. node
        // will get notified of it but it's going to be outside of this scope/thread loop and we won't be able to react.
        // If things are ok it doesn't matter, bytes will be ok etc., if it fails we'll probably need to kill it anyway.
        int nBytes = data.size() - pnode->nSendOffset;

        // TODO: send message via tunnel I2PPure*Tunnel
        // also adjust the send/receive buffer limits to match (this node buffering and the i2pd limits), otherwise we may get out of sync
        //int nBytes = send(pnode->i2pTunnel, &data[pnode->nSendOffset], data.size() - pnode->nSendOffset, MSG_NOSIGNAL | MSG_DONTWAIT);

        if (nBytes > 0) {
            pnode->nLastSend = GetTime();
            pnode->nSendBytes += nBytes;
            pnode->nSendOffset += nBytes;
            pnode->RecordBytesSent(nBytes);
            if (pnode->nSendOffset == data.size()) {
                pnode->nSendOffset = 0;
                pnode->nSendSize -= data.size();
                it++;
            } else {
                // could not send full message; stop sending more
                break;
            }
        } else {
            if (nBytes < 0) {
                // error (won't get here, report this from the HandleErrorSend
                LogPrintf("net: tunnel send error %s\n", identity);
                pnode->CloseTunnelDisconnect();
                //int nErr = WSAGetLastError();
                //if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS) {
                //    LogPrintf("tunnel send error %s\n", NetworkErrorString(nErr));
                //    pnode->CloseTunnelDisconnect();
                //}
            }
            // couldn't send anything at all
            break;
        }
    }

    if (it == pnode->vSendMsg.end()) {
        assert(pnode->nSendOffset == 0);
        assert(pnode->nSendSize == 0);
    }
    pnode->vSendMsg.erase(pnode->vSendMsg.begin(), it);
}

static list<CI2pdNode*> vNodesDisconnected;

// I2PDK: this is all about nodes, so it is tunnels, not sockets 
// (is there anything that goes over nodes that won't be serviced over i2p?)
void ThreadTunnelHandler()
{
    unsigned int nPrevNodeCount = 0;
    while (true) {
        //
        // Disconnect nodes
        //
        {
            LOCK(cs_vNodes);
            // Disconnect unused nodes
            vector<CI2pdNode*> vNodesCopy = vNodes;
            BOOST_FOREACH (CI2pdNode* pnode, vNodesCopy) {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecvMsg.empty() && pnode->nSendSize == 0 && pnode->ssSend.empty())) {
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();

                    // close tunnel and cleanup
                    pnode->CloseTunnelDisconnect();

                    // hold in disconnected pool until all refs are released
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }
        }
        {
            // Delete disconnected nodes
            list<CI2pdNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            BOOST_FOREACH (CI2pdNode* pnode, vNodesDisconnectedCopy) {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0) {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_vSend, lockSend);
                        if (lockSend) {
                            TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                            if (lockRecv) {
                                TRY_LOCK(pnode->cs_inventory, lockInv);
                                if (lockInv)
                                    fDelete = true;
                            }
                        }
                    }
                    if (fDelete) {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        size_t vNodesSize;
        {
            LOCK(cs_vNodes);
            vNodesSize = vNodes.size();
        }
        if(vNodesSize != nPrevNodeCount) {
            nPrevNodeCount = vNodesSize;
            uiInterface.NotifyNumConnectionsChanged(nPrevNodeCount);
        }

        //
        // Find which tunnels have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        //fd_set fdsetRecv;
        //fd_set fdsetSend;
        //fd_set fdsetError;
        //FD_ZERO(&fdsetRecv);
        //FD_ZERO(&fdsetSend);
        //FD_ZERO(&fdsetError);
        // I2PDK: max socket? how to map that to our i2p tunnels?
        //SOCKET hSocketMax = 0;
        bool have_fds = false;
        bool  recv_set[FD_SETSIZE];
        bool  send_set[FD_SETSIZE];
        // this isn't good enough as vNodes are not locked the entire time, so index could theoretically change?
        // FD_SETSIZE >> nMaxConnections

        BOOST_FOREACH (const ListenTunnel& hListenTunnel, vhListenTunnel) {
            // I2PDK: not sure what to do w this
            //FD_SET(hListenTunnel.tunnel, &fdsetRecv);
            //hSocketMax = max(hSocketMax, hListenTunnel.tunnel);
            //have_fds = true;
        }

        {
            LOCK(cs_vNodes);
            int iNode = 0;
            BOOST_FOREACH (CI2pdNode* pnode, vNodes) {
                if (pnode->i2pTunnel == nullptr) //INVALID_SOCKET)
                    continue;
                // I2PDK: not sure what to do w this
                //FD_SET(pnode->i2pTunnel, &fdsetError);
                //hSocketMax = max(hSocketMax, pnode->i2pTunnel);
                //have_fds = true;

                // Implement the following logic:
                // * If there is data to send, select() for sending data. As this only
                //   happens when optimistic write failed, we choose to first drain the
                //   write buffer in this case before receiving more. This avoids
                //   needlessly queueing received data, if the remote peer is not themselves
                //   receiving data. This means properly utilizing TCP flow control signalling.
                // * Otherwise, if there is no (complete) message in the receive buffer,
                //   or there is space left in the buffer, select() for receiving data.
                // * (if neither of the above applies, there is certainly one message
                //   in the receiver buffer ready to be processed).
                // Together, that means that at least one of the following is always possible,
                // so we don't deadlock:
                // * We send some data.
                // * We wait for data to be received (and disconnect after timeout).
                // * We process a message in the buffer (message handler thread).

                // I2PDK: not sure how to emulate this, FD_SET will specify preference of send over receive (as per the
                // above comments). Probably best is to store fdset equivalents (flags) within a vector on the stack
                // and consult those values when doing send/receive beneath. Or just repeat the conditions here, before
                // receiving.
                {
                    TRY_LOCK(pnode->cs_vSend, lockSend);
                    if (lockSend && !pnode->vSendMsg.empty()) {
                        send_set[iNode] = true;
                        continue;
                        // I2PDK: not sure what to do w this
                        //FD_SET(pnode->i2pTunnel, &fdsetSend);
                    }
                }
                {
                    TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                    if (lockRecv && (pnode->vRecvMsg.empty() || !pnode->vRecvMsg.front().complete() ||
                        pnode->GetTotalRecvSize() <= ReceiveFloodSize())) {
                        recv_set[iNode] = true;
                        //FD_SET(pnode->i2pTunnel, &fdsetRecv);
                    }
                }
                ++iNode;
                if (!(iNode < FD_SETSIZE)) {
                    LogPrintf("net: !(iNode < FD_SETSIZE) %s\n", pnode->addr.ToString());
                    //exit(-1);
                }
            }
        }

        // I2PDK: not sure what to do w this
        //int nSelect = select(have_fds ? hSocketMax + 1 : 0,
        //    &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        boost::this_thread::interruption_point();

        // I2PDK: we have almost no error handling the I2PD way, where and how to handle it?
        //if (nSelect == SOCKET_ERROR) {
        //    if (have_fds) {
        //        int nErr = WSAGetLastError();
        //        LogPrintf("socket select error %s\n", NetworkErrorString(nErr));
        //        for (unsigned int i = 0; i <= hSocketMax; i++)
        //            FD_SET(i, &fdsetRecv);
        //    }
        //    FD_ZERO(&fdsetSend);
        //    FD_ZERO(&fdsetError);
        //    MilliSleep(timeout.tv_usec / 1000);
        //}

        //
        // Accept new connections
        //
        BOOST_FOREACH (const ListenTunnel& hListenTunnel, vhListenTunnel) {
            // I2PDK: if it has something to receive, new streams waiting to connect...
            // this is now done in the HandleServerStreamAccepted, a callback called from the tunnel when something happens.
            // i.e. there is no need for this any more - unless there're issues (thread wise if it isn't delegated),
            // in which case we'd want to mark the tunnel (tunnel entry in vhListenTunnel) and do it here, also may
            // be better for performance (I'm not sure if that was the reason doing it this way or was just due to the nature of the sockets.
        }

        // increment ref count before doing things, i.e. we're processing it (and to be release when all is done at the end here)
        // Service each socket
        vector<CI2pdNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH (CI2pdNode* pnode, vNodesCopy)
                pnode->AddRef();
        }

        int iNode = 0;
        BOOST_FOREACH (CI2pdNode* pnode, vNodesCopy) {
            boost::this_thread::interruption_point();

            //
            // Receive
            //
            if (pnode->i2pTunnel == nullptr) //INVALID_SOCKET)
                continue;

            // TODO: this is the tunnel received messages processing...
            // also, having things done here within the thread helps the locks (to be in one place), so we may need to gather it and do it here as well.

            // I2PDK: this has to be rewritten for i2pd, none of it is useful as is
            // always try and do the receive as we're no longer using sockets/select/FD_SET
            //if (FD_ISSET(pnode->i2pTunnel, &fdsetRecv) || FD_ISSET(pnode->i2pTunnel, &fdsetError)) 
            //if (recv_set[iNode])
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv) {
                    {
                        // I2PDK: this is the tunnel/i2pd equivalent of the sockets receive op. It works differently,
                        // it's based on the StreamReceive() 'loop' which is called/started from within the I2PConnect,
                        // and it uses m_Stream->AsyncReceive to wait for any new data, which calls HandleStreamReceive
                        // callback, which then loops back to the StreamReceive and so on. So it mostly waits in the 
                        // AsyncReceive, then it 'writes' (calls our received callback right into a node here), and
                        // loops back and waits for another batch of data.

                        // should _messageReceived be locked? not for the moment, cs_vRecvMsg locks vector vRecvMsg
                        // std::string message = pnode->PopMessageReceived();
                        // if (message.empty()) continue;
                        // const char* pchBuf = message.data();
                        // size_t nBytes = message.length();

                        static std::unique_ptr<uint8_t[]> buffer = 
                            std::unique_ptr<uint8_t[]>(new uint8_t[I2P_TUNNEL_CONNECTION_BUFFER_SIZE]);

                        size_t nBytes = pnode->PopMessageReceived(buffer);
                        if (nBytes <= 0) continue;
                        const char* pchBuf = (const char*)buffer.get();

                        //// typical socket buffer is 8K-64K
                        //char pchBuf[0x10000];
                        //int nBytes = recv(pnode->i2pTunnel, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                        if (nBytes > 0) {
                            if (!pnode->ReceiveMsgBytes(pchBuf, nBytes))
                                pnode->CloseTunnelDisconnect();
                            pnode->nLastRecv = GetTime();
                            pnode->nRecvBytes += nBytes;
                            pnode->RecordBytesRecv(nBytes);
                        } else if (nBytes == 0) {
                            // socket closed gracefully
                            if (!pnode->fDisconnect)
                                LogPrintf("net: tunnel closed\n");
                            pnode->CloseTunnelDisconnect();
                        } else if (nBytes < 0) {
                            // error (won't get here)
                            LogPrintf("net: tunnel recv error %s\n", pnode->addr.ToString());
                            pnode->CloseTunnelDisconnect();
                            //int nErr = WSAGetLastError();
                            //if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS) {
                            //    if (!pnode->fDisconnect)
                            //        LogPrintf("tunnel recv error %s\n", NetworkErrorString(nErr));
                            //    pnode->CloseTunnelDisconnect();
                            //}
                        }
                    }
                }
            }

            //
            // Send
            //
            if (pnode->i2pTunnel == nullptr) //INVALID_SOCKET)
                continue;

            // this is the new tunnel send, it looks pretty much the same, we just don't use socket FD_SET/select stuff.
            // i.e. always check and try to send regardless (we have less optimization this way but we're shielded 
            // better and most of the functionality is encapsulated within the i2pd classed, streaming etc.
            //if (FD_ISSET(pnode->i2pTunnel, &fdsetSend))
            //if (send_set[iNode])
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                    TunnelSendData(pnode);
            }

            //
            // Inactivity checking
            //
            int64_t nTime = GetTime();
            // I2PDK: this interval was too small for I2P network, in 60 secs we're hardly ever connected, not to mention exchanging messages
            // check if this makes sense, or at least separate these if-s not like this.
            if (nTime - pnode->nTimeConnected > TIMEOUT_INTERVAL) { //60) {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0) {
                    LogPrintf("net: tunnel no message in first 60 seconds, %d %d from %d\n", pnode->nLastRecv != 0, pnode->nLastSend != 0, pnode->id);
                    pnode->fDisconnect = true;
                } else if (nTime - pnode->nLastSend > TIMEOUT_INTERVAL) {
                    LogPrintf("tunnel sending timeout: %is\n", nTime - pnode->nLastSend);
                    pnode->fDisconnect = true;
                } else if (nTime - pnode->nLastRecv > (pnode->nVersion > BIP0031_VERSION ? TIMEOUT_INTERVAL : 90 * 60)) {
                    LogPrintf("tunnel receive timeout: %is\n", nTime - pnode->nLastRecv);
                    pnode->fDisconnect = true;
                } else if (pnode->nPingNonceSent && pnode->nPingUsecStart + TIMEOUT_INTERVAL * 1000000 < GetTimeMicros()) {
                    // TODO: check these last 2 for i2p relevance (and maybe we're not processing that and it'd fail here)
                    LogPrintf("ping timeout: %fs\n", 0.000001 * (GetTimeMicros() - pnode->nPingUsecStart));
                    pnode->fDisconnect = true;
                }
            }
            ++iNode;
        }

        // decrement the ref count for all nodes
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH (CI2pdNode* pnode, vNodesCopy)
                pnode->Release();
        }
    }
}


#ifdef USE_UPNP
void ThreadMapPort()
{
    std::string port = strprintf("%u", GetListenPort());
    const char* multicastif = 0;
    const char* minissdpdpath = 0;
    struct UPNPDev* devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#elif MINIUPNPC_API_VERSION < 14
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
#else
    /* miniupnpc 1.9.20150730 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, 2, &error);
#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    if (r == 1) {
        if (fDiscover) {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if (r != UPNPCOMMAND_SUCCESS)
                LogPrintf("UPnP: GetExternalIPAddress() returned %d\n", r);
            else {
                if (externalIPAddress[0]) {
                    LogPrintf("UPnP: ExternalIPAddress = %s\n", externalIPAddress);
                    AddLocal(CI2pUrl(externalIPAddress), LOCAL_UPNP);
                } else
                    LogPrintf("UPnP: GetExternalIPAddress failed.\n");
            }
        }

        string strDesc = "COLX " + FormatFullVersion();

        try {
            while (true) {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if (r != UPNPCOMMAND_SUCCESS)
                    LogPrintf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                        port, port, lanaddr, r, strupnperror(r));
                else
                    LogPrintf("UPnP Port Mapping successful.\n");
                ;

                MilliSleep(20 * 60 * 1000); // Refresh every 20 minutes
            }
        } catch (boost::thread_interrupted) {
            r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
            LogPrintf("UPNP_DeletePortMapping() returned : %d\n", r);
            freeUPNPDevlist(devlist);
            devlist = 0;
            FreeUPNPUrls(&urls);
            throw;
        }
    } else {
        LogPrintf("No valid UPnP IGDs found\n");
        freeUPNPDevlist(devlist);
        devlist = 0;
        if (r != 0)
            FreeUPNPUrls(&urls);
    }
}

void MapPort(bool fUseUPnP)
{
    static boost::thread* upnp_thread = NULL;

    if (fUseUPnP) {
        if (upnp_thread) {
            upnp_thread->interrupt();
            upnp_thread->join();
            delete upnp_thread;
        }
        upnp_thread = new boost::thread(boost::bind(&TraceThread<void (*)()>, "upnp", &ThreadMapPort));
    } else if (upnp_thread) {
        upnp_thread->interrupt();
        upnp_thread->join();
        delete upnp_thread;
        upnp_thread = NULL;
    }
}

#else
void MapPort(bool)
{
    // Intentionally left blank.
}
#endif


// I2PDK: is this going over i2p or just regular sockets? seeding?
// this is very iffy, and LookupHost and so on, will not work like this, needs reworking
void ThreadDNSAddressSeed()
{
    // goal: only query DNS seeds if address need is acute
    if ((addrman.size() > 0) &&
        (!GetBoolArg("-forcednsseed", false))) {
        MilliSleep(11 * 1000);

        LOCK(cs_vNodes);
        if (vNodes.size() >= 2) {
            LogPrintf("P2P peers available. Skipped DNS seeding.\n");
            return;
        }
    }

    const vector<CDNSSeedData>& vSeeds = Params().DNSSeeds();
    int found = 0;

    LogPrintf("Loading addresses from DNS seeds (could take a while)\n");

    BOOST_FOREACH (const CDNSSeedData& seed, vSeeds) {
        if (HaveNameProxy()) {
            AddOneShot(seed.host);
        } else {
            vector<CI2pUrl> vIPs;
            vector<CI2PAddress> vAdd;
            // I2PDK: is this  normal address or i2p destination?
            if (LookupHost(seed.host.c_str(), vIPs)) {
                BOOST_FOREACH (CI2pUrl& ip, vIPs) {
                    int nOneDay = 24 * 3600;
                    CI2PAddress addr = CI2PAddress(CDestination(ip, Params().GetDefaultPort()));
                    addr.nTime = GetTime() - 3 * nOneDay - GetRand(4 * nOneDay); // use a random age between 3 and 7 days old
                    vAdd.push_back(addr);
                    found++;
                }
            }
            addrman.Add(vAdd, CI2pUrl(seed.name, true));
        }
    }

    LogPrintf("%d addresses found from DNS seeds\n", found);
}


static bool DumpAddresses()
{
    static bool bFirstRun = true;
    if (bFirstRun) {
        MilliSleep(60 * 1000); // wait 1 min, give wallet time to start
        bFirstRun = false;
    }

    int64_t nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    LogPrintf("net: Flushed %d addresses to peers.dat %dms\n", addrman.size(), GetTimeMillis() - nStart);

    return true; // never exit thread
}

static void ProcessOneShot()
{
    string strDest;
    {
        LOCK(cs_vOneShots);
        if (vOneShots.empty())
            return;
        strDest = vOneShots.front();
        vOneShots.pop_front();
    }
    CI2PAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        if (!OpenNetworkConnection(addr, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

void ThreadOpenConnections()
{
    // Connect to specific addresses
    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0) {
        for (int64_t nLoop = 0;; nLoop++) {
            ProcessOneShot();
            BOOST_FOREACH (string strAddr, mapMultiArgs["-connect"]) {
                CI2PAddress addr;
                OpenNetworkConnection(addr, NULL, strAddr.c_str());
                for (int i = 0; i < 10 && i < nLoop; i++) {
                    MilliSleep(500);
                }
            }
            MilliSleep(500);
        }
    }

    // Initiate network connections
    int64_t nStart = GetTime();
    while (true) {
        ProcessOneShot();

        MilliSleep(500);

        CSemaphoreGrant grant(*semOutbound);
        boost::this_thread::interruption_point();

        // Add seed nodes if DNS seeds are all down (an infrastructure attack?).
        if (addrman.size() == 0 && (GetTime() - nStart > 60)) {
            static bool done = false;
            if (!done) {
                LogPrintf("Adding fixed seed nodes as DNS doesn't seem to be available.\n");
                addrman.Add(Params().FixedSeeds(), CI2pUrl("127.0.0.1"));
                done = true;
            }
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CI2PAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        int nOutbound = 0;
        set<vector<unsigned char> > setConnected;
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH (CI2pdNode* pnode, vNodes) {
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        int64_t nANow = GetAdjustedTime();

        int nTries = 0;
        while (true) {
            CI2PAddress addr = addrman.Select();
            // I2PDK: in case there's nothing or random buckets algo is stalling (which happens on near empty list)
            // we'd return an empty address, which is not valid, bail out if so.
            if (!addr.IsValid())
                break;

            // I2PDK: not sure about this or how much? due to the random looping forever issue
            MilliSleep(50);

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()) || IsLocal(addr))
                break;

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;
            if (nTries > 100)
                break;

            if (IsLimited(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (Params().NetworkID() == CBaseChainParams::MAIN && addr.GetPort() != Params().GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect, &grant);
    }
}

void ThreadOpenAddedConnections()
{
    {
        LOCK(cs_vAddedNodes);
        vAddedNodes = mapMultiArgs["-addnode"];
    }

    if (HaveNameProxy()) {
        while (true) {
            list<string> lAddresses(0);
            {
                LOCK(cs_vAddedNodes);
                BOOST_FOREACH (string& strAddNode, vAddedNodes)
                    lAddresses.push_back(strAddNode);
            }
            BOOST_FOREACH (string& strAddNode, lAddresses) {
                CI2PAddress addr;
                CSemaphoreGrant grant(*semOutbound);
                OpenNetworkConnection(addr, &grant, strAddNode.c_str());
                MilliSleep(500);
            }
            MilliSleep(120000); // Retry every 2 minutes
        }
    }

    for (unsigned int i = 0; true; i++) {
        list<string> lAddresses(0);
        {
            LOCK(cs_vAddedNodes);
            BOOST_FOREACH (string& strAddNode, vAddedNodes)
                lAddresses.push_back(strAddNode);
        }

        list<vector<CDestination> > lservAddressesToAdd(0);
        BOOST_FOREACH (string& strAddNode, lAddresses) {
            vector<CDestination> vservNode(0);
            if (Lookup(strAddNode.c_str(), vservNode, Params().GetDefaultPort(), fNameLookup, 0)) {
                lservAddressesToAdd.push_back(vservNode);
                {
                    LOCK(cs_setservAddNodeAddresses);
                    BOOST_FOREACH (CDestination& serv, vservNode)
                        setservAddNodeAddresses.insert(serv);
                }
            }
        }
        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fNameLookup)
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH (CI2pdNode* pnode, vNodes)
                for (list<vector<CDestination> >::iterator it = lservAddressesToAdd.begin(); it != lservAddressesToAdd.end(); it++)
                    BOOST_FOREACH (CDestination& addrNode, *(it))
                        if (pnode->addr == addrNode) {
                            it = lservAddressesToAdd.erase(it);
                            it--;
                            break;
                        }
        }
        BOOST_FOREACH (vector<CDestination>& vserv, lservAddressesToAdd) {
            CSemaphoreGrant grant(*semOutbound);
            OpenNetworkConnection(CI2PAddress(vserv[i % vserv.size()]), &grant);
            MilliSleep(500);
        }
        MilliSleep(120000); // Retry every 2 minutes
    }
}

// if successful, this moves the passed grant to the constructed node
bool OpenNetworkConnection(const CI2PAddress& addrConnect, CSemaphoreGrant* grantOutbound, const char* pszDest, bool fOneShot)
{
    //
    // Initiate outbound network connection
    //
    boost::this_thread::interruption_point();
    if (!pszDest) {
        if (IsLocal(addrConnect) ||
            FindNode((CI2pUrl)addrConnect) || CI2pdNode::IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort()))
            return false;
    } else if (FindNode(pszDest))
        return false;

    // I2PDK: this is a precondition to even consider connecting to i2p nodes, we need to be
    // up and running and 'on the network' with inbound and outbound channels, floodfills etc.
    // Only once that is available we can try going after lease-sets, and that's a precursor
    // for any connection.
    auto localDest = i2p::client::context.GetSharedLocalDestination();
    if (!localDest->AreTunnelsReady ()) {
        // this will loop anyway (every 2 mins?) so just bail out and try later on.
        return false;
    }

    CI2pdNode* pnode = ConnectNode(addrConnect, pszDest);
    boost::this_thread::interruption_point();

    if (!pnode)
        return false;
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    pnode->fNetworkNode = true;
    if (fOneShot)
        pnode->fOneShot = true;

    return true;
}


void ThreadMessageHandler()
{
    boost::mutex condition_mutex;
    boost::unique_lock<boost::mutex> lock(condition_mutex);

    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
    while (true) {
        vector<CI2pdNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH (CI2pdNode* pnode, vNodesCopy) {
                pnode->AddRef();
            }
        }

        // Poll the connected nodes for messages
        CI2pdNode* pnodeTrickle = NULL;
        if (!vNodesCopy.empty())
            pnodeTrickle = vNodesCopy[GetRand(vNodesCopy.size())];

        bool fSleep = true;

        BOOST_FOREACH (CI2pdNode* pnode, vNodesCopy) {
            if (pnode->fDisconnect)
                continue;

            // Receive messages
            {
                // DLOCKSFIX: send is most often needed within receive, resulting in
                // deadlock warnings (cs_vSend at the end of chain, while we also have cs_vSend, cs_main)

                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv) {
                    if (!g_signals.ProcessMessages(pnode))
                        pnode->CloseTunnelDisconnect();

                    if (pnode->nSendSize < SendBufferSize()) {
                        if (!pnode->vRecvGetData.empty() || (!pnode->vRecvMsg.empty() && pnode->vRecvMsg[0].complete())) {
                            fSleep = false;
                        }
                    }
                }
            }
            boost::this_thread::interruption_point();

            // Send messages
            {
                // DLOCKSFIX: order of locks: 
                // cs_vRecvMsg, cs_main (main: ProcessMessage...), cs_vSend (PushMessage)
                // cs_vSend, cs_main (main: SendMessages) 
                // SendMessages acquires cs_main right after the call, just in wrong order
                // (cs_main should go first, cs_vSend is always called from inside
                TRY_LOCK(cs_main, lockMain);
                if (lockMain) {
                    // DLOCKSFIX: order of locks: cs_main, mempool.cs, ...
                    // I'm reluctantly doing this but almost always mempool goes along
                    TRY_LOCK(mempool.cs, lockMempool);
                    if (lockMempool) {
                        if (pnode->nVersion != 0)
                        {
                            // DLOCKSFIX: AddressRefreshBroadcast <= a signal of its own. The idea is to separate the locks, as it's isolated code. Just something we have to do from time to time, SendMessages was used as a trigger
                            TRY_LOCK(cs_vNodes, lockNodes);
                            if (lockNodes) {
                                g_signals.AddressRefreshBroadcast();
                            }
                        }
                        TRY_LOCK(pnode->cs_vSend, lockSend);
                        if (lockSend) {
                            g_signals.SendMessages(pnode, pnode == pnodeTrickle || pnode->fWhitelisted);
                        }
                    }
                }
            }
            boost::this_thread::interruption_point();
        }


        {
            LOCK(cs_vNodes);
            BOOST_FOREACH (CI2pdNode* pnode, vNodesCopy)
                pnode->Release();
        }

        if (fSleep)
            messageHandlerCondition.timed_wait(lock, boost::posix_time::microsec_clock::universal_time() + boost::posix_time::milliseconds(100));
    }
}

// ppcoin: stake minter thread
void static ThreadStakeMinter()
{
    boost::this_thread::interruption_point();
    LogPrintf("ThreadStakeMinter starting\n");
    CWallet* pwallet = pwalletMain;
    try {
        BitcoinMiner(pwallet, true);
        boost::this_thread::interruption_point();
    } catch (std::exception& e) {
        LogPrintf("ThreadStakeMinter() exception: %s\n", e.what());
    } catch (...) {
        LogPrintf("ThreadStakeMinter() error\n");
    }
    LogPrintf("ThreadStakeMinter exiting\n");
}

// I2PDK: one possible difference here (vs old code) is that this is called async from the tunnel,
// while previously was done from the main thread, iterating over vhListenSocket 
// & accepting if flags were set. We can fix that by keeping a vector of all streams accepted here,
// and offloading the processing from within the main thread (possibly best is to lookup the vhListenTunnel,
// we'd need a lookup per tunnel, and set a flag in there, then iterate over from the thread if flags set).
// But there're no locks or anything in the main thread that we dont' have here - still order, reentry etc.
// also cs_vNodes is called from here (full, not a try) so that could slow things down vs all in one thread.
// (sequentiallity could also be different and an issue), i.e. something to have in mind.
void static HandleServerStreamAccepted(
    std::shared_ptr<I2PPureServerTunnel> tunnel, 
    std::string identity,
    ServerConnectionCreatedCallback& connectionCreatedCallback,
    ServerClientConnectedCallback& connectedCallback,
    ReceivedCallback& receivedCallback, 
    const CDestination& addrBind, 
    bool fWhitelisted) //, const ListenTunnel& hListenTunnel)
{
    using namespace std::placeholders;    // adds visibility of _1, _2, _3,...

    CI2PAddress addr = CI2PAddress(addrBind);


    // at this point tunnel has already accepted a stream but has no connection, we create the node first...

    if (!tunnel) {
        LogPrintf("net: tunnel accept failed: %s\n", addrBind.ToString());
        return;
    }

    int nInbound = 0;
    bool whitelisted = fWhitelisted || CI2pdNode::IsWhitelistedRange(addrBind);
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH (CI2pdNode* pnode, vNodes)
            if (pnode->fInbound)
                nInbound++;
    }

    if (nInbound >= nMaxConnections - MAX_OUTBOUND_CONNECTIONS) {
        LogPrintf("net: connection from %s dropped (full)\n", addrBind.ToString());
        CloseTunnel(tunnel);
        return;
    }

    if (CI2pdNode::IsBanned(addrBind) && !whitelisted) {
        LogPrintf("net: connection from %s dropped (banned)\n", addrBind.ToString());
        CloseTunnel(tunnel);
        return;
    }

    // I2PDK: inbound node/socket, i.e. the 'server' in our terms, no target 'addrName'
    // socket is result of a 'accept' (not ConnectSocket...)
    // give it a name?

    // TODO: make our 'address' from CDestination 
    //CI2PAddress addr; 
    //addr = CDestination(*(const struct sockaddr_in*)paddr); // : CI2pUrl(addr.sin_addr), port(ntohs(addr.sin_port))

    // server node created here, addr doesn't make much sense as it's going to point back to us
    // as mentioned in the comment above (binding), we get one of these per each client connection
    // so that's fine but callbacks are not, they're set on the tunnel - which has many connections.
    CI2pdNode* pnode = new CI2pdNode(tunnel, addr, "", true);
    pnode->AddRef();
    pnode->fWhitelisted = whitelisted;

    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }

    connectionCreatedCallback = std::bind(&CI2pdNode::HandleServerConnectionCreated, pnode, _1, _2);
    connectedCallback = std::bind(&CI2pdNode::HandleServerClientConnected, pnode, _1, _2);
    receivedCallback = std::bind(&CI2pdNode::HandleServerReceived, pnode, _1, _2, _3);

    // now that we have a node register callbacks (bound to the node itself)
    // should this be before or after the vNodes.push?

    //auto node_shared = std::make_shared<CI2pdNode>(pnode);
    // I2PDK: these need to be lists (of callbacks) and we're just adding our callback to the list
    // when callbacks are to be called it iterates and sends the connection along, each callback 
    // then decides whether to process or not, something like that. We also need to 'decrement', 
    // i.e. both add and remove to the list - I guess on the CI2pNode deconstcruct or so.
    // GetSendMoreCallback
    // tunnel->SetConnectionCreatedCallback(std::bind(&CI2pdNode::HandleServerConnectionCreated, pnode, _1, _2));
    // tunnel->SetConnectedCallback(std::bind(&CI2pdNode::HandleServerClientConnected, pnode, _1, _2));
    // tunnel->SetReceivedCallback(std::bind(&CI2pdNode::HandleServerReceived, pnode, _1, _2, _3));

}

// I2PDK: this doesn't seem to have anything to do w/ i2pd or tunnels, this is called from init directly 
// on Bind (-bind, --whitelist and similar), whether that's still going to exist I'm not sure, or maybe 
// it's going to be a vehicle for binding directly to .i2p destinations who knows at this point. I.e. this
// could be a general type of functionality or related to nodes, not clear.
// ...actually, these always end up as nodes, so it is directly related to server nodes (likely MN bind related).
// (whether we may want / need to use it for something else we need to see - and duplicate this for non-i2pd
// scenario, node class as well, I doubt it though)
bool BindListenPort(const CDestination& addrBind, string& strError, bool fWhitelisted)
{
    using namespace std::placeholders;    // adds visibility of _1, _2, _3,...

    strError = "";
    int nOne = 1;

    // I2PDK: rewrite this for tunnels/i2pd
//    // Create socket for listening for incoming connections
//    struct sockaddr_storage sockaddr;
//    socklen_t len = sizeof(sockaddr);
//    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len)) {
//        strError = strprintf("Error: Bind address family for %s not supported", addrBind.ToString());
//        LogPrintf("%s\n", strError);
//        return false;
//    }
//
//    //I2PService tunnel = ...create server tunnel
//    SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
//    if (hListenSocket == INVALID_SOCKET) {
//        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %s)", NetworkErrorString(WSAGetLastError()));
//        LogPrintf("%s\n", strError);
//        return false;
//    }
//    if (!IsSelectableSocket(hListenSocket)) {
//        strError = "Error: Couldn't create a listenable socket for incoming connections";
//        LogPrintf("%s\n", strError);
//        return false;
//    }
//
//
//#ifndef WIN32
//#ifdef SO_NOSIGPIPE
//    // Different way of disabling SIGPIPE on BSD
//    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
//#endif
//    // Allow binding if the port is still in TIME_WAIT state after
//    // the program was closed and restarted. Not an issue on windows!
//    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
//#endif
//
//    // Set to non-blocking, incoming connections will also inherit this
//    if (!SetTunnelNonBlocking(hListenSocket, true)) {
//        strError = strprintf("BindListenPort: Setting listening socket to non-blocking failed, error %s\n", NetworkErrorString(WSAGetLastError()));
//        LogPrintf("%s\n", strError);
//        return false;
//    }
//
//    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
//    // and enable it by default or not. Try to enable it, if possible.
//    if (addrBind.IsIPv6()) {
//#ifdef IPV6_V6ONLY
//#ifdef WIN32
//        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&nOne, sizeof(int));
//#else
//        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&nOne, sizeof(int));
//#endif
//#endif
//#ifdef WIN32
//        int nProtLevel = PROTECTION_LEVEL_UNRESTRICTED;
//        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_PROTECTION_LEVEL, (const char*)&nProtLevel, sizeof(int));
//#endif
//    }
//
//    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR) {
//        int nErr = WSAGetLastError();
//        if (nErr == WSAEADDRINUSE)
//            strError = strprintf(_("Unable to bind to %s on this computer. COLX Core is probably already running."), addrBind.ToString());
//        else
//            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %s)"), addrBind.ToString(), NetworkErrorString(nErr));
//        LogPrintf("%s\n", strError);
//        CloseTunnel(hListenSocket);
//        return false;
//    }
//    LogPrintf("Bound to %s\n", addrBind.ToString());
//
//    // Listen for incoming connections
//    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR) {
//        strError = strprintf(_("Error: Listening for incoming connections failed (listen returned error %s)"), NetworkErrorString(WSAGetLastError()));
//        LogPrintf("%s\n", strError);
//        CloseTunnel(hListenSocket);
//        return false;
//    }

    std::shared_ptr<I2PPureServerTunnel> tunnel;

    // I2PDK: we're binding here only to be able to accept incoming connections/streams from clients.
    // but we're not yet creating server nodes, 'each' server node gets created only once the client stream
    // is opened - i.e. we have one server node per each client.
    // ServerStreamAcceptedCallback acceptedCallback = std::bind(HandleServerStreamAccepted, _1, addrBind, fWhitelisted);
    ServerStreamAcceptedCallback acceptedCallback = 
        std::bind(HandleServerStreamAccepted, _1, _2, _3, _4, _5, addrBind, fWhitelisted);
    if (ConnectServerTunnel(addrBind, tunnel, nConnectTimeout, acceptedCallback)) { //std::bind(HandleServerStreamAccepted, _1, addrBind, fWhitelisted))) {
        vhListenTunnel.push_back(ListenTunnel(tunnel, fWhitelisted));

        // what about this and i2p?
        if (addrBind.IsRoutable() && fDiscover && !fWhitelisted)
            AddLocal(addrBind, LOCAL_BIND);

        return true;
    }

    return false;
}

// I2PDK: is this needed? turned off to compile and makes no sense w/o sockets/ip-s
void static Discover(boost::thread_group& threadGroup)
{
    if (!fDiscover)
        return;

//#ifdef WIN32
//    // Get local host IP
//    char pszHostName[256] = "";
//    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR) {
//        vector<CI2pUrl> vaddr;
//        if (LookupHost(pszHostName, vaddr)) {
//            BOOST_FOREACH (const CI2pUrl& addr, vaddr) {
//                if (AddLocal(addr, LOCAL_IF))
//                    LogPrintf("%s: %s - %s\n", __func__, pszHostName, addr.ToString());
//            }
//        }
//    }
//#else
//    // Get local host ip
//    struct ifaddrs* myaddrs;
//    if (getifaddrs(&myaddrs) == 0) {
//        for (struct ifaddrs* ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next) {
//            if (ifa->ifa_addr == NULL) continue;
//            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
//            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
//            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
//            if (ifa->ifa_addr->sa_family == AF_INET) {
//                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
//                CI2pUrl addr(s4->sin_addr);
//                if (AddLocal(addr, LOCAL_IF))
//                    LogPrintf("%s: IPv4 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
//            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
//                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
//                CI2pUrl addr(s6->sin6_addr);
//                if (AddLocal(addr, LOCAL_IF))
//                    LogPrintf("%s: IPv6 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
//            }
//        }
//        freeifaddrs(myaddrs);
//    }
//#endif
}

// return true and url of the release folder if new update is available
// return false if error or update is not available
// I2PDK: nothing to do w/ i2p
static bool IsUpdateAvailable(CUrl& redirect)
{
    DebugPrintf("%s: starting\n", __func__);

    string urlRelease = GetArg("-checkforupdateurl", GITHUB_RELEASE_URL);

    string error;
    if (!CURLGetRedirect(urlRelease, redirect, error)) {
        DebugPrintf("%s: error - %s\n", __func__, error);
        if (urlRelease == GITHUB_RELEASE_URL) {
            boost::algorithm::replace_all(urlRelease, "ColossusCoinXT", "ColossusXT");
            if (!CURLGetRedirect(urlRelease, redirect, error)) {
                DebugPrintf("%s: error - %s\n", __func__, error);
                return false;
            }
        } else
            return false;
    }

    const string ver = strprintf("v%s", FormatVersion(CLIENT_VERSION));
    DebugPrintf("%s: redirect is %s, version is %s\n", __func__, redirect, ver);

    // assume version mismatch means new update is available (downgrage possible)
    if (redirect.find(ver) == string::npos) {
        LogPrintf("New version is available, please update your wallet! Go to: %s\n", redirect);
        return true;
    }
    else
        return false;
}

static bool ThreadCheckForUpdates(CContext& context)
{
    static bool bFirstRun = true;
    if (bFirstRun) {
        MilliSleep(60 * 1000); // wait 1 min, give wallet time to start
        bFirstRun = false;
    }

    CUrl urlRelease;
    if (!IsUpdateAvailable(urlRelease)) {
        context.GetAutoUpdateModel()->SetUpdateAvailable(false, "", "");
        return true; // continue thread execution
    }

    CUrl urlInfo = strprintf("%s/%s", urlRelease, "update.info");
    boost::algorithm::replace_first(urlInfo, "/tag/", "/download/");

    string error;
    CUrl urlInfoNew;
    if (CURLGetRedirect(urlInfo, urlInfoNew, error))
        urlInfo = urlInfoNew;

    string info;
    if (!CURLDownloadToMem(urlInfo, nullptr, info, error)) {
        LogPrintf("%s: %s\n", __func__, error);
        return true; // continue thread execution
    }

    string urlPath;
    if (!FindUpdateUrlForThisPlatform(info, urlPath, error)) {
        LogPrintf("%s: %s\n", __func__, error);
        return true; // continue thread execution
    } else {
        context.GetAutoUpdateModel()->SetUpdateAvailable(true, urlRelease, urlPath);
        uiInterface.NotifyUpdateAvailable();

        DebugPrintf("%s: update found, exit thread.\n", __func__);
        return false;
    }
}

// a thread manager for everything network related
void StartNode(boost::thread_group& threadGroup, CScheduler& scheduler)
{
    uiInterface.InitMessage(_("Loading addresses..."));
    // Load addresses for peers.dat
    int64_t nStart = GetTimeMillis();
    {
        CAddrDB adb;
        if (!adb.Read(addrman))
            LogPrintf("Invalid or missing peers.dat; recreating\n");
    }

    //try to read stored banlist
    CBanDB bandb;
    banmap_t banmap;
    if (!bandb.Read(banmap))
        LogPrintf("Invalid or missing banlist.dat; recreating\n");

    CI2pdNode::SetBanned(banmap); //thread save setter
    CI2pdNode::SetBannedSetDirty(false); //no need to write down just read or nonexistent data
    CI2pdNode::SweepBanned(); //sweap out unused entries

    LogPrintf("Loaded %i addresses from peers.dat  %dms\n",
        addrman.size(), GetTimeMillis() - nStart);
    fAddressesInitialized = true;

    if (semOutbound == NULL) {
        // initialize semaphore
        int nMaxOutbound = min(MAX_OUTBOUND_CONNECTIONS, nMaxConnections);
        semOutbound = new CSemaphore(nMaxOutbound);
    }

    if (pnodeLocalHost == NULL)
        // I2PDK: neither client nor server, invalid, no target 'addrName' either, no socket of any kind.
        //pnodeLocalHost = new CI2pdNode(INVALID_SOCKET, CI2PAddress(CDestination("127.0.0.1", 0), nLocalServices));
        pnodeLocalHost = new CI2pdNode(nullptr, CI2PAddress(CDestination("127.0.0.1", 0), nLocalServices));

    // I2PDK: do we need this?
    //Discover(threadGroup);

    //
    // Start threads
    //

    if (!GetBoolArg("-dnsseed", true))
        LogPrintf("DNS seeding disabled\n");
    else
        threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "dnsseed", &ThreadDNSAddressSeed));

    // Map ports with UPnP
    MapPort(GetBoolArg("-upnp", DEFAULT_UPNP));

    // Send and receive from tunnels, accept connections
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "net", &ThreadTunnelHandler));

    // Initiate outbound connections from -addnode
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "addcon", &ThreadOpenAddedConnections));

    // Initiate outbound connections
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "opencon", &ThreadOpenConnections));

    // Process messages
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "msghand", &ThreadMessageHandler));

    // Dump network addresses
    threadGroup.create_thread(boost::bind(&LoopForever<bool (*)()>, "dumpaddr", &DumpAddresses, DUMP_ADDRESSES_INTERVAL * 1000));

    // Dump banned addresses
    scheduler.scheduleEvery(&DumpBanlist, DUMP_ADDRESSES_INTERVAL);

    // ppcoin:mint proof-of-stake blocks in the background
    if (GetBoolArg("-staking", true))
        threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "stakemint", &ThreadStakeMinter));

    // Check for updates once per day
    int64_t nCheckForUpdatesInterval = 1000 * GetArg("-checkforupdate", 60 * 60 * 24);
    if (nCheckForUpdatesInterval > 0) {
        threadGroup.create_thread(boost::bind(&LoopForever<bool (*)()>, "checkforupdates",
            [](){ return ThreadCheckForUpdates(GetContext()); }, nCheckForUpdatesInterval));
    }
}

bool StopNode()
{
    LogPrintf("StopNode()\n");
    MapPort(false);
    if (semOutbound)
        for (int i = 0; i < MAX_OUTBOUND_CONNECTIONS; i++)
            semOutbound->post();

    if (fAddressesInitialized) {
        DumpAddresses();
        DumpBanlist();
        fAddressesInitialized = false;
    }

    return true;
}

class CNetCleanup
{
public:
    CNetCleanup() {}

    ~CNetCleanup()
    {
        // Close tunnels
        BOOST_FOREACH (CI2pdNode* pnode, vNodes)
            if (pnode->i2pTunnel != nullptr) //INVALID_SOCKET)
                CloseTunnel(pnode->i2pTunnel, pnode->fInbound);
        BOOST_FOREACH (ListenTunnel& hListenTunnel, vhListenTunnel)
            if (hListenTunnel.tunnel != nullptr) // INVALID_SOCKET)
                if (!CloseTunnel(hListenTunnel.tunnel))
                    LogPrintf("CloseTunnel(hListenTunnel) failed with error %s\n", NetworkErrorString(WSAGetLastError()));

        // clean up some globals (to help leak detection)
        BOOST_FOREACH (CI2pdNode* pnode, vNodes)
            delete pnode;
        BOOST_FOREACH (CI2pdNode* pnode, vNodesDisconnected)
            delete pnode;
        vNodes.clear();
        vNodesDisconnected.clear();
        vhListenTunnel.clear();
        delete semOutbound;
        semOutbound = NULL;
        delete pnodeLocalHost;
        pnodeLocalHost = NULL;

#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
} instance_of_cnetcleanup;

void CExplicitNetCleanup::callCleanup()
{
    // Explicit call to destructor of CNetCleanup because it's not implicitly called
    // when the wallet is restarted from within the wallet itself.
    CNetCleanup* tmp = new CNetCleanup();
    delete tmp; // Stroustrup's gonna kill me for that
}

void RelayTransaction(const CTransaction& tx)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(10000);
    ss << tx;
    RelayTransaction(tx, ss);
}

void RelayTransaction(const CTransaction& tx, const CDataStream& ss)
{
    CInv inv(MSG_TX, tx.GetHash());
    {
        LOCK(cs_mapRelay);
        // Expire old relay messages
        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < GetTime()) {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        // Save original serialized message so newer versions are preserved
        mapRelay.insert(std::make_pair(inv, ss));
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));
    }
    LOCK(cs_vNodes);
    BOOST_FOREACH (CI2pdNode* pnode, vNodes) {
        if (!pnode->fRelayTxes)
            continue;
        LOCK(pnode->cs_filter);
        if (pnode->pfilter) {
            if (pnode->pfilter->IsRelevantAndUpdate(tx))
                pnode->PushInventory(inv);
        } else
            pnode->PushInventory(inv);
    }
}

void RelayTransactionLockReq(const CTransaction& tx, bool relayToAll)
{
    CInv inv(MSG_TXLOCK_REQUEST, tx.GetHash());

    //broadcast the new lock
    LOCK(cs_vNodes);
    BOOST_FOREACH (CI2pdNode* pnode, vNodes) {
        if (!relayToAll && !pnode->fRelayTxes)
            continue;

        pnode->PushMessage("ix", tx);
    }
}

void RelayInv(CInv& inv)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH (CI2pdNode* pnode, vNodes){
            if((pnode->nServices==NODE_BLOOM_WITHOUT_MN) && inv.IsMasterNodeType())continue;
        if (pnode->nVersion >= ActiveProtocol())
            pnode->PushInventory(inv);
    }
}

void CI2pdNode::RecordBytesRecv(uint64_t bytes)
{
    LOCK(cs_totalBytesRecv);
    nTotalBytesRecv += bytes;
}

void CI2pdNode::RecordBytesSent(uint64_t bytes)
{
    LOCK(cs_totalBytesSent);
    nTotalBytesSent += bytes;
}

uint64_t CI2pdNode::GetTotalBytesRecv()
{
    LOCK(cs_totalBytesRecv);
    return nTotalBytesRecv;
}

uint64_t CI2pdNode::GetTotalBytesSent()
{
    LOCK(cs_totalBytesSent);
    return nTotalBytesSent;
}

void CI2pdNode::Fuzz(int nChance)
{
    if (!fSuccessfullyConnected) return; // Don't fuzz initial handshake
    if (GetRand(nChance) != 0) return;   // Fuzz 1 of every nChance messages

    switch (GetRand(3)) {
    case 0:
        // xor a random byte with a random value:
        if (!ssSend.empty()) {
            CDataStream::size_type pos = GetRand(ssSend.size());
            ssSend[pos] ^= (unsigned char)(GetRand(256));
        }
        break;
    case 1:
        // delete a random byte:
        if (!ssSend.empty()) {
            CDataStream::size_type pos = GetRand(ssSend.size());
            ssSend.erase(ssSend.begin() + pos);
        }
        break;
    case 2:
        // insert a random byte at a random position
        {
            CDataStream::size_type pos = GetRand(ssSend.size());
            char ch = (char)GetRand(256);
            ssSend.insert(ssSend.begin() + pos, ch);
        }
        break;
    }
    // Chance of more than one change half the time:
    // (more changes exponentially less likely):
    Fuzz(2);
}

//
// CAddrDB
//

CAddrDB::CAddrDB()
{
    pathAddr = GetDataDir() / "peers.dat";
}

void CAddrDB::RemoveStorage() const
{
    if (boost::filesystem::exists(pathAddr)) {
        LogPrintf("%s: Deleting peers database %s\n", __func__, pathAddr.string());
        boost::filesystem::remove(pathAddr);
    }
}

bool CAddrDB::Write(const CAddrMan& addr)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("peers.dat.%04x", randv);

    // serialize addresses, checksum data up to that point, then append csum
    CDataStream ssPeers(SER_DISK, CLIENT_VERSION);
    ssPeers << FLATDATA(Params().MessageStart());
    ssPeers << addr;
    uint256 hash = Hash(ssPeers.begin(), ssPeers.end());
    ssPeers << hash;

    // open output file, and associate with CAutoFile
    boost::filesystem::path pathAddr = GetDataDir() / "peers.dat";
    FILE* file = fopen(pathAddr.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathAddr.string());

    // Write and commit header, data
    try {
        fileout << ssPeers;
    } catch (std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    return true;
}

bool CAddrDB::Read(CAddrMan& addr)
{
    // open input file, and associate with CAutoFile
    FILE* file = fopen(pathAddr.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s : Failed to open file %s", __func__, pathAddr.string());

    // use file size to size memory buffer
    uint64_t fileSize = boost::filesystem::file_size(pathAddr);
    uint64_t dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }
    filein.fclose();

    CDataStream ssPeers(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssPeers.begin(), ssPeers.end());
    if (hashIn != hashTmp)
        return error("%s : Checksum mismatch, data corrupted", __func__);

    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssPeers >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("%s : Invalid network magic number", __func__);

        // de-serialize address data into one CAddrMan object
        ssPeers >> addr;
    } catch (std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

unsigned int ReceiveFloodSize() { return 1000 * GetArg("-maxreceivebuffer", 5 * 1000); }
unsigned int SendBufferSize() { return 1000 * GetArg("-maxsendbuffer", 1 * 1000); }

CI2pdNode::CI2pdNode(std::shared_ptr<I2PService> tunnelIn, CI2PAddress addrIn, std::string addrNameIn, bool fInboundIn) : ssSend(SER_NETWORK, INIT_PROTO_VERSION), setAddrKnown(5000)
{
    nServices = 0;
    i2pTunnel = tunnelIn;
    nRecvVersion = INIT_PROTO_VERSION;
    nLastSend = 0;
    nLastRecv = 0;
    nSendBytes = 0;
    nRecvBytes = 0;
    nTimeConnected = GetTime();
    nTimeOffset = 0;
    addr = addrIn;
    addrName = addrNameIn == "" ? addr.ToStringIPPort() : addrNameIn;
    nVersion = 0;
    strSubVer = "";
    fWhitelisted = false;
    fOneShot = false;
    fClient = false; // set by version message
    fInbound = fInboundIn;
    fNetworkNode = false;
    fSuccessfullyConnected = false;
    fDisconnect = false;
    nRefCount = 0;
    nSendSize = 0;
    nSendOffset = 0;
    hashContinue = 0;
    nStartingHeight = -1;
    fGetAddr = false;
    fRelayTxes = false;
    setInventoryKnown.max_size(SendBufferSize() / 1000);
    pfilter = new CBloomFilter();
    nPingNonceSent = 0;
    nPingUsecStart = 0;
    nPingUsecTime = 0;
    fPingQueued = false;
    fObfuScationMaster = false;

    _receivedBuffer = std::unique_ptr<uint8_t[]>();
    _receivedBufferSize = 0;
    _hasPushedMessages = false;

    {
        LOCK(cs_nLastNodeId);
        id = nLastNodeId++;
    }

    if (fLogIPs)
        LogPrintf("net: Added connection to %s peer=%d\n", addrName, id);
    else
        LogPrintf("net: Added connection peer=%d\n", id);

    // Be shy and don't send version until we hear
    //if (i2pTunnel != INVALID_SOCKET && !fInbound)
    if (i2pTunnel != nullptr && !fInbound)
        PushVersion();

    GetNodeSignals().InitializeNode(GetId(), this);
}

CI2pdNode::~CI2pdNode()
{
    CloseTunnel(i2pTunnel, fInbound);

    if (pfilter)
        delete pfilter;

    GetNodeSignals().FinalizeNode(GetId());
}

// std::shared_ptr<i2p::client::I2PPureTunnelConnection> connection
void CI2pdNode::HandleClientConnectionCreated(std::shared_ptr<i2p::client::I2PPureTunnelConnection> connection)
{
    _connection = connection;
}

void CI2pdNode::HandleClientConnected(std::shared_ptr<i2p::client::I2PPureTunnelConnection> connection)
{
    // we're now ready to send etc. - so trigger IsReady or something
}

// void CI2pdNode::HandleClientReceived(std::string message, ContinueToReceiveCallback continueToReceiveCallback)
void CI2pdNode::HandleClientReceived(const uint8_t * buf, size_t len, ContinueToReceiveCallback continueToReceiveCallback)
{
    // std::string message((const char*)buf, len);
    std::string message((const char*)buf, std::min((int)len, 30));
    LogPrintf("HandleClientReceived: message received...'%s' (%d) \n", message, len);

    LOCK(cs_messageReceived);

    // if (_hasPushedMessages || !_messageReceived.empty()) {
    if (_hasPushedMessages || _receivedBufferSize > 0) {
        LogPrintf("net: HandleServerReceived: this shouldn't have happened! old message is not processed and we have a new one coming in?  (%s)\n", addr.ToString());
        // try and do a fast message processing here if possible? we're outside the processing thread so it's not easy
    }

    // std::shared_ptr<uint8_t> sp(new uint8_t[10], std::default_delete<uint8_t[]>());

    auto buffer = new uint8_t[len];
    memcpy (buffer, buf, len);
    _receivedBuffer = std::unique_ptr<uint8_t[]>(buffer); // _receivedBuffer.reset (buffer);
    _receivedBufferSize = len;

    _hasPushedMessages = true;

    // _messageReceived = message;
}

void CI2pdNode::HandleServerConnectionCreated(std::shared_ptr<I2PPureServerTunnel> tunnel, std::shared_ptr<i2p::client::I2PPureTunnelConnection> connection)
{
    using namespace std::placeholders;    // adds visibility of _1, _2, _3,...

    LogPrintf("HandleServerConnectionCreated: connection created...'%s' \n", connection->GetRemoteIdentity());
    
    // now we can bind the connection send for when we need to send messages (this is the right place to do it)
    _connection = connection;

    //_sendCallback = std::bind(&I2PPureTunnelConnection::HandleSendRawSigned, connection, _1, _2);

    //// shared_from_this()
    ////auto readyCallback = std::bind(&CI2pdNode::HandleReadyToSend, this);
    //auto errorCallback = std::bind(&CI2pdNode::HandleErrorSend, this, _1);
    //_sendMoreCallback = std::bind(&I2PPureTunnelConnection::HandleSendReadyRawSigned, connection, _1, _2, nullptr, errorCallback); // , _3);

    _errorCallback = std::bind(&CI2pdNode::HandleErrorSend, this, _1);

    //_continueToReceiveCallback = std::bind(&I2PPureTunnelConnection::HandleWriteAsync, connection, _1);
}

void CI2pdNode::HandleServerClientConnected(std::shared_ptr<I2PPureServerTunnel> tunnel, std::shared_ptr<i2p::client::I2PPureTunnelConnection> connection)
{
    LogPrintf("HandleServerClientConnected: connected...'%s' \n", connection->GetRemoteIdentity());
}

// void CI2pdNode::HandleServerReceived(std::string message, ContinueToReceiveCallback continueToReceiveCallback)
void CI2pdNode::HandleServerReceived(const uint8_t * buf, size_t len, ContinueToReceiveCallback continueToReceiveCallback)
{
    // std::string message((const char*)buf, len);
    std::string message((const char*)buf, std::min((int)len, 30));
    
    LogPrintf("HandleServerReceived: message received...'%s' (%d), from '%s' \n", message, len, _connection->GetRemoteIdentity());
    // LogPrintf("HandleServerReceived: message received...'%s' (%d) \n", _connection->GetRemoteIdentity(), len);

    LOCK(cs_messageReceived);

    if (_hasPushedMessages || _receivedBufferSize > 0) { //!_messageReceived.empty()) {
        LogPrintf("net: HandleServerReceived: this shouldn't have happened! old message is not processed and we have a new one coming in?  (%s)\n", addr.ToString());
        // try and do a fast message processing here if possible? we're outside the processing thread so it's not easy
    }

    auto buffer = new uint8_t[len];
    memcpy (buffer, buf, len);
    _receivedBuffer = std::unique_ptr<uint8_t[]>(buffer); // _receivedBuffer.reset (buffer);
    _receivedBufferSize = len;

    _hasPushedMessages = true;

    // _messageReceived = message;
}

// this gets, clears and signals the tunnel to continue receiving
// should _messageReceived be locked?
// std::string CI2pdNode::PopMessageReceived()
size_t CI2pdNode::PopMessageReceived(std::unique_ptr<uint8_t[]>& buffer)
{
    // std::string message;
    size_t len = 0;
    bool hasPushedMessages;

    {
        LOCK(cs_messageReceived);

        // only nullptr _messageReceived means we've had nothing received, anything else (even empty) requires restarting...
        hasPushedMessages = _hasPushedMessages; // && !_messageReceived.empty()

        if ((hasPushedMessages && _receivedBufferSize <= 0) ||
            (!hasPushedMessages && _receivedBufferSize > 0)) {
            LogPrintf("net: PopMessageReceived: this shouldn't have happened!?\n");
        }

        if (hasPushedMessages && _receivedBufferSize > 0) {
            memcpy (buffer.get(), _receivedBuffer.get(), _receivedBufferSize);
            len = _receivedBufferSize;
        }

        _receivedBuffer = std::unique_ptr<uint8_t[]>(); // _receivedBuffer.reset ();
        _receivedBufferSize = 0;
        _hasPushedMessages = false;

        // message = hasPushedMessages ? _messageReceived : message;
        // _hasPushedMessages = false;
        // _messageReceived = std::string();
    }

    // signal the tunnel/connection to continue receiving (as it stops automatically once we receive something)
    if (hasPushedMessages)
        _connection->HandleWriteAsync(boost::system::error_code());

    if (len > 0) {
        // std::string message((const char*)buffer.get(), len);
        std::string message((const char*)buffer.get(), std::min((int)len, 30));
        LogPrintf("PopMessageReceived: message popped...'%s' (%d) \n", message, len);
    }

    return len;
}

void CI2pdNode::HandleReadyToSend()
{
}

void CI2pdNode::HandleErrorSend(const boost::system::error_code& ecode)
{
    LogPrintf("net: HandleErrorSend: %s  (%s)\n", ecode.message(), addr.ToString());
}

void CI2pdNode::AskFor(const CInv& inv)
{
    if (mapAskFor.size() > MAPASKFOR_MAX_SZ)
        return;
    // We're using mapAskFor as a priority queue,
    // the key is the earliest time the request can be sent
    int64_t nRequestTime;
    limitedmap<CInv, int64_t>::const_iterator it = mapAlreadyAskedFor.find(inv);
    if (it != mapAlreadyAskedFor.end())
        nRequestTime = it->second;
    else
        nRequestTime = 0;
    LogPrintf("net: askfor %s  %d (%s) peer=%d\n", inv.ToString(), nRequestTime, DateTimeStrFormat("%H:%M:%S", nRequestTime / 1000000), id);

    // Make sure not to reuse time indexes to keep things in the same order
    int64_t nNow = GetTimeMicros() - 1000000;
    static int64_t nLastTime;
    ++nLastTime;
    nNow = std::max(nNow, nLastTime);
    nLastTime = nNow;

    // Each retry is 2 minutes after the last
    nRequestTime = std::max(nRequestTime + 2 * 60 * 1000000, nNow);
    if (it != mapAlreadyAskedFor.end())
        mapAlreadyAskedFor.update(it, nRequestTime);
    else
        mapAlreadyAskedFor.insert(std::make_pair(inv, nRequestTime));
    mapAskFor.insert(std::make_pair(nRequestTime, inv));
}

void CI2pdNode::BeginMessage(const char* pszCommand) EXCLUSIVE_LOCK_FUNCTION(cs_vSend)
{
    ENTER_CRITICAL_SECTION(cs_vSend);
    assert(ssSend.size() == 0);
    ssSend << CMessageHeader(pszCommand, 0);
    LogPrintf("net.BeginMessage: sending: command: %s \n", SanitizeString(pszCommand));
}

void CI2pdNode::AbortMessage() UNLOCK_FUNCTION(cs_vSend)
{
    ssSend.clear();

    LEAVE_CRITICAL_SECTION(cs_vSend);

    LogPrintf("net.AbortMessage: (aborted)\n");
}

void CI2pdNode::EndMessage() UNLOCK_FUNCTION(cs_vSend)
{
    // The -*messagestest options are intentionally not documented in the help message,
    // since they are only used during development to debug the networking code and are
    // not intended for end-users.
    if (mapArgs.count("-dropmessagestest") && GetRand(GetArg("-dropmessagestest", 2)) == 0) {
        LogPrintf("net: dropmessages DROPPING SEND MESSAGE\n");
        AbortMessage();
        return;
    }
    if (mapArgs.count("-fuzzmessagestest"))
        Fuzz(GetArg("-fuzzmessagestest", 10));

    if (ssSend.size() == 0)
        return;

    auto str = ssSend.str();
    LogPrintf("net.EndMessage: message: '%s' (%d)\n", str, str.size());

    // Set the size
    unsigned int nSize = ssSend.size() - CMessageHeader::HEADER_SIZE;
    memcpy((char*)&ssSend[CMessageHeader::MESSAGE_SIZE_OFFSET], &nSize, sizeof(nSize));

    // Set the checksum
    uint256 hash = Hash(ssSend.begin() + CMessageHeader::HEADER_SIZE, ssSend.end());
    unsigned int nChecksum = 0;
    memcpy(&nChecksum, &hash, sizeof(nChecksum));
    assert(ssSend.size() >= CMessageHeader::CHECKSUM_OFFSET + sizeof(nChecksum));
    memcpy((char*)&ssSend[CMessageHeader::CHECKSUM_OFFSET], &nChecksum, sizeof(nChecksum));

    LogPrintf("net.EndMessage: (%d bytes) peer=%d\n", nSize, id);

    std::deque<CSerializeData>::iterator it = vSendMsg.insert(vSendMsg.end(), CSerializeData());
    ssSend.GetAndClear(*it);
    nSendSize += (*it).size();

    // If write queue empty, attempt "optimistic write"
    if (it == vSendMsg.begin())
        TunnelSendData(this);

    LEAVE_CRITICAL_SECTION(cs_vSend);
}

//
// CBanDB
//

CBanDB::CBanDB()
{
    pathBanlist = GetDataDir() / "banlist.dat";
}

bool CBanDB::Write(const banmap_t& banSet)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("banlist.dat.%04x", randv);

    // serialize banlist, checksum data up to that point, then append csum
    CDataStream ssBanlist(SER_DISK, CLIENT_VERSION);
    ssBanlist << FLATDATA(Params().MessageStart());
    ssBanlist << banSet;
    uint256 hash = Hash(ssBanlist.begin(), ssBanlist.end());
    ssBanlist << hash;

    // open temp output file, and associate with CAutoFile
    boost::filesystem::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fopen(pathTmp.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: Failed to open file %s", __func__, pathTmp.string());

    // Write and commit header, data
    try {
        fileout << ssBanlist;
    }
    catch (const std::exception& e) {
        return error("%s: Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    // replace existing banlist.dat, if any, with new banlist.dat.XXXX
    if (!RenameOver(pathTmp, pathBanlist))
        return error("%s: Rename-into-place failed", __func__);

    return true;
}

bool CBanDB::Read(banmap_t& banSet)
{
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathBanlist.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: Failed to open file %s", __func__, pathBanlist.string());

    // use file size to size memory buffer
    uint64_t fileSize = boost::filesystem::file_size(pathBanlist);
    uint64_t dataSize = 0;
    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    filein.fclose();

    CDataStream ssBanlist(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssBanlist.begin(), ssBanlist.end());
    if (hashIn != hashTmp)
        return error("%s: Checksum mismatch, data corrupted", __func__);

    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssBanlist >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("%s: Invalid network magic number", __func__);

        // de-serialize address data into one CAddrMan object
        ssBanlist >> banSet;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

void DumpBanlist()
{
    CI2pdNode::SweepBanned(); // clean unused entries (if bantime has expired)

    if (!CI2pdNode::BannedSetIsDirty())
        return;

    int64_t nStart = GetTimeMillis();

    CBanDB bandb;
    banmap_t banmap;
    CI2pdNode::GetBanned(banmap);
    if (bandb.Write(banmap)) {
        CI2pdNode::SetBannedSetDirty(false);
    }

    LogPrintf("net: Flushed %d banned node ips/subnets to banlist.dat  %dms\n",
        banmap.size(), GetTimeMillis() - nStart);
}
