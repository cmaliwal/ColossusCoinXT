// Copyright (c) 2018 The COLX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "context.h"

#include <memory>
#include <exception>

using namespace std;

static unique_ptr<CContext> context_;

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

CContext::CContext() {}

CContext::~CContext() {}

bool CContext::IsUpdateAvailable() const
{
    return bUpdateAvailable_;
}

void CContext::SetUpdateAvailable(bool available)
{
    bUpdateAvailable_ = available;
}
