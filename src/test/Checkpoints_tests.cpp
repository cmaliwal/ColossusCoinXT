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
    // ZCTEST: this was reworked to fit the new checkpoints (tests were failing)
    uint256 p57 = uint256("0x000000e62c81ce3db7a0954ce7573a57f127028cd01dd5bc2c0d31d890b2f46b");
    uint256 p3250 = uint256("0x000000000007e3a1bdc37ba87b9621634b8b99a7f1f35fce1704747b43f47361");
    BOOST_CHECK(Checkpoints::CheckBlock(57, p57));
    BOOST_CHECK(Checkpoints::CheckBlock(3250, p3250));

    // Wrong hashes at checkpoints should fail:
    BOOST_CHECK(!Checkpoints::CheckBlock(57, p3250));
    BOOST_CHECK(!Checkpoints::CheckBlock(3250, p57));

    // ... but any hash not at a checkpoint should succeed:
    BOOST_CHECK(Checkpoints::CheckBlock(57 + 1, p3250));
    BOOST_CHECK(Checkpoints::CheckBlock(3250 + 1, p57));

    BOOST_CHECK(Checkpoints::GetTotalBlocksEstimate() >= 3250);

    //uint256 p259201 = uint256("0x1c9121bf9329a6234bfd1ea2d91515f19cd96990725265253f4b164283ade5dd");
    //uint256 p623933 = uint256("0xc7aafa648a0f1450157dc93bd4d7448913a85b7448f803b4ab970d91fc2a7da7");
    //BOOST_CHECK(Checkpoints::CheckBlock(259201, p259201));
    //BOOST_CHECK(Checkpoints::CheckBlock(623933, p623933));


    //// Wrong hashes at checkpoints should fail:
    //BOOST_CHECK(!Checkpoints::CheckBlock(259201, p623933));
    //BOOST_CHECK(!Checkpoints::CheckBlock(623933, p259201));

    //// ... but any hash not at a checkpoint should succeed:
    //BOOST_CHECK(Checkpoints::CheckBlock(259201+1, p623933));
    //BOOST_CHECK(Checkpoints::CheckBlock(623933+1, p259201));

    //BOOST_CHECK(Checkpoints::GetTotalBlocksEstimate() >= 623933);
}

BOOST_AUTO_TEST_SUITE_END()
