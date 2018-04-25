// Copyright (c) 2018 The COLX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONTEXT_H
#define BITCOIN_CONTEXT_H

class CContext;

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
     * Return availability of the update on the server.
     * @return true - available, false - not available
     */
    bool IsUpdateAvailable() const;

    /**
     * Set availability of the update on the server.
     * @param available true - available, false - not available
     */
    void SetUpdateAvailable(bool available);

private:
    CContext(const CContext&);
    CContext& operator=(const CContext&);

private:
    bool bUpdateAvailable_ = false;
};

#endif // BITCOIN_CONTEXT_H
