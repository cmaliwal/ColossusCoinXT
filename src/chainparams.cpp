// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017 The ColossusXT developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"

#include "random.h"
#include "util.h"
#include "utilstrencodings.h"
#include "base58.h"

#include <assert.h>

#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost::assign;

struct SeedSpec6 {
    uint8_t addr[16];
    uint16_t port;
};

#include "chainparamsseeds.h"

/**
 * Main network
 */

//! Convert the pnSeeds6 array into usable address objects.
static void convertSeed6(std::vector<CAddress>& vSeedsOut, const SeedSpec6* data, unsigned int count)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7 * 24 * 60 * 60;
    for (unsigned int i = 0; i < count; i++) {
        struct in6_addr ip;
        memcpy(&ip, data[i].addr, sizeof(ip));
        CAddress addr(CService(ip, data[i].port));
        addr.nTime = GetTime() - GetRand(nOneWeek) - nOneWeek;
        vSeedsOut.push_back(addr);
    }
}

//   What makes a good checkpoint block?
// + Is surrounded by blocks with reasonable timestamps
//   (no blocks before with a timestamp after, none after with
//    timestamp before)
// + Contains no strange transactions
static Checkpoints::MapCheckpoints mapCheckpoints =
    boost::assign::map_list_of(1, uint256("00000dc8bfcb3ce2b02ebb378ae409353a4bd31a7f5e46951ac08506d591cbd7")) ///
        (57, uint256("000000e62c81ce3db7a0954ce7573a57f127028cd01dd5bc2c0d31d890b2f46b"))
        (2050, uint256("000000000097133d6e0b0ce0216a632f84273dc4cafad6ea56d54427a7a62e47"))
        (2090, uint256("00000000001a17b5aaea15479f80b10ace166664e29ae2a575bbdc6126cf0e12"))
        (3250, uint256("000000000007e3a1bdc37ba87b9621634b8b99a7f1f35fce1704747b43f47361"))
        (300006,uint256("cfe4a07b3ba6c57d9b255f0cd55e39753a0a24ff34526aebc120d29e3c62ab69")) // First block with stake min age 8 hours
        (351117,uint256("d0b422f62c4856cdbc7d2c21169f67d4dbbe5f7c43622bda0c50d756dc159454")) // Fork correction
        (356500,uint256("f04f249dd88571539286e1cc8d1d9136ec2733671b8d8294509db59d62030411")); // Fork correction

static const Checkpoints::CCheckpointData data = {
    &mapCheckpoints,
    1528393811, // * UNIX timestamp of last checkpoint block
    761041,     // * total number of transactions between genesis and last checkpoint
                //   (the tx=... number in the SetBestChain debug.log lines)
    2000        // * estimated number of transactions per day after checkpoint
};

static Checkpoints::MapCheckpoints mapCheckpointsTestnet =
    boost::assign::map_list_of(0, uint256("6cd37a546cfaafeee652fd0f3a85ba64c0f539f771a27fca9610cdc2f3278932"));
static const Checkpoints::CCheckpointData dataTestnet = {
    &mapCheckpointsTestnet,
    1520769358,
    0,
    250};

static Checkpoints::MapCheckpoints mapCheckpointsRegtest =
    boost::assign::map_list_of(0, uint256("0x001"));
static const Checkpoints::CCheckpointData dataRegtest = {
    &mapCheckpointsRegtest,
    1454124731,
    0,
    100};

libzerocoin::ZerocoinParams* CChainParams::Zerocoin_Params() const
{
    assert(this);
    static CBigNum bnTrustedModulus(zerocoinModulus);
    static libzerocoin::ZerocoinParams ZCParams = libzerocoin::ZerocoinParams(bnTrustedModulus);

    return &ZCParams;
}

class CMainParams : public CChainParams
{
public:
    CMainParams()
    {
        networkID = CBaseChainParams::MAIN;
        strNetworkID = "main";
        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0x91;
        pchMessageStart[1] = 0xc5;
        pchMessageStart[2] = 0xfe;
        pchMessageStart[3] = 0xea;
        vAlertPubKey = ParseHex("0000098d3ba6ba6e7423fa5cbd6a89e0a9a5300f88d33000005cb1a8b7ed2c1000335fc8dc4f012cb8241cc0bdafd6ca70c5f5448916e4e6f511ffffffffffffff");
        nDefaultPort = 51572;
        bnProofOfWorkLimit = ~uint256(0) >> 20; // ColossusXT starting difficulty is 1 / 2^12
        bnProofOfStakeLimit = (~uint256(0) >> 24);
        nSubsidyHalvingInterval = 210000;
        nEnforceBlockUpgradeMajority = 750;
        nRejectBlockOutdatedMajority = 950;
        nToCheckBlockUpgradeMajority = 1000;
        nMinerThreads = 0;
        nTargetTimespan = 1 * 60 * 40;
        nTargetSpacing = 1 * 60;
        nPastBlocksMin = 24;
        nLastPOWBlock = 10080;
        nMaturity = 90;
        nMasternodeCountDrift = 20;
        nModifierUpdateBlock = 0;
        nMaxMoneyOut = int64_t(20000000000) * COIN;
        nModifierInterval = 60;
        nModifierIntervalRatio = 3;
        nBudgetPercent = 10;
        nDevFundPercent = 10;
        nBudgetPaymentCycle = 60*60*24*30; // 1 month
        nMasternodePaymentSigTotal = 10;
        nMasternodePaymentSigRequired = 6;
        nRequiredMasternodeCollateral = 10000000 * COIN; //10,000,000

        /**
         * Build the genesis block. Note that the output of the genesis coinbase cannot
         * be spent as it did not originally exist in the database.
         *
         * CBlock(hash=00000ffd590b14, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=e0028e, nTime=1390095618, nBits=1e0ffff0, nNonce=28917698, vtx=1)
         *   CTransaction(hash=e0028e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
         *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d01044c5957697265642030392f4a616e2f3230313420546865204772616e64204578706572696d656e7420476f6573204c6976653a204f76657273746f636b2e636f6d204973204e6f7720416363657074696e6720426974636f696e73)
         *     CTxOut(nValue=50.00000000, scriptPubKey=0xA9037BAC7050C479B121CF)
         *   vMerkleTree: e0028e
         */
        //// ZCTEST: experimenting w/ the genesis
        //const std::string pszTimestamp = "U.S. News & World Report Jan 28 2016 With His Absence, Trump Dominates Another Debate";
        const std::string pszTimestamp = "2017-09-21 22:01:04 : Bitcoin Block Hash for Height 486382 : 00000000000000000092d15e5b3e6e8269398a84a60ae5a2dbd4e7f431199d03";
        CMutableTransaction txNew;
        txNew.vin.resize(1);
        txNew.vout.resize(1);
        txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << vector<unsigned char>(pszTimestamp.begin(), pszTimestamp.end());
        txNew.vout[0].nValue = 250 * COIN;
        txNew.vout[0].scriptPubKey = CScript() << ParseHex("04c10e83b2703ccf322f7dbd62dd5855ac7c10bd055814ce121ba32607d573b8810c02c0582aed05b4deb9c4b77b26d92428c61256cd42774babea0a073b2ed0c9") << OP_CHECKSIG;
        genesis.vtx.push_back(txNew);
        genesis.hashPrevBlock = 0;
        genesis.hashMerkleRoot = genesis.BuildMerkleTree();
        genesis.nVersion = 1;
        genesis.nTime = 1454124731;
        genesis.nBits = 0x1e0ffff0;
        genesis.nNonce = 2402015;

        hashGenesisBlock = genesis.GetHash();
        //// ZCTEST: experimenting w/ the genesis
        //assert(hashGenesisBlock == uint256("0x0000041e482b9b9691d98eefb48473405c0b8ec31b76df3797c74a78680ef818"));
        //assert(genesis.hashMerkleRoot == uint256("0x1b2ef6e2f28be914103a277377ae7729dcd125dfeb8bf97bd5964ba72b6dc39b"));
        assert(hashGenesisBlock == uint256("0xa0ce8206c908357008c1b9a8ba2813aff0989ca7f72d62b14e652c55f02b4f5c"));
        assert(genesis.hashMerkleRoot == uint256("0xf7c9a0d34fffa0887892dff1f384048b7be854a99937871705283758b727e414"));

        vSeeds.push_back(CDNSSeedData("colx1", "seed.colossuscoinxt.org"));
        vSeeds.push_back(CDNSSeedData("colx2", "seed.colossusxt.org"));
        vSeeds.push_back(CDNSSeedData("colx3", "seed.colxt.net"));

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 30);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 13);
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 212);
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x02)(0x2D)(0x25)(0x33).convert_to_container<std::vector<unsigned char> >();
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x02)(0x21)(0x31)(0x2B).convert_to_container<std::vector<unsigned char> >();
        // 	BIP44 coin type is from https://github.com/satoshilabs/slips/blob/master/slip-0044.md
        base58Prefixes[EXT_COIN_TYPE] = boost::assign::list_of(0x80)(0x00)(0x00)(0x77).convert_to_container<std::vector<unsigned char> >();

        convertSeed6(vFixedSeeds, pnSeed6_main, ARRAYLEN(pnSeed6_main));

        fRequireRPCPassword = true;
        fMiningRequiresPeers = true;
        fAllowMinDifficultyBlocks = false;
        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;
        fSkipProofOfWorkCheck = false;
        fTestnetToBeDeprecatedFieldRPC = false;
        fHeadersFirstSyncingActive = false;

        nPoolMaxTransactions = 3;

        strSporkKey = "0423f2b48d99f15a0bceedbe9b05a06d028aca587c3a0f0ee4a7dff6b0859181c1225b5842a17e8bb74758b8f1757a82025631f3276bec0734c6f61de71c1e4d28";
        strObfuscationPoolDummyAddress = "D87q2gC9j6nNrnzCsg4aY6bHMLsT9nUhEw";
        nStartMasternodePayments = 1403728576; //Wed, 25 Jun 2014 20:36:16 GMT

        /** Zerocoin */
        zerocoinModulus = "eea95da0a8c277c6fa63a84ab86e748bd499417feb8bc312e67d4076e6d1bb048d0e60afc42bc04b4f3afb754b35bc2056"
            "31855dc262811b2f77b8323ece492299e599b8ac048b3335004c63bb115fa1448619741ae4eafd21e59fc41761bf0015fe96fc0300ed78b8"
            "b050440b093f82bcbdf4d63d9851004d72c71b79506b687a7a12a796813da48baa4ab23708f1d1b5b380ac5cc38529465961a1c098da9f26"
            "06dcb6d5393e9b9eef43f8eb42c8e3e1634a0d1649337a409fe2b948fd3f5753e258ef855b72a5a83f34f14add3180ffc15e1da6a3d6a147"
            "ad4ef9da82a9253b39f5ffca4c483d76333c93c1781b2b0e5ccb520ebf58da506da303ea28c975a4463c0fc5b2f8a7421e072d09ef391268"
            "fd64fcb0f72a736350ae83b394d5f861af8378b348b4a359b5e5e8837aae2ba4b23838610fe65605fd7ef34972b47e773a906b56a60129ce"
            "04ed78030ff7ad2c2c4f54cc715ee0cccab5f42566829ad507c4bd834cde358ff079f87c0352b3434c059d3df8bcb7e9b19f13f9150b41";
        nMaxZerocoinSpendsPerTransaction = 7; // Assume about 20kb each
        nMinZerocoinMintFee = 1 * CENT; //high fee required for zerocoin mints
        // ZCDEV: confirmations are not allowing us to confirm and spend zc-s.
        nMintRequiredConfirmations = 0; // 20; //the maximum amount of confirmations until accumulated in 19
        nRequiredAccumulation = 1; // 0?
        nDefaultSecurityLevel = 100; //full security level for accumulators
        nZerocoinHeaderVersion = CBlockHeader::VERSION5; //Block headers must be this version once zerocoin is active
        nBudget_Fee_Confirmations = 6; // Number of confirmations for the finalization fee

        //FIXME: params must correspond zerocoin release
        /** Height or Time Based Activations **/
        //nLastPOWBlock = 10080;  // 259200
        //nModifierUpdateBlock = 0; // 615800
        nZerocoinStartHeight = 333333; // 863787;
        // ZCTEST: a bit in the future for blocks to be accepted (i.e. to be able to run the testnet)
        nZerocoinStartTime = 1527972200; // June 2nd, 2018 8:43:20 PM
        nBlockEnforceSerialRange = 533333; // 895400; //Enforce serial range starting this block
        nBlockRecalculateAccumulators = 533333; // 908000; //Trigger a recalculation of accumulators
        nBlockFirstFraudulent = 533333; // 891737; //First block that bad serials emerged
        nBlockLastGoodCheckpoint = 533333; // 891730; //Last valid accumulator checkpoint
        nBlockEnforceInvalidUTXO = 533333; // 902850; //Start enforcing the invalid UTXO's

        // THIS IS FOR TESTING PURPOSES ONLY
        // ZCTEST: // ZCMAINNET: this is to avoid errors related to nBits and POW (another layer of changes, still testing)
        fSkipProofOfWorkCheck = true;
        nZerocoinStartHeight = 9333333;
        nZerocoinStartTime = 1539999999; // 1529999999;
        //nZerocoinHeaderVersion = 5;

        // ZCTEST: // tests are failing due to the modulus change, paramgen generated big#-s don't work for some reason,
        // ...I'm guessing that tests are hardcoded (some of the tx-s there, modulus's been adjusted). Questions is whether that
        // will result in some ZC problems? For that reason, leaving the zc modulus to what pivx is using
        zerocoinModulus = "25195908475657893494027183240048398571429282126204032027777137836043662020707595556264018525880784"
            "4069182906412495150821892985591491761845028084891200728449926873928072877767359714183472702618963750149718246911"
            "6507761337985909570009733045974880842840179742910064245869181719511874612151517265463228221686998754918242243363"
            "7259085141865462043576798423387184774447920739934236584823824281198163815010674810451660377306056201619676256133"
            "8441436038339044149526344321901146575444541784240209246165157233507787077498171257724679629263863563732899121548"
            "31438167899885040445364023527381951378636564391212010397122822120720357";

    }

    CBitcoinAddress GetDevFundAddress() const
    { return CBitcoinAddress("DBKqofwU8QUFYFwNYZetyBbj2Y7oAcWLbX"); }

    CBitcoinAddress GetTxFeeAddress() const
    { return CBitcoinAddress("DEKP7sVxwwuN1mtCpTXtjua77XqFBBRaKG"); }

    CBitcoinAddress GetUnallocatedBudgetAddress() const
    { return CBitcoinAddress("DE2nWCnyYyWxoUNRg5gEeA7Kx1kpBs2spB"); }

    int GetChainHeight(ChainHeight ch) const
    {
        switch (ch) {
        case ChainHeight::H1:
            return 1;

        case ChainHeight::H2:
            return 151202;

        case ChainHeight::H3:
            return 302401;

        case ChainHeight::H4:
            return 388800;

        default:
            assert(false);
            return -1;
        }
    }

    int64_t GetMinStakeAge(int nTargetHeight) const
    {
        if (nTargetHeight >= 300000)
            return 60*60*8; //8 hours
        else
            return 60*60*24*7; //7 days
    }

    const Checkpoints::CCheckpointData& Checkpoints() const
    {
        return data;
    }
};
static CMainParams mainParams;

/**
 * Testnet (v3)
 */
class CTestNetParams : public CMainParams
{
public:
    CTestNetParams()
    {
        networkID = CBaseChainParams::TESTNET;
        strNetworkID = "test";
        pchMessageStart[0] = 0x46;
        pchMessageStart[1] = 0x77;
        pchMessageStart[2] = 0x66;
        pchMessageStart[3] = 0xbb;
        vAlertPubKey = ParseHex("000010e83b2703ccf322f7dbd62dd5855ac7c10bd055814ce121ba32607d573b8810c02c0582aed05b4deb9c4b77b26d92428c61256cd42774babea0a073b2ed0c9");
        nDefaultPort = 51374;
        bnProofOfWorkLimit = ~uint256(0) >> 1;
        nEnforceBlockUpgradeMajority = 51;
        nRejectBlockOutdatedMajority = 75;
        nToCheckBlockUpgradeMajority = 100;
        nMinerThreads = 0;
        nTargetTimespan = 1 * 60 * 40;
        nTargetSpacing = 1 * 60;
        nPastBlocksMin = 10080;
        nLastPOWBlock = 10080;
        nMaturity = 15;
        nModifierUpdateBlock = 0; //approx Mon, 17 Apr 2017 04:00:00 GMT
        nMaxMoneyOut = int64_t(20000000000) * COIN;
        nModifierInterval = 60;
        nModifierIntervalRatio = 3;
        nBudgetPercent = 10;
        nDevFundPercent = 10;
        nBudgetPaymentCycle = 60*60*2; // 2 hours
        nRequiredMasternodeCollateral = 10000000 * COIN; //10,000,000
        nMasternodePaymentSigTotal = 10;
        nMasternodePaymentSigRequired = 1;

        //! Modify the testnet genesis block so the timestamp is valid for a later start.
        genesis.nTime = 1520769358;
        genesis.nNonce = 2452017;

        hashGenesisBlock = genesis.GetHash();
        //// ZCTEST: experimenting w/ the genesis
        //assert(hashGenesisBlock == uint256("0xf6b11dff4a93ed81adc9eb0aa2709564f3567cc393831e3ff99d378e7f567b16"));
        assert(hashGenesisBlock == uint256("0x6cd37a546cfaafeee652fd0f3a85ba64c0f539f771a27fca9610cdc2f3278932"));

        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 139); // Testnet colx addresses start with 'x' or 'y'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 19);  // Testnet colx script addresses start with '8' or '9'
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);     // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
        // Testnet colx BIP32 pubkeys start with 'DRKV'
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x3a)(0x80)(0x61)(0xa0).convert_to_container<std::vector<unsigned char> >();
        // Testnet colx BIP32 prvkeys start with 'DRKP'
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x3a)(0x80)(0x58)(0x37).convert_to_container<std::vector<unsigned char> >();
        // Testnet colx BIP44 coin type is '1' (All coin's testnet default)
        base58Prefixes[EXT_COIN_TYPE] = boost::assign::list_of(0x80)(0x00)(0x00)(0x01).convert_to_container<std::vector<unsigned char> >();

        convertSeed6(vFixedSeeds, pnSeed6_test, ARRAYLEN(pnSeed6_test));

        fRequireRPCPassword = true;
        fMiningRequiresPeers = true;
        fAllowMinDifficultyBlocks = false;
        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;
        fSkipProofOfWorkCheck = false;
        fTestnetToBeDeprecatedFieldRPC = true;
        fHeadersFirstSyncingActive = false;

        nPoolMaxTransactions = 3;
        strSporkKey = "026ee678f254a97675a90ebea1e7593fdb53047321f3cb0560966d4202b32c48e2";
        strObfuscationPoolDummyAddress = "y57cqfGRkekRyDRNeJiLtYVEbvhXrNbmox";
        nStartMasternodePayments = 1420837558; //Fri, 09 Jan 2015 21:05:58 GMT
        nBudget_Fee_Confirmations = 3; // Number of confirmations for the finalization fee. We have to make this very short
                                       // here because we only have a 8 block finalization window on testnet

        // ZC: 
        /** Height or Time Based Activations **/
        nZerocoinStartHeight = 333333; // 863787;
        // ZCTEST: a bit in the future for blocks to be accepted (i.e. to be able to run the testnet)
        nZerocoinStartTime = 1527972200; // June 2nd, 2018 8:43:20 PM
        nBlockEnforceSerialRange = 533333; // 895400; //Enforce serial range starting this block
        nBlockRecalculateAccumulators = 533333; // 908000; //Trigger a recalculation of accumulators
        nBlockFirstFraudulent = 533333; // 891737; //First block that bad serials emerged
        nBlockLastGoodCheckpoint = 533333; // 891730; //Last valid accumulator checkpoint
        nBlockEnforceInvalidUTXO = 533333; // 902850; //Start enforcing the invalid UTXO's
        // THIS IS FOR TESTING PURPOSES ONLY
        // ZCTEST: // ZCTESTNET: this is to avoid errors related to nBits and POW (another layer of changes, still testing)
        // ZCTEST: // ZCTESTNET: adjusting to the testnet version
        fSkipProofOfWorkCheck = true;
        nZerocoinStartHeight = 52000;
        nZerocoinStartTime = 1528142000; // 1529999999;

        //strSporkKey = "026ee678f254a97675a90ebea1e7593fdb53047321f3cb0560966d4202b32c48e2";
        //nLastPOWBlock = 200;
        //nModifierUpdateBlock = 0; // 51197 //approx Mon, 17 Apr 2017 04:00:00 GMT
        //nZerocoinStartHeight = 201576;
        //nZerocoinStartTime = 1526272200; // May 14, 2018 04:30:00 AM // 1501776000;
        //nBlockEnforceSerialRange = 1; //Enforce serial range starting this block
    }

    CBitcoinAddress GetDevFundAddress() const
    { return CBitcoinAddress("y4XhfKjJPwxi42YRQssbdDytJ74W8V1bVt"); }

    CBitcoinAddress GetTxFeeAddress() const
    { return CBitcoinAddress("yE8w3zvHtbn7mAFxyKk1UJEX92DWrnqzg6"); }

    CBitcoinAddress GetUnallocatedBudgetAddress() const
    { return CBitcoinAddress("yBtxR3o3uvbtkfeWLuFqa7o7yY9N1ha4Yn"); }

    int GetChainHeight(ChainHeight ch) const
    {
        switch (ch) {
        case ChainHeight::H1:
            return 1;

        case ChainHeight::H2:
        case ChainHeight::H3:
        case ChainHeight::H4:
            return 35500; // on testnet

        default:
            assert(false);
            return -1;
        }
    }

    int64_t GetMinStakeAge(int nTargetHeight) const
    {
        return 60*60*8; //8 hours
    }

    const Checkpoints::CCheckpointData& Checkpoints() const
    {
        return dataTestnet;
    }
};
static CTestNetParams testNetParams;

/**
 * Regression test
 */
class CRegTestParams : public CTestNetParams
{
public:
    CRegTestParams()
    {
        networkID = CBaseChainParams::REGTEST;
        strNetworkID = "regtest";
        strNetworkID = "regtest";
        pchMessageStart[0] = 0xa1;
        pchMessageStart[1] = 0xcf;
        pchMessageStart[2] = 0x7e;
        pchMessageStart[3] = 0xac;
        nSubsidyHalvingInterval = 150;
        nEnforceBlockUpgradeMajority = 750;
        nRejectBlockOutdatedMajority = 950;
        nToCheckBlockUpgradeMajority = 1000;
        nMinerThreads = 1;
        nTargetTimespan = 24 * 60 * 60; // COLX: 1 day
        nTargetSpacing = 1 * 60;        // COLX: 1 minutes
        nPastBlocksMin = 200;
        bnProofOfWorkLimit = ~uint256(0) >> 1;
        genesis.nTime = 1454124731;
        genesis.nBits = 0x207fffff;
        genesis.nNonce = 12345;

        hashGenesisBlock = genesis.GetHash();
        nDefaultPort = 51476;
        //// ZCTEST: experimenting w/ the genesis
        //assert(hashGenesisBlock == uint256("0x4f023a2120d9127b21bbad01724fdb79b519f593f2a85b60d3d79160ec5f29df"));
        assert(hashGenesisBlock == uint256("0x41d203d900885c5ff18d2c550957743a164060a184182fa17ad1d8cff46c7eac"));

        vFixedSeeds.clear(); //! Testnet mode doesn't have any fixed seeds.
        vSeeds.clear();      //! Testnet mode doesn't have any DNS seeds.

        fRequireRPCPassword = false;
        fMiningRequiresPeers = false;
        fAllowMinDifficultyBlocks = true;
        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true;
        fTestnetToBeDeprecatedFieldRPC = false;
    }

    const Checkpoints::CCheckpointData& Checkpoints() const
    {
        return dataRegtest;
    }
};
static CRegTestParams regTestParams;

/**
 * Unit test
 */
class CUnitTestParams : public CMainParams, public CModifiableParams
{
public:
    CUnitTestParams()
    {
        networkID = CBaseChainParams::UNITTEST;
        strNetworkID = "unittest";
        nDefaultPort = 51478;
        vFixedSeeds.clear(); //! Unit test mode doesn't have any fixed seeds.
        vSeeds.clear();      //! Unit test mode doesn't have any DNS seeds.

        fRequireRPCPassword = false;
        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = true;
        fAllowMinDifficultyBlocks = false;
        fMineBlocksOnDemand = true;
    }

    const Checkpoints::CCheckpointData& Checkpoints() const
    {
        // UnitTest share the same checkpoints as MAIN
        return data;
    }

    //! Published setters to allow changing values in unit test cases
    virtual void setSubsidyHalvingInterval(int anSubsidyHalvingInterval) { nSubsidyHalvingInterval = anSubsidyHalvingInterval; }
    virtual void setEnforceBlockUpgradeMajority(int anEnforceBlockUpgradeMajority) { nEnforceBlockUpgradeMajority = anEnforceBlockUpgradeMajority; }
    virtual void setRejectBlockOutdatedMajority(int anRejectBlockOutdatedMajority) { nRejectBlockOutdatedMajority = anRejectBlockOutdatedMajority; }
    virtual void setToCheckBlockUpgradeMajority(int anToCheckBlockUpgradeMajority) { nToCheckBlockUpgradeMajority = anToCheckBlockUpgradeMajority; }
    virtual void setDefaultConsistencyChecks(bool afDefaultConsistencyChecks) { fDefaultConsistencyChecks = afDefaultConsistencyChecks; }
    virtual void setAllowMinDifficultyBlocks(bool afAllowMinDifficultyBlocks) { fAllowMinDifficultyBlocks = afAllowMinDifficultyBlocks; }
    virtual void setSkipProofOfWorkCheck(bool afSkipProofOfWorkCheck) { fSkipProofOfWorkCheck = afSkipProofOfWorkCheck; }
};
static CUnitTestParams unitTestParams;


static CChainParams* pCurrentParams = 0;

CModifiableParams* ModifiableParams()
{
    assert(pCurrentParams);
    assert(pCurrentParams == &unitTestParams);
    return (CModifiableParams*)&unitTestParams;
}

const CChainParams& Params()
{
    assert(pCurrentParams);
    return *pCurrentParams;
}

CChainParams& Params(CBaseChainParams::Network network)
{
    switch (network) {
    case CBaseChainParams::MAIN:
        return mainParams;
    case CBaseChainParams::TESTNET:
        return testNetParams;
    case CBaseChainParams::REGTEST:
        return regTestParams;
    case CBaseChainParams::UNITTEST:
        return unitTestParams;
    default:
        assert(false && "Unimplemented network");
        return mainParams;
    }
}

void SelectParams(CBaseChainParams::Network network)
{
    SelectBaseParams(network);
    pCurrentParams = &Params(network);
}

bool SelectParamsFromCommandLine()
{
    CBaseChainParams::Network network = NetworkIdFromCommandLine();
    if (network == CBaseChainParams::MAX_NETWORK_TYPES)
        return false;

    SelectParams(network);
    return true;
}
