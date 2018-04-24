// Copyright (c) 2018 The COLX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "curl.h"

#include <string>
#include <boost/test/unit_test.hpp>

using namespace std;

BOOST_AUTO_TEST_SUITE(curl_tests)


BOOST_AUTO_TEST_CASE(curl_getredirect_test)
{
    string err1, out1;
    BOOST_CHECK(GetRedirect("https://github.com/ColossusCoinXT/ColossusCoinXT/releases/latest", out1, err1));
    BOOST_CHECK(err1.empty());
    BOOST_CHECK(!out1.empty());

    string err2, out2;
    BOOST_CHECK(!GetRedirect("https://google.com", out2, err2));
    BOOST_CHECK(!err2.empty());
    BOOST_CHECK(out2.empty());
}


BOOST_AUTO_TEST_SUITE_END()
