// Copyright (c) 2018 The COLX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CURL_H
#define BITCOIN_CURL_H

#include <string>

typedef std::string CUrl;

/**
 * Extract URL to redirect to.
 * @param[url] input http(s) address
 * @param[redirect] redirect http(s) address
 * @param[out] error error description on fail
 * @return true - success, false - fail
 */
bool CURLGetRedirect(const CUrl& url, CUrl& redirect, std::string& error);

#endif // BITCOIN_CURL_H
