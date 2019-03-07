// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifdef HAVE_CONFIG_H
#include "config/colx-config.h"
#endif

#include "netdestination.h"

#include "I2PService.h"
#include <I2PTunnel.h>
#include <I2PPureTunnel.h>
#include <ClientContext.h>
#include <Destination.h>
#include <Identity.h>

#include "hash.h"
#include "sync.h"
#include "uint256.h"
#include "random.h"
#include "util.h"
#include "utilstrencodings.h"

#ifdef HAVE_GETADDRINFO_A
#include <netdb.h>
#endif

#ifndef WIN32
#if HAVE_INET_PTON
#include <arpa/inet.h>
#endif
#include <fcntl.h>
#endif

#include <algorithm>
#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/algorithm/string/predicate.hpp> // for startswith() and endswith()
#include <boost/thread.hpp>
#include <boost/asio.hpp>

#if !defined(HAVE_MSG_NOSIGNAL) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

using namespace std;
using namespace i2p::client;
using namespace i2p::data;

// Settings
//static proxyType proxyInfo[NET_MAX];
//static proxyType nameProxy;
//static CCriticalSection cs_proxyInfos;
//int nConnectTimeout = DEFAULT_CONNECT_TIMEOUT;

// duplicates with netbase.*
bool isNameLookup = false;

// Need ample time for negotiation for very slow proxies such as Tor (milliseconds)
static const int SOCKS5_RECV_TIMEOUT = 20 * 1000;

//bool static LookupI2pIntern(const char* pszName, std::vector<CI2pUrl>& vIP, unsigned int nMaxSolutions, bool fAllowLookup)
//{
//    vIP.clear();
//
//    {
//        CI2pUrl addr;
//        if (addr.SetSpecial(std::string(pszName))) {
//            vIP.push_back(addr);
//            return true;
//        }
//    }
//
//#ifdef HAVE_GETADDRINFO_A
//    struct in_addr ipv4_addr;
//#ifdef HAVE_INET_PTON
//    if (inet_pton(AF_INET, pszName, &ipv4_addr) > 0) {
//        vIP.push_back(CI2pUrl(ipv4_addr));
//        return true;
//    }
//
//    struct in6_addr ipv6_addr;
//    if (inet_pton(AF_INET6, pszName, &ipv6_addr) > 0) {
//        vIP.push_back(CI2pUrl(ipv6_addr));
//        return true;
//    }
//#else
//    ipv4_addr.s_addr = inet_addr(pszName);
//    if (ipv4_addr.s_addr != INADDR_NONE) {
//        vIP.push_back(CI2pUrl(ipv4_addr));
//        return true;
//    }
//#endif
//#endif
//
//    struct addrinfo aiHint;
//    memset(&aiHint, 0, sizeof(struct addrinfo));
//    aiHint.ai_socktype = SOCK_STREAM;
//    aiHint.ai_protocol = IPPROTO_TCP;
//    aiHint.ai_family = AF_UNSPEC;
//#ifdef WIN32
//    aiHint.ai_flags = fAllowLookup ? 0 : AI_NUMERICHOST;
//#else
//    aiHint.ai_flags = fAllowLookup ? AI_ADDRCONFIG : AI_NUMERICHOST;
//#endif
//
//    struct addrinfo* aiRes = NULL;
//#ifdef HAVE_GETADDRINFO_A
//    struct gaicb gcb, *query = &gcb;
//    memset(query, 0, sizeof(struct gaicb));
//    gcb.ar_name = pszName;
//    gcb.ar_request = &aiHint;
//    int nErr = getaddrinfo_a(GAI_NOWAIT, &query, 1, NULL);
//    if (nErr)
//        return false;
//
//    do {
//        // Should set the timeout limit to a resonable value to avoid
//        // generating unnecessary checking call during the polling loop,
//        // while it can still response to stop request quick enough.
//        // 2 seconds looks fine in our situation.
//        struct timespec ts = {2, 0};
//        gai_suspend(&query, 1, &ts);
//        boost::this_thread::interruption_point();
//
//        nErr = gai_error(query);
//        if (0 == nErr)
//            aiRes = query->ar_result;
//    } while (nErr == EAI_INPROGRESS);
//#else
//    int nErr = getaddrinfo(pszName, NULL, &aiHint, &aiRes);
//#endif
//    if (nErr)
//        return false;
//
//    struct addrinfo* aiTrav = aiRes;
//    while (aiTrav != NULL && (nMaxSolutions == 0 || vIP.size() < nMaxSolutions)) {
//        if (aiTrav->ai_family == AF_INET) {
//            assert(aiTrav->ai_addrlen >= sizeof(sockaddr_in));
//            vIP.push_back(CI2pUrl(((struct sockaddr_in*)(aiTrav->ai_addr))->sin_addr));
//        }
//
//        if (aiTrav->ai_family == AF_INET6) {
//            assert(aiTrav->ai_addrlen >= sizeof(sockaddr_in6));
//            vIP.push_back(CI2pUrl(((struct sockaddr_in6*)(aiTrav->ai_addr))->sin6_addr));
//        }
//
//        aiTrav = aiTrav->ai_next;
//    }
//
//    freeaddrinfo(aiRes);
//
//    return (vIP.size() > 0);
//}
//
//bool LookupI2pHost(const char* pszName, std::vector<CI2pUrl>& vIP, unsigned int nMaxSolutions, bool fAllowLookup)
//{
//    std::string strHost(pszName);
//    if (strHost.empty())
//        return false;
//    if (boost::algorithm::starts_with(strHost, "[") && boost::algorithm::ends_with(strHost, "]")) {
//        strHost = strHost.substr(1, strHost.size() - 2);
//    }
//
//    return LookupI2pIntern(strHost.c_str(), vIP, nMaxSolutions, fAllowLookup);
//}

// I2PDK: just to compile, turned off but unsure yet if we're going to need anything similar.
bool IsProxy(const CI2pUrl& addr)
{
    return false;
}

bool static LookupIntern(const char* pszName, std::vector<CI2pUrl>& vIP, unsigned int nMaxSolutions, bool fAllowLookup)
{
    vIP.clear();

    vIP.push_back(CI2pUrl(std::string(pszName)));
    //return true;

    return (vIP.size() > 0);

//    {
//        CNetAddr addr;
//        if (addr.SetSpecial(std::string(pszName))) {
//            vIP.push_back(addr);
//            return true;
//        }
//    }
//
//#ifdef HAVE_GETADDRINFO_A
//    struct in_addr ipv4_addr;
//#ifdef HAVE_INET_PTON
//    if (inet_pton(AF_INET, pszName, &ipv4_addr) > 0) {
//        vIP.push_back(CNetAddr(ipv4_addr));
//        return true;
//    }
//
//    struct in6_addr ipv6_addr;
//    if (inet_pton(AF_INET6, pszName, &ipv6_addr) > 0) {
//        vIP.push_back(CNetAddr(ipv6_addr));
//        return true;
//    }
//#else
//    ipv4_addr.s_addr = inet_addr(pszName);
//    if (ipv4_addr.s_addr != INADDR_NONE) {
//        vIP.push_back(CNetAddr(ipv4_addr));
//        return true;
//    }
//#endif
//#endif
//
//    struct addrinfo aiHint;
//    memset(&aiHint, 0, sizeof(struct addrinfo));
//    aiHint.ai_socktype = SOCK_STREAM;
//    aiHint.ai_protocol = IPPROTO_TCP;
//    aiHint.ai_family = AF_UNSPEC;
//#ifdef WIN32
//    aiHint.ai_flags = fAllowLookup ? 0 : AI_NUMERICHOST;
//#else
//    aiHint.ai_flags = fAllowLookup ? AI_ADDRCONFIG : AI_NUMERICHOST;
//#endif
//
//    struct addrinfo* aiRes = NULL;
//#ifdef HAVE_GETADDRINFO_A
//    struct gaicb gcb, *query = &gcb;
//    memset(query, 0, sizeof(struct gaicb));
//    gcb.ar_name = pszName;
//    gcb.ar_request = &aiHint;
//    int nErr = getaddrinfo_a(GAI_NOWAIT, &query, 1, NULL);
//    if (nErr)
//        return false;
//
//    do {
//        // Should set the timeout limit to a resonable value to avoid
//        // generating unnecessary checking call during the polling loop,
//        // while it can still response to stop request quick enough.
//        // 2 seconds looks fine in our situation.
//        struct timespec ts = { 2, 0 };
//        gai_suspend(&query, 1, &ts);
//        boost::this_thread::interruption_point();
//
//        nErr = gai_error(query);
//        if (0 == nErr)
//            aiRes = query->ar_result;
//    } while (nErr == EAI_INPROGRESS);
//#else
//    int nErr = getaddrinfo(pszName, NULL, &aiHint, &aiRes);
//#endif
//    if (nErr)
//        return false;
//
//    struct addrinfo* aiTrav = aiRes;
//    while (aiTrav != NULL && (nMaxSolutions == 0 || vIP.size() < nMaxSolutions)) {
//        if (aiTrav->ai_family == AF_INET) {
//            assert(aiTrav->ai_addrlen >= sizeof(sockaddr_in));
//            vIP.push_back(CNetAddr(((struct sockaddr_in*)(aiTrav->ai_addr))->sin_addr));
//        }
//
//        if (aiTrav->ai_family == AF_INET6) {
//            assert(aiTrav->ai_addrlen >= sizeof(sockaddr_in6));
//            vIP.push_back(CNetAddr(((struct sockaddr_in6*)(aiTrav->ai_addr))->sin6_addr));
//        }
//
//        aiTrav = aiTrav->ai_next;
//    }
//
//    freeaddrinfo(aiRes);
}

bool LookupHost(const char* pszName, std::vector<CI2pUrl>& vIP, unsigned int nMaxSolutions, bool fAllowLookup)
{
    std::string strHost(pszName);
    if (strHost.empty())
        return false;
    if (boost::algorithm::starts_with(strHost, "[") && boost::algorithm::ends_with(strHost, "]")) {
        strHost = strHost.substr(1, strHost.size() - 2);
    }

    return LookupIntern(strHost.c_str(), vIP, nMaxSolutions, fAllowLookup);
}

bool Lookup(const char* pszName, std::vector<CDestination>& vAddr, int portDefault, bool fAllowLookup, unsigned int nMaxSolutions)
{
    if (pszName[0] == 0)
        return false;
    int port = portDefault;
    std::string hostname = "";
    SplitHostPort(std::string(pszName), port, hostname);

    std::vector<CI2pUrl> vIP;
    //bool fRet = LookupI2pIntern(hostname.c_str(), vIP, nMaxSolutions, fAllowLookup);
    //if (!fRet)
    //    return false;
    //vAddr.resize(vIP.size());

    vAddr.resize(1);
    vAddr[0] = CDestination(CI2pUrl(hostname), port);
    // for (unsigned int i = 0; i < vIP.size(); i++) {
    //     vAddr[i] = CDestination(CI2pUrl(hostname), port);
    // }
    return true;
}

bool Lookup(const char* pszName, CDestination& addr, int portDefault, bool fAllowLookup)
{
    std::vector<CDestination> vService;
    bool fRet = Lookup(pszName, vService, portDefault, fAllowLookup, 1);
    if (!fRet)
        return false;
    addr = vService[0];
    return true;
}

bool LookupNumeric(const char* pszName, CDestination& addr, int portDefault)
{
    return Lookup(pszName, addr, portDefault, false);
}

void ReadDefaultI2CPOptions(std::map<std::string, std::string>& options)
{
    options[I2CP_PARAM_INBOUND_TUNNEL_LENGTH] = std::to_string(DEFAULT_INBOUND_TUNNEL_LENGTH);
    options[I2CP_PARAM_OUTBOUND_TUNNEL_LENGTH] = std::to_string(DEFAULT_OUTBOUND_TUNNEL_LENGTH);
    options[I2CP_PARAM_INBOUND_TUNNELS_QUANTITY] = std::to_string(DEFAULT_INBOUND_TUNNELS_QUANTITY);
    options[I2CP_PARAM_OUTBOUND_TUNNELS_QUANTITY] = std::to_string(DEFAULT_OUTBOUND_TUNNELS_QUANTITY);
    options[I2CP_PARAM_TAGS_TO_SEND] = std::to_string(DEFAULT_TAGS_TO_SEND);
    options[I2CP_PARAM_MIN_TUNNEL_LATENCY] = std::to_string(DEFAULT_MIN_TUNNEL_LATENCY);
    options[I2CP_PARAM_MAX_TUNNEL_LATENCY] = std::to_string(DEFAULT_MAX_TUNNEL_LATENCY);
    options[I2CP_PARAM_STREAMING_INITIAL_ACK_DELAY] = std::to_string(DEFAULT_INITIAL_ACK_DELAY);
}

std::shared_ptr<ClientDestination> static GetLocalDestination(std::string dest, bool isServer) //= false)
{
    // I2CP
    std::map<std::string, std::string> options;
    ReadDefaultI2CPOptions(options);

    bool isPublic = isServer; // false; // true; // server || type == I2P_TUNNELS_SECTION_TYPE_UDPCLIENT;
    i2p::data::SigningKeyType sigType = i2p::data::SIGNING_KEY_TYPE_ECDSA_SHA256_P256;
    i2p::data::CryptoKeyType cryptoType = i2p::data::CRYPTO_KEY_TYPE_ELGAMAL;
    std::shared_ptr<ClientDestination> localDestination = nullptr;

    if (isServer) {
        // I2PDK: read this from the config if needed?
        //// mandatory params
        //std::string host = section.second.get<std::string> (I2P_SERVER_TUNNEL_HOST);
        //int port = section.second.get<int> (I2P_SERVER_TUNNEL_PORT);
        //std::string keys = section.second.get<std::string> (I2P_SERVER_TUNNEL_KEYS);
        //// optional params
        //int inPort = section.second.get (I2P_SERVER_TUNNEL_INPORT, 0);
        //std::string accessList = section.second.get (I2P_SERVER_TUNNEL_ACCESS_LIST, "");
        //std::string hostOverride = section.second.get (I2P_SERVER_TUNNEL_HOST_OVERRIDE, "");
        //std::string webircpass = section.second.get<std::string> (I2P_SERVER_TUNNEL_WEBIRC_PASSWORD, "");
        //bool gzip = section.second.get (I2P_SERVER_TUNNEL_GZIP, true);
        //i2p::data::SigningKeyType sigType = section.second.get (I2P_SERVER_TUNNEL_SIGNATURE_TYPE, i2p::data::SIGNING_KEY_TYPE_ECDSA_SHA256_P256);
        //i2p::data::CryptoKeyType cryptoType = section.second.get (I2P_SERVER_TUNNEL_CRYPTO_TYPE, i2p::data::CRYPTO_KEY_TYPE_ELGAMAL);
        //std::string address = section.second.get<std::string> (I2P_SERVER_TUNNEL_ADDRESS, "127.0.0.1");
        //bool isUniqueLocal = section.second.get(I2P_SERVER_TUNNEL_ENABLE_UNIQUE_LOCAL, true);

        //std::string dest;
        //if (!isServer)
        //    dest = section.second.get<std::string>(I2P_CLIENT_TUNNEL_DESTINATION);
        //int port = section.second.get<int>(I2P_CLIENT_TUNNEL_PORT);
        std::string keys = "my-keys.dat"; // this is the hardcoded name for the moment

        // LoadPrivateKeys - loads the keys if file exists, or create a new key (and saves it to disk) if it doesn't - so we're safe. It fails (returns false) only if the keys file is corrupted.
        i2p::data::PrivateKeys k;
        if (!i2p::client::context.LoadPrivateKeys(k, keys, sigType, cryptoType))
            return localDestination; // I2PDK: this shouldn't happen, should fail instead
        localDestination = i2p::client::context.FindLocalDestination(k.GetPublic()->GetIdentHash());
        if (!localDestination)
            localDestination = i2p::client::context.CreateNewLocalDestination(k, isPublic, &options);
            // localDestination = context.CreateNewLocalDestination(k, true, &options);
    } else {
        // I2PDK: read this from the config if needed?
        // mandatory params
        //std::string dest;
        //if (type == I2P_TUNNELS_SECTION_TYPE_CLIENT || type == I2P_TUNNELS_SECTION_TYPE_UDPCLIENT || type == I2P_TUNNELS_SECTION_TYPE_CLIENT_PURE)
        //	dest = section.second.get<std::string>(I2P_CLIENT_TUNNEL_DESTINATION);
        //int port = section.second.get<int>(I2P_CLIENT_TUNNEL_PORT);
        // optional params
        //bool matchTunnels = section.second.get(I2P_CLIENT_TUNNEL_MATCH_TUNNELS, false);
        //std::string keys = section.second.get(I2P_CLIENT_TUNNEL_KEYS, "transient");
        //std::string address = section.second.get (I2P_CLIENT_TUNNEL_ADDRESS, "127.0.0.1");
        //int destinationPort = section.second.get (I2P_CLIENT_TUNNEL_DESTINATION_PORT, 0);
        //i2p::data::SigningKeyType sigType = section.second.get(I2P_CLIENT_TUNNEL_SIGNATURE_TYPE, i2p::data::SIGNING_KEY_TYPE_ECDSA_SHA256_P256);
        //i2p::data::CryptoKeyType cryptoType = section.second.get(I2P_CLIENT_TUNNEL_CRYPTO_TYPE, i2p::data::CRYPTO_KEY_TYPE_ELGAMAL);
        std::string keys = "my-keys.dat"; // this is the hardcoded name for the moment
        //i2p::data::SigningKeyType sigType = i2p::data::SIGNING_KEY_TYPE_ECDSA_SHA256_P256;
        //i2p::data::CryptoKeyType cryptoType = i2p::data::CRYPTO_KEY_TYPE_ELGAMAL;
        bool matchTunnels = false;

        //auto localDestination = std::make_shared<ClientDestination>(keys, isPublic, params);
        //std::shared_ptr<ClientDestination> localDestination = nullptr;
        if (keys.length() > 0)
        {
            i2p::data::PrivateKeys k;
            if (i2p::client::context.LoadPrivateKeys(k, keys, sigType, cryptoType))
            {
                localDestination = i2p::client::context.FindLocalDestination(k.GetPublic()->GetIdentHash());
                if (!localDestination)
                {
                    if (matchTunnels)
                        localDestination = i2p::client::context.CreateNewMatchedTunnelDestination(k, dest, &options);
                    else
                        localDestination = i2p::client::context.CreateNewLocalDestination(k, isPublic, &options);
                }
            }
        }
    }
    return localDestination;
}

std::shared_ptr<ClientDestination> static GetLocalDestination(std::string dest)
{
    return GetLocalDestination(dest, false);
}
std::shared_ptr<ClientDestination> static GetLocalDestination(bool isServer)
{
    std::string dest = "";
    return GetLocalDestination(dest, isServer);
}

// There really should be only one server tunnel (at least one per a specific bind address:port).
// Once created, we're listening to clients and accepting new connections, at which point we're
// creating new server nodes - nodes are per connection, but one tunnel only.
bool ConnectServerTunnel(const CDestination &addrDest, std::shared_ptr<I2PPureServerTunnel>& tunnel, int nTimeout, ServerStreamAcceptedCallback acceptedCallback)
{
    tunnel = nullptr; // invalid tunnel;

    //#[MY-SOCKS-SRV]
    //#type = server
    //#host = 127.0.0.1
    //#port = 6667
    //#keys = my-socks-srv-keys.dat
    // mandatory params
    //std::string host = section.second.get<std::string>(I2P_SERVER_TUNNEL_HOST);
    //int port = section.second.get<int> (I2P_SERVER_TUNNEL_PORT);
    //std::string keys = section.second.get<std::string> (I2P_SERVER_TUNNEL_KEYS);
    //// optional params
    //int inPort = section.second.get (I2P_SERVER_TUNNEL_INPORT, 0);
    //std::string accessList = section.second.get (I2P_SERVER_TUNNEL_ACCESS_LIST, "");
    //std::string hostOverride = section.second.get (I2P_SERVER_TUNNEL_HOST_OVERRIDE, "");
    //std::string webircpass = section.second.get<std::string> (I2P_SERVER_TUNNEL_WEBIRC_PASSWORD, "");
    //bool gzip = section.second.get (I2P_SERVER_TUNNEL_GZIP, true);
    //i2p::data::SigningKeyType sigType = section.second.get (I2P_SERVER_TUNNEL_SIGNATURE_TYPE, i2p::data::SIGNING_KEY_TYPE_ECDSA_SHA256_P256);
    //i2p::data::CryptoKeyType cryptoType = section.second.get (I2P_CLIENT_TUNNEL_CRYPTO_TYPE, i2p::data::CRYPTO_KEY_TYPE_ELGAMAL);
    //std::string address = section.second.get<std::string> (I2P_SERVER_TUNNEL_ADDRESS, "127.0.0.1");
    //bool isUniqueLocal = section.second.get(I2P_SERVER_TUNNEL_ENABLE_UNIQUE_LOCAL, true);

    std::string host = "127.0.0.1";
    int port = 6667;
    std::string keys = "my-keys.dat"; // this is the hardcoded name for the moment, created if none.
    // optional params
    int inPort = 0;
    std::string accessList = "";
    //std::string hostOverride = "";
    //std::string webircpass = "";
    bool gzip = true;
    //i2p::data::SigningKeyType sigType = i2p::data::SIGNING_KEY_TYPE_ECDSA_SHA256_P256;
    //i2p::data::CryptoKeyType cryptoType = i2p::data::CRYPTO_KEY_TYPE_ELGAMAL;
    std::string address = "127.0.0.1";
    bool isUniqueLocal = true;
    std::string name = "MY-SOCKS-SRV";

    std::shared_ptr<ClientDestination> localDestination = GetLocalDestination(true); // server

    std::shared_ptr<I2PPureServerTunnel>  serverTunnel;
    serverTunnel = std::make_shared<I2PPureServerTunnel>(
        name, host, port, localDestination, inPort, gzip, acceptedCallback);

    if (!isUniqueLocal)
    {
        LogPrint(eLogInfo, "Clients: disabling loopback address mapping");
        serverTunnel->SetUniqueLocal(isUniqueLocal);
    }

    if (accessList.length() > 0)
    {
        std::set<i2p::data::IdentHash> idents;
        size_t pos = 0, comma;
        do
        {
            comma = accessList.find(',', pos);
            i2p::data::IdentHash ident;
            ident.FromBase32(accessList.substr(pos, comma != std::string::npos ? comma - pos : std::string::npos));
            idents.insert(ident);
            pos = comma + 1;
        } while (comma != std::string::npos);
        serverTunnel->SetAccessList(idents);
    }
    
    //i2p::client::context.GetServerPureTunnels
    bool success = i2p::client::context.InsertStartServerTunnel(localDestination->GetIdentHash(), inPort, serverTunnel);
    //auto ins = i2p::client::context.GetServerPureTunnels().insert(std::make_pair(
    //    std::make_pair(localDestination->GetIdentHash(), inPort),
    //    serverTunnel));

    if (success) {
        // done inside
        //serverTunnel->Start();
    } else {
        LogPrint(eLogInfo, "ConnectServerTunnel: I2P server tunnel for destination/port ", i2p::client::context.GetAddressBook().ToAddress(localDestination->GetIdentHash()), "/", inPort, " already exists");
    }
    //if (ins.second)
    //{
    //    // this actually calls Accept() and is setting up listening for incoming connections.
    //    serverTunnel->Start();
    //    //numServerTunnels++;
    //} else
    //{
    //    // TODO: update
    //    if (ins.first->second->GetLocalDestination() != serverTunnel->GetLocalDestination())
    //    {
    //        LogPrint(eLogInfo, "Clients: I2P server tunnel destination updated");
    //        ins.first->second->SetLocalDestination(serverTunnel->GetLocalDestination());
    //    }
    //    ins.first->second->isUpdated = true;
    //    LogPrint(eLogInfo, "Clients: I2P server tunnel for destination/port ", i2p::client::context.GetAddressBook().ToAddress(localDestination->GetIdentHash()), "/", inPort, " already exists");
    //}

    // Set to non-blocking
    if (!SetTunnelNonBlocking(serverTunnel, true))
        return error("ConnectClientTunnelDirectly: Setting tunnel to non-blocking failed, error %s\n", "");
    // NetworkErrorString(WSAGetLastError()));

    tunnel = serverTunnel;
    return true;
}

bool static ConnectClientTunnelDirectly(const CDestination& addrConnect, std::shared_ptr<I2PPureClientTunnel>& tunnel, int nTimeout, StreamCreatedCallback streamCreated)
{
    //hSocketRet = INVALID_SOCKET;

    tunnel = nullptr; // invalid tunnel;

    // I2PDK: read this from the config if needed?
    // mandatory params
    //std::string dest = section.second.get<std::string>(I2P_CLIENT_TUNNEL_DESTINATION);
    //int port = section.second.get<int>(I2P_CLIENT_TUNNEL_PORT);
    // optional params
    //std::string address = section.second.get (I2P_CLIENT_TUNNEL_ADDRESS, "127.0.0.1");
    //int destinationPort = section.second.get (I2P_CLIENT_TUNNEL_DESTINATION_PORT, 0);

    // these won't be used inside (no socket created) so it doesn't matter much...
    std::string address = "127.0.0.1";
    int port = 6668;

    std::string dest = addrConnect.ToStringIP();
    int destinationPort = addrConnect.GetPort(); // .ToStringPort(); // 6667; // 0;
    // std::string name = dest;
    std::string name = "MY-SOCKS-CLIENT";

    // I2PDK: 
    // this is always client, server doesn't use ConnectTunnel*()
    auto localDest = i2p::client::context.GetSharedLocalDestination();
    std::shared_ptr<ClientDestination> localDestination = localDest; //nullptr; // GetLocalDestination(false);
    //if (server && !localDestination) return false;

    // can this fail here (like it's not possible to create, wrong address or something? surely)
    auto tun = std::make_shared<I2PPureClientTunnel>(
        name, dest, address, port, localDestination, destinationPort, streamCreated);

    //std::shared_ptr<I2PService> clientTunnel = tun;
    std::shared_ptr<I2PPureClientTunnel> clientTunnel = tun;

    // endpoint here is always the same (as it's basically address:port above), i.e. we can't use
    // that for the map key/index (within the InsertStartClientTunnel). Ideally we should use
    // dest:destPort
    boost::asio::ip::tcp::endpoint clientEndpoint = tun->GetLocalEndpoint();

    // recheck just...
    i2p::data::IdentHash identHash;
    if (!i2p::client::context.GetAddressBook().GetIdentHash(dest, identHash)) {
        LogPrint(eLogWarning, "ReadTunnel: Remote destination ", dest, " not found");
    }
    LogPrint(eLogInfo, "tunnels.created: name: ", name, ", dest:", dest, ", getname:", tun->GetName(), ", hash:", identHash.ToBase32(), ", hash1:", clientTunnel->GetLocalDestination()->GetIdentHash().ToBase32());

    // wrap up...

    // timeout?
    uint32_t timeout = 0; // section.second.get<uint32_t>(I2P_CLIENT_TUNNEL_CONNECT_TIMEOUT, 0);
    if (timeout)
    {
        clientTunnel->SetConnectTimeout(timeout);
        LogPrint(eLogInfo, "Clients: I2P Client tunnel connect timeout set to ", timeout);
    }

    // add to the context list (mimick what's normally done on startup, when reading .conf, adding nodes)
    //auto ins = i2p::client::context.GetClientTunnels().insert(std::make_pair(clientEndpoint, clientTunnel));
    bool success = i2p::client::context.InsertStartClientTunnel(identHash, destinationPort, clientTunnel);
    // bool success = i2p::client::context.InsertStartClientTunnel(clientEndpoint, clientTunnel);

    if (success) {
        // done inside
        //serverTunnel->Start();
    } else {
        LogPrint(eLogInfo, "ConnectClientTunnelDirectly: I2P client tunnel for endpoint ", clientEndpoint, " already exists");
    }
    //if (ins.second)
    //{
    //    clientTunnel->Start();
    //    //numClientTunnels++;
    //} else
    //{
    //    // TODO: update
    //    if (ins.first->second->GetLocalDestination() != clientTunnel->GetLocalDestination())
    //    {
    //        LogPrint(eLogInfo, "Clients: I2P client tunnel destination updated");
    //        ins.first->second->SetLocalDestination(clientTunnel->GetLocalDestination());
    //    }
    //    ins.first->second->isUpdated = true;
    //    LogPrint(eLogInfo, "Clients: I2P client tunnel for endpoint ", clientEndpoint, " already exists");
    //}

    // Set to non-blocking
    //auto tunnel_shared = std::shared_ptr<i2p::client::I2PService>(clientTunnel);
    //if (!SetTunnelNonBlocking(tunnel_shared, true))
    if (!SetTunnelNonBlocking(clientTunnel, true))
        return error("ConnectClientTunnelDirectly: Setting tunnel to non-blocking failed, error %s\n", "");
        // NetworkErrorString(WSAGetLastError()));

    // do the tunnel 'connect', wait for connect or similar, and handler errors
    // ...do this later because of the callbacks
    //AcceptClientTunnel(clientEndpoint, clientTunnel);


    tunnel = clientTunnel;
    return true;

//    struct sockaddr_storage sockaddr;
//    socklen_t len = sizeof(sockaddr);
//    if (!addrConnect.GetSockAddr((struct sockaddr*)&sockaddr, &len)) {
//        LogPrintf("Cannot connect to %s: unsupported network\n", addrConnect.ToString());
//        return false;
//    }
//    SOCKET hSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
//    if (hSocket == INVALID_SOCKET)
//        return false;
//
//#ifdef SO_NOSIGPIPE
//    int set = 1;
//    // Different way of disabling SIGPIPE on BSD
//    setsockopt(hSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(int));
//#endif
//    // Set to non-blocking
//    if (!SetTunnelNonBlocking(clientTunnel, true))
//		return error("ConnectClientTunnelDirectly: Setting tunnel to non-blocking failed, error %s\n", ""); 
//        // NetworkErrorString(WSAGetLastError()));
//
//	// do the tunnel 'connect', wait for connect or similar, and handler errors
//
//    if (connect(hSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR) {
//        int nErr = WSAGetLastError();
//        // WSAEINVAL is here because some legacy version of winsock uses it
//        if (nErr == WSAEINPROGRESS || nErr == WSAEWOULDBLOCK || nErr == WSAEINVAL) {
//            struct timeval timeout = MillisToTimeval(nTimeout);
//            fd_set fdset;
//            FD_ZERO(&fdset);
//            FD_SET(hSocket, &fdset);
//            int nRet = select(hSocket + 1, NULL, &fdset, NULL, &timeout);
//            if (nRet == 0) {
//                LogPrint("net", "connection to %s timeout\n", addrConnect.ToString());
//                CloseTunnel(hSocket);
//                return false;
//            }
//            if (nRet == SOCKET_ERROR) {
//                LogPrintf("select() for %s failed: %s\n", addrConnect.ToString(), NetworkErrorString(WSAGetLastError()));
//                CloseTunnel(hSocket);
//                return false;
//            }
//            socklen_t nRetSize = sizeof(nRet);
//#ifdef WIN32
//            if (getsockopt(hSocket, SOL_SOCKET, SO_ERROR, (char*)(&nRet), &nRetSize) == SOCKET_ERROR)
//#else
//            if (getsockopt(hSocket, SOL_SOCKET, SO_ERROR, &nRet, &nRetSize) == SOCKET_ERROR)
//#endif
//            {
//                LogPrintf("getsockopt() for %s failed: %s\n", addrConnect.ToString(), NetworkErrorString(WSAGetLastError()));
//                CloseTunnel(hSocket);
//                return false;
//            }
//            if (nRet != 0) {
//                LogPrintf("connect() to %s failed after select(): %s\n", addrConnect.ToString(), NetworkErrorString(nRet));
//                CloseTunnel(hSocket);
//                return false;
//            }
//        }
//#ifdef WIN32
//        else if (WSAGetLastError() != WSAEISCONN)
//#else
//        else
//#endif
//        {
//            LogPrintf("connect() to %s failed: %s\n", addrConnect.ToString(), NetworkErrorString(WSAGetLastError()));
//            CloseTunnel(hSocket);
//            return false;
//        }
//    }
//
//	tunnel = clientTunnel;
//    //hSocketRet = hSocket;
//    return true;
}

bool AcceptClientTunnel(boost::asio::ip::tcp::endpoint endpoint, std::shared_ptr<I2PPureClientTunnel> tunnel)
{
    // now from the 'ClientServerTest()'

    std::string name = tunnel->GetName();
    auto& ident = tunnel->GetLocalDestination()->GetIdentHash();

    std::string dest = tunnel->GetDestination();

    i2p::data::IdentHash identHash;
    if (!i2p::client::context.GetAddressBook().GetIdentHash(dest, identHash))
    {
        LogPrint(eLogWarning, "AcceptClientTunnel: Remote destination ", dest, " not found");
    }

    LogPrint(eLogInfo, "AcceptClientTunnel: name: ", name, ", local:", endpoint.address().to_string(), ", port:", endpoint.port(), ", dest:", dest, ", hash:", i2p::client::context.GetAddressBook().ToAddress(ident), ", hash1:", ident.ToBase32());

    // not sure about this one, test it out
    // auto localDest = tunnel->GetLocalDestination();
    // //auto localDest = context.GetSharedLocalDestination();
    auto localDest = i2p::client::context.GetSharedLocalDestination();

    int retries = 0;

    while(!localDest->IsReady () && retries++ < 10) {
        localDest->GetLeaseSet();
        MilliSleep(500);
    }
    retries = 0;

    auto leaseSet = localDest->FindLeaseSet(identHash);

    if (leaseSet) {
        // this is the accept call (mimicking the local socket connect)
        std::static_pointer_cast<i2p::client::TCPIPAcceptor>(tunnel)->InvokeHandleAccept();
        return true;
    }

    // this is fine as well but for now we want to micromanage this
    //std::static_pointer_cast<i2p::client::TCPIPAcceptor>(tunnel)->InvokeHandleAccept();
    //return;

    // we may also skip this and just call InvokeHandleAccept
    // (though timing is slightly different, it's good to request a leaseset asap)
    // thing is that HandleAccept calls CreateStream which calls FindLeaseSet first and if none then
    // RequestDestination. Problem is we don't get the retry (or we'll have to build that into CreateS...)
    std::condition_variable newDataReceived;
    std::mutex newDataReceivedMutex;

    retries = 0;
    // if (!leaseSet)
    while (retries < 5) //true)
    {
        //auto s = GetSharedFromThis();
        LogPrint(eLogInfo, "AcceptClientTunnel: no lease set yet, recreate...");
        std::unique_lock<std::mutex> l(newDataReceivedMutex);

        // std::shared_ptr<I2PPureClientTunnel> tunnel_shared(tunnel.get());
        // assert( tunnel_shared.get() == tunnel.get() );
        // // auto tunnel_shared = std::shared_ptr<I2PPureClientTunnel>(tunnel.get());
        // // std::shared_ptr<I2PPureClientTunnel> tunnel_shared(tunnel.get());
        // // std::shared_ptr<boost::asio::ip::tcp::endpoint> endpoint_shared (endpoint);

        // auto localDest_shared = tunnel_shared->GetLocalDestination();

        // , &endpoint, &localDest_shared, &retries, &tunnel_shared, &tunnel
        // endpoint is ok probably, but not really needed to pass

        // localDest_shared->RequestDestination(identHash,
        //     [&newDataReceived, &leaseSet, &newDataReceivedMutex](std::shared_ptr<i2p::data::LeaseSet> ls)
        localDest->RequestDestination(identHash,
            [&newDataReceived, &leaseSet, &newDataReceivedMutex, &dest](std::shared_ptr<i2p::data::LeaseSet> ls)
        {
            // auto clientEndpoint = tunnel_shared->GetLocalEndpoint();

            // // reworking this to be done here again to avoid faults ('dest' above is not good here)
            // std::string tunnelDest = tunnel_shared->GetDestination();
            // i2p::data::IdentHash identHashDest;
            // if (!i2p::client::context.GetAddressBook().GetIdentHash(tunnelDest, identHashDest))
            // {
            //     LogPrint(eLogWarning, "AcceptClientTunnel: Remote destination ", tunnelDest, " not found");
            // }

            if (!ls) {
                /* still no leaseset found */
                LogPrint(eLogError, "AcceptClientTunnel.RequestDestination: LeaseSet for address ", dest, " not found");

                // // shouldn't be necessary but given code it should be ok to call (w overhead though)
                // localDest_shared->CancelDestinationRequest(identHashDest, false); // don't notify, we know

                // if (retries < 5)
                //     AcceptClientTunnel(clientEndpoint, tunnel_shared, retries + 1);

                // will this not unlock, i.e. wait another 120 secs unnecessarily?
                std::unique_lock<std::mutex> l1(newDataReceivedMutex);
                newDataReceived.notify_all();

                return;
            } else {
                LogPrint(eLogInfo, "AcceptClientTunnel: LeaseSet set!");
                leaseSet = ls;

                // // this is the accept call (mimicking the local socket)
                // std::static_pointer_cast<i2p::client::TCPIPAcceptor>(tunnel_shared)->InvokeHandleAccept();

                std::unique_lock<std::mutex> l1(newDataReceivedMutex);
                newDataReceived.notify_all();
            }
        });
        if (newDataReceived.wait_for(l, std::chrono::seconds(i2p::client::SUBSCRIPTION_REQUEST_TIMEOUT)) == std::cv_status::timeout)
        {
            LogPrint(eLogError, "AcceptClientTunnel: Subscription LeaseSet request for address ", dest, " - timeout expired");
            localDest->CancelDestinationRequest(identHash, false); // don't notify, because we know it already
            // return;
        }

        if (!leaseSet) {
            retries++;
            continue;
        }

        break;

        // we don't want to block though we may need this in some situations
        //if (newDataReceived.wait_for(l, std::chrono::seconds(i2p::client::SUBSCRIPTION_REQUEST_TIMEOUT)) == std::cv_status::timeout)
        //{
        //    LogPrint(eLogError, "ClientServerTest: Subscription LeaseSet request timeout expired");
        //    localDest->CancelDestinationRequest(identHash, false); // don't notify, because we know it already
        //    return;
        //}
    }

    auto leaseSet1 = localDest->FindLeaseSet(identHash);
    if (!leaseSet1) {
        /* still no leaseset found */
        LogPrint(eLogError, "AcceptClientTunnel: LeaseSet for address ", dest, " not found");
        return false;
    }

    if (!leaseSet) {
        LogPrint(eLogError, "AcceptClientTunnel: Fully failed subscription LeaseSet request for address ", dest, "");
        return false;
    }

    // this is the accept call (mimicking the local socket)
    // the cast is no longer needed as we have specific tunnel type here
    std::static_pointer_cast<i2p::client::TCPIPAcceptor>(tunnel)->InvokeHandleAccept();

    return true;

    //auto leaseSet1 = localDest->FindLeaseSet(identHash);
    //if (!leaseSet1) {
    //    //if (!leaseSet) {
    //        /* still no leaseset found */
    //    LogPrint(eLogError, "ClientServerTest: LeaseSet for address ", dest, " not found");
    //    return;
    //}
    //std::static_pointer_cast<i2p::client::TCPIPAcceptor>(tunnel)->InvokeHandleAccept();
    //MakeSureLeaseSet();
    //std::condition_variable newDataReceived;
    //std::mutex newDataReceivedMutex;
    //if (!leaseSet)
    //{
    //    LogPrint(eLogInfo, "ClientServerTest: no lease set, recreate...");
    //    std::unique_lock<std::mutex> l(newDataReceivedMutex);
    //    localDest->RequestDestination(identHash,
    //        [&newDataReceived, &leaseSet, &newDataReceivedMutex, &dest](std::shared_ptr<i2p::data::LeaseSet> ls)
    //    {
    //        if (!ls) {
    //            /* still no leaseset found */
    //            LogPrint(eLogError, "ClientServerTest.RequestDestination: LeaseSet for address ", dest, " not found");
    //            return;
    //        } else {
    //            LogPrint(eLogInfo, "ClientServerTest: LeaseSet set!");
    //            leaseSet = ls;
    //            std::unique_lock<std::mutex> l1(newDataReceivedMutex);
    //            newDataReceived.notify_all();
    //        }
    //    });
    //    if (newDataReceived.wait_for(l, std::chrono::seconds(i2p::client::SUBSCRIPTION_REQUEST_TIMEOUT)) == std::cv_status::timeout)
    //    {
    //        LogPrint(eLogError, "ClientServerTest: Subscription LeaseSet request timeout expired");
    //        localDest->CancelDestinationRequest(identHash, false); // don't notify, because we know it already
    //        return;
    //    }
    //}
    //auto leaseSet1 = localDest->FindLeaseSet(identHash);
    //if (!leaseSet1) {
    //    //if (!leaseSet) {
    //        /* still no leaseset found */
    //    LogPrint(eLogError, "ClientServerTest: LeaseSet for address ", dest, " not found");
    //    return;
    //}
    //std::static_pointer_cast<i2p::client::TCPIPAcceptor>(tunnel)->InvokeHandleAccept();
}

//bool MakeSureLeaseSet(std::shared_ptr<I2PService> tunnel)
//{
//}

bool ConnectClientTunnel(const CDestination &addrDest, std::shared_ptr<I2PPureClientTunnel>& tunnel, int nTimeout, StreamCreatedCallback streamCreated)
{
    //proxyType proxy;
    //if (outProxyConnectionFailed)
    //    *outProxyConnectionFailed = false;

    // I2PDK: this should always fail for i2p, no proxy
    //if (GetProxy(addrDest.GetNetwork(), proxy))
    //    return ConnectThroughProxy(proxy, addrDest.ToStringIP(), addrDest.GetPort(), hSocketRet, nTimeout, outProxyConnectionFailed);
    //else // no proxy needed (none set for target network)

    return ConnectClientTunnelDirectly(addrDest, tunnel, nTimeout, streamCreated);
}

bool ConnectClientTunnelByName(CDestination& addr, std::shared_ptr<I2PPureClientTunnel>& tunnel, const char* pszDest, int portDefault, int nTimeout, StreamCreatedCallback streamCreated)
{
    string strDest;
    int port = portDefault;

    //if (outProxyConnectionFailed)
    //    *outProxyConnectionFailed = false;

    SplitHostPort(string(pszDest), port, strDest);

    //proxyType nameProxy;
    //GetNameProxy(nameProxy);

    CDestination addrResolved(CI2pUrl(strDest, fNameLookup && !HaveNameProxy()), port);
    if (addrResolved.IsValid()) {
        addr = addrResolved;
        return ConnectClientTunnel(addr, tunnel, nTimeout, streamCreated);
    }

    addr = CDestination("0.0.0.0:0");

    if (!HaveNameProxy())
        return false;

    // we have no proxies now w/ i2p (or are handled internally), no sockets, just use i2pd
    return false;
    //return ConnectThroughProxy(nameProxy, strDest, port, hSocketRet, nTimeout, outProxyConnectionFailed);
}

void CDestination::Init()
{
    port = 0;
}

CDestination::CDestination()
{
    Init();
}

CDestination::CDestination(const CI2pUrl& cip, unsigned short portIn) : CI2pUrl(cip), port(portIn)
{
}

CDestination::CDestination(const char* pszIpPort, bool fAllowLookup)
{
    Init();
    CDestination ip;
    // I2PDK: rework this, we should not just lookup but init as well
    if (Lookup(pszIpPort, ip, 0, fAllowLookup))
        *this = ip;
}

CDestination::CDestination(const char* pszIpPort, int portDefault, bool fAllowLookup)
{
    Init();
    CDestination ip;
    if (Lookup(pszIpPort, ip, portDefault, fAllowLookup))
        *this = ip;
}

CDestination::CDestination(const std::string& strIpPort, bool fAllowLookup)
{
    Init();
    CDestination ip;
    if (Lookup(strIpPort.c_str(), ip, 0, fAllowLookup))
        *this = ip;
}

CDestination::CDestination(const std::string& strIpPort, int portDefault, bool fAllowLookup)
{
    Init();
    CDestination ip;
    if (Lookup(strIpPort.c_str(), ip, portDefault, fAllowLookup))
        *this = ip;
}

unsigned short CDestination::GetPort() const
{
    return port;
}

bool operator==(const CDestination& a, const CDestination& b)
{
    return (CI2pUrl)a == (CI2pUrl)b && a.port == b.port;
}

bool operator!=(const CDestination& a, const CDestination& b)
{
    return (CI2pUrl)a != (CI2pUrl)b || a.port != b.port;
}

bool operator<(const CDestination& a, const CDestination& b)
{
    return (CI2pUrl)a < (CI2pUrl)b || ((CI2pUrl)a == (CI2pUrl)b && a.port < b.port);
}

std::vector<unsigned char> CDestination::GetKey() const
{
    // does key have to 18 in len?
    std::vector<unsigned char> vKey;
    vKey.resize(18);
    const char* str = url.c_str();
    memcpy(&vKey[0], str, std::min(16, (int)url.length()));
    vKey[16] = port / 0x100;
    vKey[17] = port & 0x0FF;
    return vKey;
}

std::string CDestination::ToStringPort() const
{
    return strprintf("%u", port);
}

std::string CDestination::ToStringIPPort() const
{
    if (true) { //IsIPv4() || IsTor()) {
        return ToStringIP() + ":" + ToStringPort();
    } else {
        return "[" + ToStringIP() + "]:" + ToStringPort();
    }
}

std::string CDestination::ToString() const
{
    return ToStringIPPort();
}

void CDestination::SetPort(unsigned short portIn)
{
    port = portIn;
}

bool CloseTunnel(std::shared_ptr<I2PPureClientTunnel> tunnel)
{
    if (tunnel->IsStopped()){
        LogPrint(eLogWarning, "CloseTunnel(I2PPureClientTunnel): already stopped?");
        // return;
    }
    // we need to remove before renewing the tunnel, or InsertStartClientTunnel might behave strangely
    // remove from map first then stop? I guess    
    i2p::client::context.RemoveClientTunnel(*tunnel->GetIdentHash(), tunnel->GetDestinationPort(), tunnel);
    tunnel->Stop();
    return true;
}
bool CloseTunnel(std::shared_ptr<I2PPureServerTunnel> tunnel)
{
    if (tunnel->IsStopped()){
        LogPrint(eLogWarning, "CloseTunnel(I2PPureServerTunnel): already stopped?");
        // return;
    }
    // port is a bit iffy here, but this should be ok, we normally pass '0' for inport, which means
    // it's actually using the local ('sockets' port), all should point to the same # anyways.
    // anyhow server tunnel should only be created once for the lifetime of the app.
    i2p::client::context.RemoveServerTunnel(
        tunnel->GetLocalDestination()->GetIdentHash(), 
        tunnel->GetLocalPort(),
        tunnel);
    tunnel->Stop();
    return true;
    // what's the equivalent of the invalid socket check for tunnels here?
//    if (hSocket == INVALID_SOCKET)
//        return false;
//#ifdef WIN32
//    int ret = closesocket(hSocket);
//#else
//    int ret = close(hSocket);
//#endif
//    hSocket = INVALID_SOCKET;
//    return ret != SOCKET_ERROR;
}
bool CloseTunnel(std::shared_ptr<I2PService> tunnel, bool isServer)
{
    if (isServer) {
        auto serverTunnel = std::static_pointer_cast<I2PPureServerTunnel>(tunnel);
        return CloseTunnel(serverTunnel);
    } else {
        auto clientTunnel = std::static_pointer_cast<I2PPureClientTunnel>(tunnel);
        return CloseTunnel(clientTunnel);
    }
}

bool SetTunnelNonBlocking(std::shared_ptr<I2PPureClientTunnel> tunnel, bool fNonBlocking)
{
    return true;
}
bool SetTunnelNonBlocking(std::shared_ptr<I2PPureServerTunnel> tunnel, bool fNonBlocking)
{
//    if (fNonBlocking) {
//#ifdef WIN32
//        u_long nOne = 1;
//        if (ioctlsocket(hSocket, FIONBIO, &nOne) == SOCKET_ERROR) {
//#else
//        int fFlags = fcntl(hSocket, F_GETFL, 0);
//        if (fcntl(hSocket, F_SETFL, fFlags | O_NONBLOCK) == SOCKET_ERROR) {
//#endif
//            CloseTunnel(hSocket);
//            return false;
//        }
//    } else {
//#ifdef WIN32
//        u_long nZero = 0;
//        if (ioctlsocket(hSocket, FIONBIO, &nZero) == SOCKET_ERROR) {
//#else
//        int fFlags = fcntl(hSocket, F_GETFL, 0);
//        if (fcntl(hSocket, F_SETFL, fFlags & ~O_NONBLOCK) == SOCKET_ERROR) {
//#endif
//            CloseTunnel(hSocket);
//            return false;
//        }
//    }

    return true;
}

void CI2pUrl::Init()
{
    //memset(ip, 0, sizeof(ip));
}

void CI2pUrl::SetIP(const CI2pUrl& ipIn)
{
    url = ipIn.url;
    //memcpy(ip, ipIn.ip, sizeof(ip));
}

//static const unsigned char pchOnionCat[] = {0xFD, 0x87, 0xD8, 0x7E, 0xEB, 0x43};

bool CI2pUrl::SetSpecial(const std::string& strName)
{
    //if (strName.size() > 6 && strName.substr(strName.size() - 6, 6) == ".onion") {
    //    std::vector<unsigned char> vchAddr = DecodeBase32(strName.substr(0, strName.size() - 6).c_str());
    //    if (vchAddr.size() != 16 - sizeof(pchOnionCat))
    //        return false;
    //    memcpy(ip, pchOnionCat, sizeof(pchOnionCat));
    //    for (unsigned int i = 0; i < 16 - sizeof(pchOnionCat); i++)
    //        ip[i + sizeof(pchOnionCat)] = vchAddr[i];
    //    return true;
    //}
    return false;
}

CI2pUrl::CI2pUrl()
{
    Init();
}

CI2pUrl::CI2pUrl(const char* pszIp, bool fAllowLookup) 
    : CI2pUrl(std::string(pszIp), fAllowLookup)
{
    //Init();
    //std::vector<CI2pUrl> vIP;
    //if (LookupHost(pszIp, vIP, 1, fAllowLookup))
    //    *this = vIP[0];
}

CI2pUrl::CI2pUrl(const std::string& strIp, bool fAllowLookup)
{
    // maybe we'll need to perform some checks, normalization as well (for *.i2p urls) but none for now.

    //std::string str(pszIp);
    url = strIp;

    //Init();
    //std::vector<CI2pUrl> vIP;
    //if (LookupHost(strIp.c_str(), vIP, 1, fAllowLookup))
    //    *this = vIP[0];
}

//unsigned int CI2pUrl::GetByte(int n) const
//{
//    return ip[15 - n];
//}

bool CI2pUrl::IsLocal() const
{
    // TODO: not sure, no local i2p addresses? or we'll never know I guess
    return false;
    //// IPv4 loopback
    //if (IsIPv4() && (GetByte(3) == 127 || GetByte(3) == 0))
    //    return true;
    //// IPv6 loopback (::1/128)
    //static const unsigned char pchLocal[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    //if (memcmp(ip, pchLocal, 16) == 0)
    //    return true;
    //return false;
}

bool CI2pUrl::IsValid() const
{
    return !url.empty();
    // // TODO: do something here, at least check for .i2p or something (if that makes sense)
    // return true;

    //// Cleanup 3-byte shifted addresses caused by garbage in size field
    //// of addr messages from versions before 0.2.9 checksum.
    //// Two consecutive addr messages look like this:
    //// header20 vectorlen3 addr26 addr26 addr26 header20 vectorlen3 addr26 addr26 addr26...
    //// so if the first length field is garbled, it reads the second batch
    //// of addr misaligned by 3 bytes.
    //if (memcmp(ip, pchIPv4 + 3, sizeof(pchIPv4) - 3) == 0)
    //    return false;
    //// unspecified IPv6 address (::/128)
    //unsigned char ipNone[16] = {};
    //if (memcmp(ip, ipNone, 16) == 0)
    //    return false;
    //// documentation IPv6 address
    //if (IsRFC3849())
    //    return false;
    //if (IsIPv4()) {
    //    // INADDR_NONE
    //    uint32_t ipNone = INADDR_NONE;
    //    if (memcmp(ip + 12, &ipNone, 4) == 0)
    //        return false;
    //    // 0
    //    ipNone = 0;
    //    if (memcmp(ip + 12, &ipNone, 4) == 0)
    //        return false;
    //}
    //return true;
}

bool CI2pUrl::IsRoutable() const
{
    return IsValid(); // && !(IsRFC1918() || IsRFC2544() || IsRFC3927() || IsRFC4862() || IsRFC6598() || IsRFC5737() || (IsRFC4193() && !IsTor()) || IsRFC4843() || IsLocal());
}

enum Network CI2pUrl::GetNetwork() const
{
    // I2PDK: return NET_I2PD;
    if (!IsRoutable())
        return NET_UNROUTABLE;
    return NET_I2P;

    //if (IsIPv4())
    //    return NET_IPV4;
    //if (IsTor())
    //    return NET_TOR;
    //return NET_IPV6;
}

std::string CI2pUrl::ToStringIP() const
{
    return url;
    //if (IsTor())
    //    return EncodeBase32(&ip[6], 10) + ".onion";
    //CService serv(*this, 0);
    //struct sockaddr_storage sockaddr;
    //socklen_t socklen = sizeof(sockaddr);
    //if (serv.GetSockAddr((struct sockaddr*)&sockaddr, &socklen)) {
    //    char name[1025] = "";
    //    if (!getnameinfo((const struct sockaddr*)&sockaddr, socklen, name, sizeof(name), NULL, 0, NI_NUMERICHOST))
    //        return std::string(name);
    //}
    //if (IsIPv4())
    //    return strprintf("%u.%u.%u.%u", GetByte(3), GetByte(2), GetByte(1), GetByte(0));
    //else
    //    return strprintf("%x:%x:%x:%x:%x:%x:%x:%x",
    //        GetByte(15) << 8 | GetByte(14), GetByte(13) << 8 | GetByte(12),
    //        GetByte(11) << 8 | GetByte(10), GetByte(9) << 8 | GetByte(8),
    //        GetByte(7) << 8 | GetByte(6), GetByte(5) << 8 | GetByte(4),
    //        GetByte(3) << 8 | GetByte(2), GetByte(1) << 8 | GetByte(0));
}

std::string CI2pUrl::ToString() const
{
    return ToStringIP();
}

bool operator==(const CI2pUrl& a, const CI2pUrl& b)
{
    return a.url == b.url;
    //return (memcmp(a.url.c_str(), b.url.c_str(), 16) == 0);
}

bool operator!=(const CI2pUrl& a, const CI2pUrl& b)
{
    return a.url != b.url;
    //return (memcmp(a.ip, b.ip, 16) != 0);
}

bool operator<(const CI2pUrl& a, const CI2pUrl& b)
{
    return a.url < b.url;
    //return (memcmp(a.ip, b.ip, 16) < 0);
}

// get canonical identifier of an address' group
// no two connections will be attempted to addresses with the same group
std::vector<unsigned char> CI2pUrl::GetGroup() const
{
    std::vector<unsigned char> vchRet;

    vchRet.push_back(NET_I2P);
    return vchRet;

    //int nClass = NET_IPV6;
    //int nStartByte = 0;
    //int nBits = 16;

    //// all local addresses belong to the same group
    //if (IsLocal()) {
    //    nClass = 255;
    //    nBits = 0;
    //}

    //// all unroutable addresses belong to the same group
    //if (!IsRoutable()) {
    //    nClass = NET_UNROUTABLE;
    //    nBits = 0;
    //}

    //nClass = NET_I2P;
    //nStartByte = 6;
    //nBits = 4;

    // I2PDK: not sure what to do w/ this, we should be something like NET_I2P

    //int nClass = NET_IPV6;
    //int nStartByte = 0;
    //int nBits = 16;
    //// all local addresses belong to the same group
    //if (IsLocal()) {
    //    nClass = 255;
    //    nBits = 0;
    //}
    //// all unroutable addresses belong to the same group
    //if (!IsRoutable()) {
    //    nClass = NET_UNROUTABLE;
    //    nBits = 0;
    //}
    //// for IPv4 addresses, '1' + the 16 higher-order bits of the IP
    //// includes mapped IPv4, SIIT translated IPv4, and the well-known prefix
    //else if (IsIPv4() || IsRFC6145() || IsRFC6052()) {
    //    nClass = NET_IPV4;
    //    nStartByte = 12;
    //}
    //// for 6to4 tunnelled addresses, use the encapsulated IPv4 address
    //else if (IsRFC3964()) {
    //    nClass = NET_IPV4;
    //    nStartByte = 2;
    //}
    //// for Teredo-tunnelled IPv6 addresses, use the encapsulated IPv4 address
    //else if (IsRFC4380()) {
    //    vchRet.push_back(NET_IPV4);
    //    vchRet.push_back(GetByte(3) ^ 0xFF);
    //    vchRet.push_back(GetByte(2) ^ 0xFF);
    //    return vchRet;
    //} else if (IsTor()) {
    //    nClass = NET_TOR;
    //    nStartByte = 6;
    //    nBits = 4;
    //}
    //// for he.net, use /36 groups
    //else if (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x04 && GetByte(12) == 0x70)
    //    nBits = 36;
    //// for the rest of the IPv6 network, use /32 groups
    //else
    //    nBits = 32;
    //vchRet.push_back(nClass);
    //while (nBits >= 8) {
    //    vchRet.push_back(GetByte(15 - nStartByte));
    //    nStartByte++;
    //    nBits -= 8;
    //}
    //if (nBits > 0)
    //    vchRet.push_back(GetByte(15 - nStartByte) | ((1 << nBits) - 1));
    //return vchRet;
}

uint64_t CI2pUrl::GetHash() const
{
    int len = url.length();
    const char* address = url.c_str();

    uint256 hash;
    if (url.empty()) 
        hash = Hash(address, address);
    else
        // should be len, the the above fits as well, check it out but who cares
        hash = Hash(&address[0], &address[len - 1]);

    uint64_t nRet;
    memcpy(&nRet, &hash, sizeof(nRet));
    return nRet;
}

// private extensions to enum Network, only returned by GetExtNetwork,
// and only used in GetReachabilityFrom
static const int NET_UNKNOWN = NET_MAX + 0;
static const int NET_TEREDO = NET_MAX + 1;
int static GetExtNetwork(const CI2pUrl* addr)
{
    if (addr == NULL)
        return NET_UNKNOWN;
    //if (addr->IsRFC4380())
    //    return NET_TEREDO;
    return addr->GetNetwork();
}

/** Calculates a metric for how reachable (*this) is from a given partner */
int CI2pUrl::GetReachabilityFrom(const CI2pUrl* paddrPartner) const
{
    enum Reachability {
        REACH_UNREACHABLE,
        REACH_DEFAULT,
        REACH_TEREDO,
        REACH_IPV6_WEAK,
        REACH_IPV4,
        REACH_IPV6_STRONG,
        REACH_PRIVATE
    };
    return REACH_DEFAULT; // REACH_PRIVATE

    //if (!IsRoutable())
    //    return REACH_UNREACHABLE;
    //int ourNet = GetExtNetwork(this);
    //int theirNet = GetExtNetwork(paddrPartner);
    //bool fTunnel = IsRFC3964() || IsRFC6052() || IsRFC6145();
    //switch (theirNet) {
    //case NET_IPV4:
    //    switch (ourNet) {
    //    default:
    //        return REACH_DEFAULT;
    //    case NET_IPV4:
    //        return REACH_IPV4;
    //    }
    //case NET_IPV6:
    //    switch (ourNet) {
    //    default:
    //        return REACH_DEFAULT;
    //    case NET_TEREDO:
    //        return REACH_TEREDO;
    //    case NET_IPV4:
    //        return REACH_IPV4;
    //    case NET_IPV6:
    //        return fTunnel ? REACH_IPV6_WEAK : REACH_IPV6_STRONG; // only prefer giving our IPv6 address if it's not tunnelled
    //    }
    //case NET_TOR:
    //    switch (ourNet) {
    //    default:
    //        return REACH_DEFAULT;
    //    case NET_IPV4:
    //        return REACH_IPV4; // Tor users can connect to IPv4 as well
    //    case NET_TOR:
    //        return REACH_PRIVATE;
    //    }
    //case NET_TEREDO:
    //    switch (ourNet) {
    //    default:
    //        return REACH_DEFAULT;
    //    case NET_TEREDO:
    //        return REACH_TEREDO;
    //    case NET_IPV6:
    //        return REACH_IPV6_WEAK;
    //    case NET_IPV4:
    //        return REACH_IPV4;
    //    }
    //case NET_UNKNOWN:
    //case NET_UNROUTABLE:
    //default:
    //    switch (ourNet) {
    //    default:
    //        return REACH_DEFAULT;
    //    case NET_TEREDO:
    //        return REACH_TEREDO;
    //    case NET_IPV6:
    //        return REACH_IPV6_WEAK;
    //    case NET_IPV4:
    //        return REACH_IPV4;
    //    case NET_TOR:
    //        return REACH_PRIVATE; // either from Tor, or don't care about our address
    //    }
    //}
}

CI2pSubNet::CI2pSubNet() : valid(false)
{
    memset(netmask, 0, sizeof(netmask));
}

CI2pSubNet::CI2pSubNet(const std::string& strSubnet, bool fAllowLookup)
{
    size_t slash = strSubnet.find_last_of('/');
    std::vector<CI2pUrl> vIP;

    valid = true;
    // Default to /32 (IPv4) or /128 (IPv6), i.e. match single address
    memset(netmask, 255, sizeof(netmask));

    std::string strAddress = strSubnet.substr(0, slash);
    // in our case this should return a match strSubnet == CI2pUrl.url
    if (LookupHost(strAddress.c_str(), vIP, 1, fAllowLookup)) {
        network = vIP[0];
        //if (slash != strSubnet.npos) {
        //    std::string strNetmask = strSubnet.substr(slash + 1);
        //    int32_t n;
        //    // IPv4 addresses start at offset 12, and first 12 bytes must match, so just offset n
        //    const int astartofs = network.IsIPv4() ? 12 : 0;
        //    if (ParseInt32(strNetmask, &n)) // If valid number, assume /24 symtex
        //    {
        //        if (n >= 0 && n <= (128 - astartofs * 8)) // Only valid if in range of bits of address
        //        {
        //            n += astartofs * 8;
        //            // Clear bits [n..127]
        //            for (; n < 128; ++n)
        //                netmask[n >> 3] &= ~(1 << (7 - (n & 7)));
        //        } else {
        //            valid = false;
        //        }
        //    } else // If not a valid number, try full netmask syntax
        //    {
        //        if (LookupHost(strNetmask.c_str(), vIP, 1, false)) // Never allow lookup for netmask
        //        {
        //            // Copy only the *last* four bytes in case of IPv4, the rest of the mask should stay 1's as
        //            // we don't want pchIPv4 to be part of the mask.
        //            for (int x = astartofs; x < 16; ++x)
        //                netmask[x] = vIP[0].ip[x];
        //        } else {
        //            valid = false;
        //        }
        //    }
        //}
    } else {
        valid = false;
    }

    //// Normalize network according to netmask
    //for (int x = 0; x < 16; ++x)
    //    network.ip[x] &= netmask[x];
}

CI2pSubNet::CI2pSubNet(const CI2pUrl &addr) :
    valid(addr.IsValid())
{
    memset(netmask, 255, sizeof(netmask));
    network = addr;
}

bool CI2pSubNet::Match(const CI2pUrl& addr) const
{
    if (!valid || !addr.IsValid())
        return false;
    return addr.url == network.url;

    //for (int x = 0; x < 16; ++x)
    //    if ((addr.ip[x] & netmask[x]) != network.ip[x])
    //        return false;
    //return true;
}

//// I2PDK: just to compile and to keep it there for the moment, as we're not sure if it may be relevant.
//bool CI2pSubNet::Match(const CI2pUrl& addr) const
//{
//    return false;
//}

static inline int NetmaskBits(uint8_t x)
{
    switch (x) {
    case 0x00: return 0; break;
    case 0x80: return 1; break;
    case 0xc0: return 2; break;
    case 0xe0: return 3; break;
    case 0xf0: return 4; break;
    case 0xf8: return 5; break;
    case 0xfc: return 6; break;
    case 0xfe: return 7; break;
    case 0xff: return 8; break;
    default: return -1; break;
    }
}

std::string CI2pSubNet::ToString() const
{
    return network.ToString();

    ///* Parse binary 1{n}0{N-n} to see if mask can be represented as /n */
    //int cidr = 0;
    //bool valid_cidr = true;
    //int n = network.IsIPv4() ? 12 : 0;
    //for (; n < 16 && netmask[n] == 0xff; ++n)
    //    cidr += 8;
    //if (n < 16) {
    //    int bits = NetmaskBits(netmask[n]);
    //    if (bits < 0)
    //        valid_cidr = false;
    //    else
    //        cidr += bits;
    //    ++n;
    //}
    //for (; n < 16 && valid_cidr; ++n)
    //    if (netmask[n] != 0x00)
    //        valid_cidr = false;

    ///* Format output */
    //std::string strNetmask;
    //if (valid_cidr) {
    //    strNetmask = strprintf("%u", cidr);
    //} else {
    //    if (network.IsIPv4())
    //        strNetmask = strprintf("%u.%u.%u.%u", netmask[12], netmask[13], netmask[14], netmask[15]);
    //    else
    //        strNetmask = strprintf("%x:%x:%x:%x:%x:%x:%x:%x",
    //            netmask[0] << 8 | netmask[1], netmask[2] << 8 | netmask[3],
    //            netmask[4] << 8 | netmask[5], netmask[6] << 8 | netmask[7],
    //            netmask[8] << 8 | netmask[9], netmask[10] << 8 | netmask[11],
    //            netmask[12] << 8 | netmask[13], netmask[14] << 8 | netmask[15]);
    //}

    //return network.ToString() + "/" + strNetmask;
}

bool CI2pSubNet::IsValid() const
{
    return valid;
}

bool operator==(const CI2pSubNet& a, const CI2pSubNet& b)
{
    return a.valid == b.valid && a.network == b.network && !memcmp(a.netmask, b.netmask, 16);
}

bool operator!=(const CI2pSubNet& a, const CI2pSubNet& b)
{
    return !(a == b);
}

bool operator<(const CI2pSubNet& a, const CI2pSubNet& b)
{
    return (a.network < b.network || (a.network == b.network && memcmp(a.netmask, b.netmask, 16) < 0));
}


