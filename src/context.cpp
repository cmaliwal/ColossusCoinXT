// Copyright (c) 2018 The COLX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "context.h"
#include "sync.h"
#include "timedata.h"
#include "bootstrapmodel.h"

#include <memory>
#include <stdexcept>

using namespace std;

static unique_ptr<CContext> context_;
static CCriticalSection csUpdate_;

void CreateContext()
{
    if (context_)
        throw runtime_error("context has already been initialized, revise your code");
    else
        context_.reset(new CContext);
}

void ReleaseContext()
{
    context_.reset();
}

CContext& GetContext()
{
    if (!context_)
        throw runtime_error("context is not initialized");
    else
        return *context_;
}

CContext::CContext()
{
    nStartupTime_ = GetAdjustedTime();
}

CContext::~CContext() {}

void CContext::SetUpdateAvailable(bool available, const string& urlTag, const string& urlFile)
{
    LOCK(csUpdate_);

    bUpdateAvailable_ = available;
    sUpdateUrlTag_ = urlTag;
    sUpdateUrlFile_ = urlFile;
}

bool CContext::IsUpdateAvailable() const
{
    LOCK(csUpdate_);
    return bUpdateAvailable_;
}

std::string CContext::GetUpdateUrlTag() const
{
    LOCK(csUpdate_);
    return sUpdateUrlTag_;
}

std::string CContext::GetUpdateUrlFile() const
{
    LOCK(csUpdate_);
    return sUpdateUrlFile_;
}

int64_t CContext::GetStartupTime() const
{
    return nStartupTime_;
}

void CContext::SetStartupTime(int64_t nTime)
{
    nStartupTime_ = nTime;
}

BootstrapModelPtr CContext::GetBootstrapModel()
{
    if (!bootstrapModel_)
        bootstrapModel_.reset(new BootstrapModel);

    return bootstrapModel_;
}
