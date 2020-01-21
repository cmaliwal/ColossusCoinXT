// Copyright (c) 2018 The COLX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONTEXT_H
#define BITCOIN_CONTEXT_H

#include <set>
#include <string>
#include <memory>
#include <vector>

class CContext;
class BootstrapModel;
class AutoUpdateModel;

typedef std::shared_ptr<BootstrapModel> BootstrapModelPtr;
typedef std::shared_ptr<AutoUpdateModel> AutoUpdateModelPtr;

/**
 * Create and initialize unique global application context object.
 * Must be called from the main thread before any other thread started.
 * @throw runtime_error if context has already initialzied or any error occurs
 */
void CreateContext();

/**
 * Free resources allocated for context object.
 * Must be called from the main thread after all other threads completed.
 * @throw no exceptions
 */
void ReleaseContext();

/**
 * Returns unique application context object.
 * @throw runtime_error if context is not initialized
 * @return context reference
 */
CContext& GetContext();

/**
 * Context scope initializer. Automatically create/release context.
 */
struct ContextScopeInit
{
    ContextScopeInit() { CreateContext(); }

    ~ContextScopeInit() { ReleaseContext(); }

private:
    ContextScopeInit(const ContextScopeInit&);
    ContextScopeInit& operator=(const ContextScopeInit&);
};

/**
 * Unique global object that represents application context.
 * Initialized at the application startup and destroyed just before return from main.
 */
class CContext
{
public:
    ~CContext();

    /**
     * Return startup time.
     */
    int64_t GetStartupTime() const;

    /**
     * Adjust startup time.
     */
    void SetStartupTime(int64_t nTime);

    /**
     * Return unique instance of the bootstrap model.
     * Model is created if not exists.
     */
    BootstrapModelPtr GetBootstrapModel();

    /**
     * Return unique instance of the autoupdate model.
     * Model is created if not exists.
     */
    AutoUpdateModelPtr GetAutoUpdateModel();

    /**
     * Add ColossusXT address to the ban list.
     * @param mempool list of ColossusXT address for mempool ban
     * @param consensus list of ColossusXT address for consensus ban
     */
    void AddAddressToBan(
            const std::vector<std::string>& mempool,
            const std::vector<std::string>& consensus);

    /**
     * Return true if there is at least one address for banning.
     */
    bool MempoolBanActive() const;

    /**
     * Return true if given address is banned.
     * @param addr ColossusXT address in base58 format
     */
    bool MempoolBanActive(const std::string& addr) const;

    /**
     * Return true if there is at least one address for banning.
     */
    bool ConsensusBanActive() const;

    /**
     * Return true if given address is banned.
     * @param addr ColossusXT address in base58 format
     */
    bool ConsensusBanActive(const std::string& addr) const;

private:
    CContext();
    CContext(const CContext&);
    CContext& operator=(const CContext&);
    friend void CreateContext();

private:
    int64_t nStartupTime_ = 0;
    BootstrapModelPtr bootstrapModel_;
    AutoUpdateModelPtr autoupdateModel_;
    std::set<std::string> banAddrMempool_;
    std::set<std::string> banAddrConsensus_;
};

#endif // BITCOIN_CONTEXT_H
