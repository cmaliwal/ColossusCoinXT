// Copyright (c) 2014 The Bitcoin Core developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "masternode-budget.h"

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(main_tests)

CAmount nMoneySupplyPoWEnd = 43199500 * COIN;

BOOST_AUTO_TEST_CASE(block_value)
{
    std::vector<int> height = {-1, 0, 1, 2, 151201, 151202, 302400, 302401, 388799, 388800, 500000, 1000000, 10000000};
    for (int h : height) {
        BOOST_CHECK(GetBlockValue(h) == GetBlockValueReward(h) + GetBlockValueBudget(h));
        BOOST_CHECK(GetBlockExpectedMint(h) == GetBlockValueReward(h));
    }

    // block reward
    BOOST_CHECK(GetBlockValueReward(0) == CAmount(0) * COIN);
    BOOST_CHECK(GetBlockValueReward(1) == CAmount(12000000000) * COIN);
    BOOST_CHECK(GetBlockValueReward(2) == CAmount(2500) * COIN * 95 / 100);
    BOOST_CHECK(GetBlockValueReward(151201) == CAmount(2500) * COIN * 95 / 100);
    BOOST_CHECK(GetBlockValueReward(151202) == CAmount(1250) * COIN * 95 / 100);
    BOOST_CHECK(GetBlockValueReward(302400) == CAmount(1250) * COIN * 95 / 100);
    BOOST_CHECK(GetBlockValueReward(302401) == CAmount(1000) * COIN * 95 / 100);
    BOOST_CHECK(GetBlockValueReward(388799) == CAmount(1000) * COIN * 95 / 100);
    BOOST_CHECK(GetBlockValueReward(388800) == CAmount(1500) * COIN * 90 / 100);
    BOOST_CHECK(GetBlockValueReward(500000) == CAmount(1500) * COIN * 90 / 100);
    BOOST_CHECK(GetBlockValueReward(1000000) == CAmount(1500) * COIN * 90 / 100);

    // dev fund
    BOOST_CHECK(GetBlockValueDevFund(388799) == CAmount(0));
    BOOST_CHECK(GetBlockValueDevFund(388800) == CAmount(1500) * COIN * 10 / 100);

    // budget amount
    CBudgetManager budget;
    int nBlocksPerMonth = 60*60*24*30 / 60;
    BOOST_CHECK(budget.GetTotalBudget(302400) == CAmount(1250) * COIN * nBlocksPerMonth * 5 / 100);
    BOOST_CHECK(budget.GetTotalBudget(302401) == CAmount(1000) * COIN * nBlocksPerMonth * 5 / 100);
    BOOST_CHECK(budget.GetTotalBudget(388799) == CAmount(1000) * COIN * nBlocksPerMonth * 5 / 100);
    BOOST_CHECK(budget.GetTotalBudget(388800) == CAmount(1500) * COIN * nBlocksPerMonth * 10 / 100);

    // masternode reward fixed 60%
    BOOST_CHECK(GetMasternodePayment(388799, 0, 0) == GetBlockValueReward(388799) * 60 / 100);

    // masternode reward see-saw
    const CAmount nMoneySupply = 12000000000*COIN;
    const CAmount nReward = GetBlockValueReward(388800) - GetBlockValueDevFund(388800);
    BOOST_CHECK(GetMasternodePayment(388800, 0, nMoneySupply) == 0);
    BOOST_CHECK(GetMasternodePayment(388800, 1, nMoneySupply) == nReward*0.9);
    BOOST_CHECK(GetMasternodePayment(388800, 190, nMoneySupply) == nReward*0.74);
    BOOST_CHECK(GetMasternodePayment(388800, 2000, nMoneySupply) == nReward*0.01);
}

BOOST_AUTO_TEST_SUITE_END()