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
    std::vector<int> height = {-1, 0, 1, 2, 200000, 300000, 500000, 1000000, 10000000};
    for (int i = 0; i < height.size(); i += 1)
        BOOST_CHECK(GetBlockValue(height[i]) == GetBlockValueReward(height[i]) + GetBlockValueBudget(height[i]));

    // block reward
    BOOST_CHECK(GetBlockValueReward(0) == CAmount(0) * COIN);
    BOOST_CHECK(GetBlockValueReward(1) == CAmount(12000000000) * COIN);
    BOOST_CHECK(GetBlockValueReward(2) == CAmount(2250) * COIN);
    BOOST_CHECK(GetBlockValueReward(151200) == CAmount(2250) * COIN);
    BOOST_CHECK(GetBlockValueReward(151201) == CAmount(1125) * COIN);
    BOOST_CHECK(GetBlockValueReward(302399) == CAmount(1125) * COIN);
    BOOST_CHECK(GetBlockValueReward(302400) == CAmount(900) * COIN);
    BOOST_CHECK(GetBlockValueReward(1000000) == CAmount(900) * COIN);
    BOOST_CHECK(GetBlockValueReward(10000000) == CAmount(900) * COIN);

    // budget amount
    CBudgetManager budget;
    BOOST_CHECK(budget.GetTotalBudget(302399) == CAmount(125 * 60*60*24*30 / 120) * COIN);
    BOOST_CHECK(budget.GetTotalBudget(302400) == CAmount(100 * 60*60*24*30 / 120) * COIN);

    // masternode reward
    const CAmount nMoneySupply = 12000000000*COIN;
    const CAmount nBlockValue = GetBlockValueReward(302400); // 900 COIN
    BOOST_CHECK(GetMasternodePayment(0, nBlockValue, 0, nMoneySupply) == 0);
    BOOST_CHECK(GetMasternodePayment(0, nBlockValue, 1, nMoneySupply) == nBlockValue*0.9);
    BOOST_CHECK(GetMasternodePayment(0, nBlockValue, 190, nMoneySupply) == nBlockValue*0.74);
    BOOST_CHECK(GetMasternodePayment(0, nBlockValue, 2000, nMoneySupply) == nBlockValue*0.01);
}

BOOST_AUTO_TEST_SUITE_END()
