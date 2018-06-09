// Copyright (c) 2018 The COLX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "curl.h"
#include "util.h"
#include "clientversion.h"

#include <stdlib.h>
#include <curl/curl.h>

using namespace std;

//
// private section
//

static void CurlGlobalInit()
{
    static bool initialized = false;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }
}

struct CurlScopeInit
{
    CurlScopeInit() {
        CurlGlobalInit();
        curl_ = curl_easy_init();
    }

    ~CurlScopeInit() {
        if (curl_) curl_easy_cleanup(curl_);
    }

    inline CURL* instance() { return curl_; }

    inline operator CURL*() { return curl_; }

private:
    CurlScopeInit(const CurlScopeInit&);
    CurlScopeInit& operator=(const CurlScopeInit&);

private:
    CURL *curl_;
};

struct MemoryBuffer
{
  char* memory;
  size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryBuffer *mem = (struct MemoryBuffer*)userp;

    mem->memory = (char*)realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL) {
        /* out of memory! */
        error("%s: not enough memory (realloc returned NULL)\n", __func__);
        return 0;
    } else {
        memcpy(&(mem->memory[mem->size]), contents, realsize);
        mem->size += realsize;
        mem->memory[mem->size] = 0;
        return realsize;
    }
}

//
// public API
//

bool CURLGetRedirect(const CUrl& url, CUrl& redirect, string& error)
{
    CurlScopeInit curl;
    CURLcode res;
    char *location;
    long response_code;

    redirect.clear();
    error.clear();

    if (url.empty()) {
        error = "url is empty";
        return false;
    }

    if (!curl.instance()) {
        error = "curl init failed";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    /* disable peer and host verification because of issues on Mac and Win */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        error = strprintf("curl_easy_perform failed: %s", curl_easy_strerror(res));
        return false;
    }

    res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if ((res == CURLE_OK) && ((response_code / 100) != 3)) {
        /* a redirect implies a 3xx response code */
        error = "Not a redirect";
        return false;
    }
    else {
        res = curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
        if ((res == CURLE_OK) && location) {
            /* This is the new absolute URL that you could redirect to, even if
             * the Location: response header may have been a relative URL. */
            redirect = location;
            return true;
        }
        else {
            error = strprintf("curl_easy_getinfo failed: %s", curl_easy_strerror(res));
            return false;
        }
    }
}


bool CURLDownloadToMem(const CUrl& url, string& buff, string& error)
{
    CurlScopeInit curl;
    CURLcode res;
    struct MemoryBuffer chunk;

    buff.clear();
    error.clear();

    if (url.empty()) {
        error = "url is empty";
        return false;
    }

    if (!curl.instance()) {
        error = "curl init failed";
        return false;
    }

    chunk.memory = (char*)malloc(1);  /* will be grown as needed by the realloc above */
    chunk.size = 0;    /* no data at this point */

    /* specify URL to get */
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    /* send all data to this function  */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);

    /* we pass our 'chunk' struct to the callback function */
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    /* some servers don't like requests that are made without a user-agent */
    const string agent = FormatFullVersion();
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());

    /* disable peer and host verification because of issues on Mac and Win */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    /* get it! */
    res = curl_easy_perform(curl);

    /* check for errors */
    if (res != CURLE_OK) {
        error = strprintf("curl_easy_perform failed: %s", curl_easy_strerror(res));
    } else {
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code / 100 != 2) {
            error = strprintf("response code is not OK: %d", response_code);
            DebugPrintf("%s: response code=%d, response content=%s", __func__, response_code, string(chunk.memory, chunk.size));
        } else {
            /*
            * Now, our chunk.memory points to a memory block that is chunk.size
            * bytes big and contains the remote file.
            */
            DebugPrintf("%s: %lu bytes retrieved\n", __func__, (long)chunk.size);
            buff.assign(chunk.memory, chunk.size);
        }
    }

    free(chunk.memory);
    return !buff.empty();
}

bool CURLDownloadToFile(const CUrl& url, const string& path, ProgressReport callback, string& error)
{
    error = "not implemented";
    return false;
}
