// Copyright (c) 2011-2013 The Bitcoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Unit tests for block-chain checkpoints
//

#include "checkpoints.h"

#include "uint256.h"

#include <boost/test/unit_test.hpp>

using namespace std;

BOOST_AUTO_TEST_SUITE(Checkpoints_tests)

BOOST_AUTO_TEST_CASE(sanity)
{
    uint256 p2090 = uint256("0x00000000001a17b5aaea15479f80b10ace166664e29ae2a575bbdc6126cf0e12");
    uint256 p3250 = uint256("0x000000000007e3a1bdc37ba87b9621634b8b99a7f1f35fce1704747b43f47361");
    BOOST_CHECK(Checkpoints::CheckBlock(2090, p2090));
    BOOST_CHECK(Checkpoints::CheckBlock(3250, p3250));

    // Wrong hashes at checkpoints should fail:
    BOOST_CHECK(!Checkpoints::CheckBlock(2090, p3250));
    BOOST_CHECK(!Checkpoints::CheckBlock(3250, p2090));

    // ... but any hash not at a checkpoint should succeed:
    BOOST_CHECK(Checkpoints::CheckBlock(2090 + 1, p3250));
    BOOST_CHECK(Checkpoints::CheckBlock(3250 + 1, p2090));

    BOOST_CHECK(Checkpoints::GetTotalBlocksEstimate() >= 3250);
}

BOOST_AUTO_TEST_SUITE_END()
