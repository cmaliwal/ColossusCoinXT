// Copyright (c) 2018 The COLX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONTEXT_H
#define BITCOIN_CONTEXT_H

#include <string>
#include <memory>

class CContext;
class BootstrapModel;

typedef std::shared_ptr<BootstrapModel> BootstrapModelPtr;

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
    CContext();

    ~CContext();

    /**
     * Set availability of the update on the server.
     * @param available true - available, false - not available
     * @param urlTag base url of the release folder on the server
     * @param urlFile url of the release file on the server
     */
    void SetUpdateAvailable(bool available, const std::string& urlTag, const std::string& urlFile);

    /**
     * Return availability of the update on the server.
     * @return true - available, false - not available
     */
    bool IsUpdateAvailable() const;

    /**
     * Return base url of the release folder on the server.
     */
    std::string GetUpdateUrlTag() const;

    /**
     * Return url of the release file on the server.
     */
    std::string GetUpdateUrlFile() const;

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

private:
    CContext(const CContext&);
    CContext& operator=(const CContext&);

private:
    bool bUpdateAvailable_ = false;
    std::string sUpdateUrlTag_;
    std::string sUpdateUrlFile_;
    int64_t nStartupTime_ = 0;
    BootstrapModelPtr bootstrapModel_;
};

#endif // BITCOIN_CONTEXT_H
