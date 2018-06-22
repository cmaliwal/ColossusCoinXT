/**
 * @file       Denominations.h
 *
 * @brief      Denomination info for the Zerocoin library.
 *
 * @copyright  Copyright 2017 PIVX Developers
 * @license    This project is released under the MIT license.
 **/

#ifndef DENOMINATIONS_H_
#define DENOMINATIONS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace libzerocoin {

// ZCDENOMINATIONS: 10000 was added AccumulatorMap::GetCheckpoint() is asserting,
// we need to have '8' exactly. Removing 1 will throw off the tests etc. So rethink and redo
enum  CoinDenomination {
    ZQ_ERROR = 0,
    ZQ_ONE = 1,
    ZQ_FIVE = 5,
    ZQ_TEN = 10,
    ZQ_FIFTY = 50,
    ZQ_ONE_HUNDRED = 100,
    ZQ_FIVE_HUNDRED = 500,
    ZQ_ONE_THOUSAND = 1000,
    ZQ_FIVE_THOUSAND = 5000 //, ZQ_TEN_THOUSAND = 10000
};

// Order is with the Smallest Denomination first and is important for a particular routine that this order is maintained
const std::vector<CoinDenomination> zerocoinDenomList = {ZQ_ONE, ZQ_FIVE, ZQ_TEN, ZQ_FIFTY, ZQ_ONE_HUNDRED, ZQ_FIVE_HUNDRED, ZQ_ONE_THOUSAND, ZQ_FIVE_THOUSAND}; //, ZQ_TEN_THOUSAND};
// These are the max number you'd need at any one Denomination before moving to the higher denomination. Last number is 4, since it's the max number of
// possible spends at the moment    /
// ZCDENOMINATIONS: this should match in size w/ zerocoinDenomList, not used?
const std::vector<int> maxCoinsAtDenom = { 4, 1, 4, 1, 4, 1, 4, 4 };
//const std::vector<int> maxCoinsAtDenom   = {4, 1, 4, 1, 4, 1, 4, 1, 4};

int64_t ZerocoinDenominationToInt(const CoinDenomination& denomination);
int64_t ZerocoinDenominationToAmount(const CoinDenomination& denomination);
CoinDenomination IntToZerocoinDenomination(int64_t amount);
CoinDenomination AmountToZerocoinDenomination(int64_t amount);
CoinDenomination AmountToClosestDenomination(int64_t nAmount, int64_t& nRemaining);
CoinDenomination get_denomination(std::string denomAmount);
int64_t get_amount(std::string denomAmount);

} /* namespace libzerocoin */
#endif /* DENOMINATIONS_H_ */
