// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NETDESTINATION_H
#define BITCOIN_NETDESTINATION_H

#if defined(HAVE_CONFIG_H)
#include "config/colx-config.h"
#endif

#include "compat.h"
#include "serialize.h"

#include "netbase.h"

#include <I2PService.h>
#include <I2PTunnel.h>
#include <I2PPureTunnel.h>

#include <stdint.h>
#include <string>
#include <vector>

#include <boost/asio.hpp>

//extern int nConnectTimeout;
//extern bool fNameLookup;

#ifdef WIN32
// In MSVC, this is defined as a macro, undefine it to prevent a compile and link error
#undef SetPort
#endif

/** IP address (IPv6, or IPv4 using mapped IPv6 range (::FFFF:0:0/96)) */
class CI2pUrl
{
protected:
    std::string url; // make it char[]

public:
    CI2pUrl();
    explicit CI2pUrl(const char* pszIp, bool fAllowLookup = false);
    explicit CI2pUrl(const std::string& strIp, bool fAllowLookup = false);
    void Init();
    void SetIP(const CI2pUrl& url);

    //void SetRaw(Network network, const uint8_t* data);

    bool SetSpecial(const std::string& strName); // for Tor addresses
    bool IsLocal() const;
    bool IsRoutable() const;
    bool IsValid() const;
    enum Network GetNetwork() const;
    std::string ToString() const;
    std::string ToStringIP() const;
    //unsigned int GetByte(int n) const;
    uint64_t GetHash() const;
    //bool GetInAddr(struct in_addr* pipv4Addr) const;
    std::vector<unsigned char> GetGroup() const;
    int GetReachabilityFrom(const CI2pUrl* paddrPartner = NULL) const;

    friend bool operator==(const CI2pUrl& a, const CI2pUrl& b);
    friend bool operator!=(const CI2pUrl& a, const CI2pUrl& b);
    friend bool operator<(const CI2pUrl& a, const CI2pUrl& b);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(FLATDATA(url));
    }

    friend class CI2pSubNet;
};

class CI2pSubNet
{
protected:
    /// Network (base) address
    CI2pUrl network;
    /// Netmask, in network byte order
    uint8_t netmask[16];
    /// Is this value valid? (only used to signal parse errors)
    bool valid;

public:
    CI2pSubNet();
    explicit CI2pSubNet(const std::string& strSubnet, bool fAllowLookup = false);

    //constructor for single ip subnet (<ipv4>/32 or <ipv6>/128)
    explicit CI2pSubNet(const CI2pUrl &addr);

    bool Match(const CI2pUrl& addr) const;

    std::string ToString() const;
    bool IsValid() const;

    friend bool operator==(const CI2pSubNet& a, const CI2pSubNet& b);
    friend bool operator!=(const CI2pSubNet& a, const CI2pSubNet& b);
    friend bool operator<(const CI2pSubNet& a, const CI2pSubNet& b);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(network);
        READWRITE(FLATDATA(netmask));
        READWRITE(FLATDATA(valid));
    }
};

/** A combination of an i2p type address (CI2pUrl) and a (TCP) port */
// question: is there a difference if we use a registered uri/domain address vs the temp ones?
class CDestination : public CI2pUrl
{
protected:
    unsigned short port; // host order

public:
    CDestination();
    CDestination(const CI2pUrl& ip, unsigned short port);
    explicit CDestination(const char* pszIpPort, int portDefault, bool fAllowLookup = false);
    explicit CDestination(const char* pszIpPort, bool fAllowLookup = false);
    explicit CDestination(const std::string& strIpPort, int portDefault, bool fAllowLookup = false);
    explicit CDestination(const std::string& strIpPort, bool fAllowLookup = false);
    void Init();
    void SetPort(unsigned short portIn);
    unsigned short GetPort() const;
    friend bool operator==(const CDestination& a, const CDestination& b);
    friend bool operator!=(const CDestination& a, const CDestination& b);
    friend bool operator<(const CDestination& a, const CDestination& b);
    std::vector<unsigned char> GetKey() const;
    std::string ToString() const;
    std::string ToStringPort() const;
    std::string ToStringIPPort() const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        //READWRITE(FLATDATA(url));
        //READWRITE(url);
        READWRITE(LIMITED_STRING(url, 1024));
        unsigned short portN = htons(port);
        READWRITE(portN);
        if (ser_action.ForRead())
            port = ntohs(portN);
    }
};

bool IsProxy(const CI2pUrl& addr);


bool LookupHost(const char* pszName, std::vector<CI2pUrl>& vIP, unsigned int nMaxSolutions = 0, bool fAllowLookup = true);
bool Lookup(const char* pszName, CDestination& addr, int portDefault = 0, bool fAllowLookup = true);
bool Lookup(const char* pszName, std::vector<CDestination>& vAddr, int portDefault = 0, bool fAllowLookup = true, unsigned int nMaxSolutions = 0);
bool LookupNumeric(const char* pszName, CDestination& addr, int portDefault = 0);

bool ConnectClientTunnel(const CDestination& addr, std::shared_ptr<i2p::client::I2PPureClientTunnel>& tunnel, int nTimeout, i2p::client::StreamCreatedCallback streamCreated);
bool ConnectClientTunnelByName(CDestination& addr, std::shared_ptr<i2p::client::I2PPureClientTunnel>& tunnel, const char* pszDest, int portDefault, int nTimeout, i2p::client::StreamCreatedCallback streamCreated);
bool AcceptClientTunnel(boost::asio::ip::tcp::endpoint endpoint, std::shared_ptr<i2p::client::I2PPureClientTunnel> tunnel, int retries = 0);

bool ConnectServerTunnel(const CDestination &addrDest, std::shared_ptr<i2p::client::I2PPureServerTunnel>& tunnel, int nTimeout, i2p::client::ServerStreamAcceptedCallback acceptedCallback);

/** Close tunnel and set hSocket to INVALID_SOCKET */
bool CloseTunnel(std::shared_ptr<i2p::client::I2PPureServerTunnel> tunnel);
bool CloseTunnel(std::shared_ptr<i2p::client::I2PPureClientTunnel> tunnel);
bool CloseTunnel(std::shared_ptr<i2p::client::I2PService> tunnel, bool isServer);

/** Disable or enable blocking-mode for a socket */
bool SetTunnelNonBlocking(std::shared_ptr<i2p::client::I2PPureClientTunnel> tunnel, bool fNonBlocking);
bool SetTunnelNonBlocking(std::shared_ptr<i2p::client::I2PPureServerTunnel> tunnel, bool fNonBlocking);

#endif // BITCOIN_NETDESTINATION_H
