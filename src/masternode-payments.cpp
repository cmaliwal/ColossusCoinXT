// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode-payments.h"
#include "addrman.h"
#include "masternode-budget.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "obfuscation.h"
#include "spork.h"
#include "sync.h"
#include "util.h"
#include "utilmoneystr.h"
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>

/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;

CCriticalSection cs_vecPayments;
CCriticalSection cs_mapMasternodeBlocks;
CCriticalSection cs_mapMasternodePayeeVotes;

//
// CMasternodePaymentDB
//

CMasternodePaymentDB::CMasternodePaymentDB()
{
    pathDB = GetDataDir() / "mnpayments.dat";
    strMagicMessage = "MasternodePayments";
}

bool CMasternodePaymentDB::Write(const CMasternodePayments& objToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage;                   // masternode cache file specific magic message
    ssObj << FLATDATA(Params().MessageStart()); // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj.begin(), ssObj.end());
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathDB.string());

    // Write and commit header, data
    try {
        fileout << ssObj;
    } catch (std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrint("masternode","Written info to mnpayments.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CMasternodePaymentDB::ReadResult CMasternodePaymentDB::Read(CMasternodePayments& objToLoad, bool fDryRun)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..
        ssObj >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode payement cache magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CMasternodePayments object
        ssObj >> objToLoad;
    } catch (std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint("masternode","Loaded info from mnpayments.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint("masternode","  %s\n", objToLoad.ToString());
    if (!fDryRun) {
        LogPrint("masternode","Masternode payments manager - cleaning....\n");
        objToLoad.CleanPaymentList();
        LogPrint("masternode","Masternode payments manager - result:\n");
        LogPrint("masternode","  %s\n", objToLoad.ToString());
    }

    return Ok;
}

void DumpMasternodePayments()
{
    int64_t nStart = GetTimeMillis();

    CMasternodePaymentDB paymentdb;
    CMasternodePayments tempPayments;

    LogPrint("masternode","Verifying mnpayments.dat format...\n");
    CMasternodePaymentDB::ReadResult readResult = paymentdb.Read(tempPayments, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CMasternodePaymentDB::FileError)
        LogPrint("masternode","Missing budgets file - mnpayments.dat, will try to recreate\n");
    else if (readResult != CMasternodePaymentDB::Ok) {
        LogPrint("masternode","Error reading mnpayments.dat: ");
        if (readResult == CMasternodePaymentDB::IncorrectFormat)
            LogPrint("masternode","magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint("masternode","file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint("masternode","Writting info to mnpayments.dat...\n");
    paymentdb.Write(masternodePayments);

    LogPrint("masternode","Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

CAmount FindPayment(const CTransaction& tx, const string& address)
{
    CAmount nAmount = 0;
    BOOST_FOREACH(const CTxOut& out, tx.vout) {
        CTxDestination address1;
        ExtractDestination(out.scriptPubKey, address1);
        CBitcoinAddress address2(address1);
        if (address2.ToString() == address)
            nAmount += out.nValue;
    }

    return nAmount;
}

bool IsBlockValueValid(const CBlock& block, int nHeight, CAmount nExpectedValue, CAmount nMinted, CBlockIndex* pindexPrev)
{
    if (nHeight <= 0)
        return error("%s: Unexpected block height: %d (%s)\n", __func__, nHeight, block.GetHash().ToString());

    if (Params().NetworkID() == CBaseChainParams::TESTNET &&
        (nHeight == 10800 || nHeight == 11040 || nHeight == 11041))
        return true; // these blocks on testnet breaking rules

    if (!masternodeSync.IsSynced() || IsInitialBlockDownload()) {
        //there is no budget data to use to check anything and there is no old final budget's data
        //super blocks will always be on these blocks
        if (nHeight % GetBudgetPaymentCycleBlocks() < Params().GetMaxSuperBlocksPerCycle())
            return nMinted <= nExpectedValue + budget.GetTotalBudget(nHeight);
        else
            return nMinted <= nExpectedValue;
    } else { // we're synced and have data so check the budget schedule
        if (budget.IsBudgetPaymentBlock(nHeight))
            return nMinted <= nExpectedValue + budget.GetBudgetValue(nHeight, pindexPrev);
        else
            return nMinted <= nExpectedValue;
    }
}

bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight, CAmount nFees, CBlockIndex* pindexPrev)
{
    if (!masternodeSync.IsSynced()) { //there is no budget data to use to check anything -- find the longest chain
        LogPrint("mnpayments", "Client not synced, skipping block payee checks\n");
        return true;
    }

    const CTransaction& txNew = (nBlockHeight > Params().LAST_POW_BLOCK() ? block.vtx[1] : block.vtx[0]);

    bool mnValid = true;
    bool budgetValid = true;
    if (budget.IsBudgetPaymentBlock(nBlockHeight)) {
        budgetValid = budget.IsTransactionValid(txNew, nBlockHeight, pindexPrev);
        if (!budgetValid) {
            error("%s: invalid budget payment detected %s\n", __func__, txNew.ToString().c_str());
            if (IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT))
                return false;
            else {
                LogPrintf("Budget enforcement is disabled, accepting block %d\n", nBlockHeight);
                budgetValid = true;
            }
        }
    } else {
        mnValid = masternodePayments.IsTransactionValid(txNew, block.GetVersion(), nBlockHeight);
        if (!mnValid) {
            error("%s: invalid mn payment detected %s\n", __func__, txNew.ToString().c_str());
            if (IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT))
                return false;
            else {
                LogPrintf("Masternode payment enforcement is disabled, accepting block %d\n", nBlockHeight);
                mnValid = true;
            }
        }
    }

    bool feeValid = true;
    if (nFees > 0 && nBlockHeight >= Params().GetChainHeight(ChainHeight::H4)) {
        CAmount nAmount = FindPayment(txNew, Params().GetTxFeeAddress().ToString());
        if (0 == nAmount) {
            feeValid = false;
            error("%s: fee payment to the dev fund address was not found\n", __func__);
        } else
            feeValid = nAmount >= nFees;

        if (!feeValid) {
            error("%s: invalid fee payment detected, expected %s, payed %s, tx:\n%s\n",
                  __func__, FormatMoney(nFees), FormatMoney(nAmount), txNew.ToString().c_str());

            if (IsSporkActive(SPORK_17_FEE_PAYMENT_ENFORCEMENT))
                return false;
            else {
                LogPrintf("Fee enforcement is disabled, accepting block %d\n", nBlockHeight);
                feeValid = true;
            }
        }
    }

    bool fundValid = true;
    if (nBlockHeight >= Params().GetChainHeight(ChainHeight::H4)) {
        CAmount nAmount = FindPayment(txNew, Params().GetDevFundAddress().ToString());
        if (0 == nAmount) {
            fundValid = false;
            error("%s: required payment to the dev fund address was not found\n", __func__);
        } else
            fundValid = nAmount >= GetBlockValueDevFund(nBlockHeight);

        if (!fundValid) {
            error("%s: invalid dev fund payment detected, expected %s, payed %s, tx:\n%s\n",
                  __func__, FormatMoney(GetBlockValueDevFund(nBlockHeight)), FormatMoney(nAmount), txNew.ToString().c_str());

            if (IsSporkActive(SPORK_18_DEVFUND_PAYMENT_ENFORCEMENT))
                return false;
            else {
                LogPrintf("Dev fund enforcement is disabled, accepting block %d\n", nBlockHeight);
                fundValid = true;
            }
        }
    }

    return budgetValid && mnValid && feeValid && fundValid;
}

static bool SubtractFromStakeReward(CMutableTransaction& tx, int nStakeOut, CAmount nAmount, string& error)
{
    if (nStakeOut < 1) {
        error = strprintf("invalid nStakeOut: %d", nStakeOut);
        return false;
    }

    if (tx.vout.size() < 1 + nStakeOut) {
        error = strprintf("invalid vout size %u, nStakeOut=%d", tx.vout.size(), nStakeOut);
        return false;
    }

    if (nAmount <= 0) {
        error = strprintf("invalid amount: %s", FormatMoney(nAmount));
        return false;
    }

    CAmount nValueOut = CTransaction(tx).GetValueOut();
    if (nValueOut < nAmount) {
        error = strprintf("amount %s is greater than tx value out %s", FormatMoney(nAmount), FormatMoney(nValueOut));
        return false;
    }

    /**
     * For Proof Of Stake vout[0] must be null
     * Stake reward can be split into many different outputs, so we must
     * use vout.size() to align with several different cases.
     */
    if (nStakeOut > 1) {
        const CAmount nAmount1 = nAmount / 2;
        const CAmount nAmount2 = nAmount - nAmount1;
        tx.vout[1].nValue -= nAmount1;
        tx.vout[2].nValue -= nAmount2;
    } else
        tx.vout[1].nValue -= nAmount;

    return true;
}

void FillBlockPayee(CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake, CBlockIndex* pindexPrev)
{
    if (!pindexPrev) {
        error("%s: pindexPrev is nullptr", __func__);
        return;
    }

    const int nStakeOut = txNew.vout.size() > 2 ? 2 : 1;
    const int nTargetHeight = pindexPrev->nHeight + 1;
    if (budget.IsBudgetPaymentBlock(nTargetHeight))
        budget.FillBlockPayee(txNew, nFees, fProofOfStake, pindexPrev);
    else
        masternodePayments.FillBlockPayee(txNew, nFees, fProofOfStake, pindexPrev);

    //Append an additional output as the dev fund payment to the official Developer Fund Address
    const CAmount nDevfundPayment = GetBlockValueDevFund(nTargetHeight);
    if (nDevfundPayment > 0) {
        string msg;
        if (SubtractFromStakeReward(txNew, nStakeOut, nDevfundPayment, msg))
            txNew.vout.push_back(CTxOut(nDevfundPayment, GetScriptForDestination(Params().GetDevFundAddress().Get())));
        else
            error("%s: %s", __func__, msg);
    }

    if (nFees > 0) //Append an additional output as the tx fee payment to the official Developer Fund Address
        txNew.vout.push_back(CTxOut(nFees, GetScriptForDestination(Params().GetTxFeeAddress().Get())));
}

std::string GetRequiredPaymentsString(int nBlockHeight)
{
    if (budget.IsBudgetPaymentBlock(nBlockHeight)) {
        return budget.GetRequiredPaymentsString(nBlockHeight);
    } else {
        return masternodePayments.GetRequiredPaymentsString(nBlockHeight);
    }
}

void CMasternodePayments::FillBlockPayee(CMutableTransaction& txNew, int64_t nFees, bool fProofOfStake, CBlockIndex* pindexPrev)
{
    if (!pindexPrev) {
        error("%s: pindexPrev is nullptr", __func__);
        return;
    }

    CScript payee;
    bool hasPayment = true;
    const int nTargetHeight = pindexPrev->nHeight + 1;

    if (!masternodePayments.GetBlockPayee(nTargetHeight, payee)) {
        //no masternode detected
        CMasternode* winningNode = mnodeman.GetCurrentMasterNode(1);
        if (winningNode) {
            payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
        } else {
            LogPrint("masternode", "CreateNewBlock: Failed to detect masternode to pay\n");
            hasPayment = false;
        }
    }

    if (!hasPayment)
        return;

    const int nMasternodeCount = mnodeman.stable_size();
    const CAmount nBlockValue = GetBlockValueReward(nTargetHeight);
    const CAmount nMasternodePayment = GetMasternodePayment(nTargetHeight, nMasternodeCount, pindexPrev->nMoneySupply);

    if (fProofOfStake) {
        /**For Proof Of Stake vout[0] must be null
            * Stake reward can be split into many different outputs, so we must
            * use vout.size() to align with several different cases.
            */
        string msg;
        const int nStakeOut = txNew.vout.size() > 2 ? 2 : 1;
        if (SubtractFromStakeReward(txNew, nStakeOut, nMasternodePayment, msg)) {
            //Append an additional output as the masternode payment
            txNew.vout.push_back(CTxOut(nMasternodePayment, payee));

            DebugPrintf("*** total reward %s masternode %s\n", FormatMoney(nBlockValue), FormatMoney(nMasternodePayment));
        }
        else
            error("%s: %s", __func__, msg);
    } else {
        txNew.vout.resize(2);
        txNew.vout[1].scriptPubKey = payee;
        txNew.vout[1].nValue = nMasternodePayment;
        txNew.vout[0].nValue -= nMasternodePayment;
    }

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);

    LogPrint("masternode", "Masternode payment of %s to %s\n", FormatMoney(nMasternodePayment).c_str(), address2.ToString().c_str());
}

int CMasternodePayments::GetMinMasternodePaymentsProto()
{
    if (IsSporkActive(SPORK_10_MASTERNODE_PAY_UPDATED_NODES))
        return ActiveProtocol();                          // Allow only updated peers
    else
        return MIN_PEER_PROTO_VERSION_BEFORE_ENFORCEMENT; // Also allow old peers as long as they are allowed to run
}

void CMasternodePayments::ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (!masternodeSync.IsBlockchainSynced()) return;

    if (fLiteMode) return; //disable all Obfuscation/Masternode related functionality


    if (strCommand == "mnget") { //Masternode Payments Request Sync
        if (fLiteMode) return;   //disable all Obfuscation/Masternode related functionality

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (Params().NetworkID() == CBaseChainParams::MAIN) {
            if (pfrom->HasFulfilledRequest("mnget")) {
                LogPrint("masternode","mnget - peer already asked me for the list\n");
                Misbehaving(pfrom->GetId(), 20);
                return;
            }
        }

        pfrom->FulfilledRequest("mnget");
        masternodePayments.Sync(pfrom, nCountNeeded);
        LogPrint("mnpayments", "mnget - Sent Masternode winners to peer %i\n", pfrom->GetId());
    } else if (strCommand == "mnw") { //Masternode Payments Declare Winner
        //this is required in litemodef
        CMasternodePaymentWinner winner;
        vRecv >> winner;

        if (pfrom->nVersion < ActiveProtocol()) return;

        int nHeight;
        {
            TRY_LOCK(cs_main, locked);
            if (!locked || chainActive.Tip() == NULL) return;
            nHeight = chainActive.Tip()->nHeight;
        }

        if (masternodePayments.mapMasternodePayeeVotes.count(winner.GetHash())) {
            LogPrint("mnpayments", "mnw - Already seen - %s bestHeight %d\n", winner.GetHash().ToString().c_str(), nHeight);
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
            return;
        }

        int nFirstBlock = nHeight - (mnodeman.CountEnabled() * 1.25);
        if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > nHeight + 20) {
            LogPrint("mnpayments", "mnw - winner out of range - FirstBlock %d Height %d bestHeight %d\n", nFirstBlock, winner.nBlockHeight, nHeight);
            return;
        }

        std::string strError = "";
        if (!winner.IsValid(pfrom, strError)) {
            // if(strError != "") LogPrint("masternode","mnw - invalid message - %s\n", strError);
            return;
        }

        if (!masternodePayments.CanVote(winner.vinMasternode.prevout, winner.nBlockHeight)) {
            //  LogPrint("masternode","mnw - masternode already voted - %s\n", winner.vinMasternode.prevout.ToStringShort());
            return;
        }

        if (!winner.SignatureValid()) {
            // LogPrint("masternode","mnw - invalid signature\n");
            if (masternodeSync.IsSynced()) Misbehaving(pfrom->GetId(), 20);
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, winner.vinMasternode);
            return;
        }

        CTxDestination address1;
        ExtractDestination(winner.payee, address1);
        CBitcoinAddress address2(address1);

        //   LogPrint("mnpayments", "mnw - winning vote - Addr %s Height %d bestHeight %d - %s\n", address2.ToString().c_str(), winner.nBlockHeight, nHeight, winner.vinMasternode.prevout.ToStringShort());

        if (masternodePayments.AddWinningMasternode(winner)) {
            winner.Relay();
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
        }
    }
}

bool CMasternodePaymentWinner::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    std::string errorMessage;
    std::string strMasterNodeSignMessage;

    std::string strMessage = vinMasternode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             payee.ToString();

    if (!obfuScationSigner.SignMessage(strMessage, errorMessage, vchSig, keyMasternode)) {
        LogPrint("masternode","CMasternodePing::Sign() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    if (!obfuScationSigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint("masternode","CMasternodePing::Sign() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    return true;
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetPayee(payee);
    }

    return false;
}

// Is this masternode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 winners
bool CMasternodePayments::IsScheduled(CMasternode& mn, int nNotBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL) return false;
        nHeight = chainActive.Tip()->nHeight;
    }

    CScript mnpayee;
    mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = nHeight; h <= nHeight + 8; h++) {
        if (h == nNotBlockHeight) continue;
        if (mapMasternodeBlocks.count(h)) {
            if (mapMasternodeBlocks[h].GetPayee(payee)) {
                if (mnpayee == payee) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash = 0;
    if (!GetBlockHash(blockHash, winnerIn.nBlockHeight - 100)) {
        return false;
    }

    {
        LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

        if (mapMasternodePayeeVotes.count(winnerIn.GetHash())) {
            return false;
        }

        mapMasternodePayeeVotes[winnerIn.GetHash()] = winnerIn;

        if (!mapMasternodeBlocks.count(winnerIn.nBlockHeight)) {
            CMasternodeBlockPayees blockPayees(winnerIn.nBlockHeight);
            mapMasternodeBlocks[winnerIn.nBlockHeight] = blockPayees;
        }
    }

    mapMasternodeBlocks[winnerIn.nBlockHeight].AddPayee(winnerIn.payee, 1);

    return true;
}

bool CMasternodeBlockPayees::IsTransactionValid(const CTransaction& txNew, int nBlockVersion)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (!pindexPrev) {
        LogPrint("masternode", "%s: pindexPrev is nullptr", __func__);
        return false;
    }

    LOCK(cs_vecPayments);

    int nMaxSignatures = 0;
    std::string strPayeesPossible = "";

    //account for the fact that all peers do not see the same masternode count. An allowance of being off our masternode count is given
    //we only need to look at an increased masternode count because as count increases, the reward decreases. This code only checks
    //for mnPayment >= required, so it only makes sense to check the max node count allowed.
    const int nMasternodeCountStable = mnodeman.stable_size();
    const int nMasternodeCountDrift = nMasternodeCountStable + Params().MasternodeCountDrift();

    CAmount nReward = GetBlockValueReward(nBlockHeight);
    CAmount nRequiredMasternodePayment = GetMasternodePayment(nBlockHeight, nMasternodeCountDrift, pindexPrev->nMoneySupply);

    DebugPrintf("%s: Height=%d, Reward=%lld, MasternodePayment=%lld, MasternodeCountDrift=%d\n", __func__, nBlockHeight, nReward, nRequiredMasternodePayment, nMasternodeCountDrift);

    //require at least 6 signatures
    BOOST_FOREACH (CMasternodePayee& payee, vecPayments)
        if (payee.nVotes >= nMaxSignatures && payee.nVotes >= Params().GetMasternodePaymentSigRequired())
            nMaxSignatures = payee.nVotes;

    // if we don't have at least 6 signatures on a payee, approve whichever is the longest chain
    if (nMaxSignatures < Params().GetMasternodePaymentSigRequired())
        return true;

    BOOST_FOREACH (CMasternodePayee& payee, vecPayments) {
        bool found = false;
        BOOST_FOREACH (CTxOut out, txNew.vout) {
            if (payee.scriptPubKey == out.scriptPubKey) {
                if(out.nValue >= nRequiredMasternodePayment)
                    found = true;
                else
                    LogPrintf("Masternode payment is out of drift range. Paid=%s Min=%s\n", FormatMoney(out.nValue), FormatMoney(nRequiredMasternodePayment));
            }
        }

        if (payee.nVotes >= Params().GetMasternodePaymentSigRequired()) {
            if (found) return true;

            CTxDestination address1;
            ExtractDestination(payee.scriptPubKey, address1);
            CBitcoinAddress address2(address1);

            if (strPayeesPossible == "") {
                strPayeesPossible += address2.ToString();
            } else {
                strPayeesPossible += "," + address2.ToString();
            }
        }
    }

    return error("%s - Missing required payment of %s to %s. Block payees: %s\n", __func__, FormatMoney(nRequiredMasternodePayment), strPayeesPossible, GetRequiredPaymentsString());
}

std::string CMasternodeBlockPayees::GetRequiredPaymentsString()
{
    LOCK(cs_vecPayments);

    std::string ret = "Unknown";

    BOOST_FOREACH (CMasternodePayee& payee, vecPayments) {
        CTxDestination address1;
        ExtractDestination(payee.scriptPubKey, address1);
        CBitcoinAddress address2(address1);

        if (ret != "Unknown") {
            ret += ", " + address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.nVotes);
        } else {
            ret = address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.nVotes);
        }
    }

    return ret;
}

std::string CMasternodePayments::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CMasternodePayments::IsTransactionValid(const CTransaction& txNew, int nBlockVersion, int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].IsTransactionValid(txNew, nBlockVersion);
    } else {
        return true;
    }
}

void CMasternodePayments::CleanPaymentList()
{
    LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL) return;
        nHeight = chainActive.Tip()->nHeight;
    }

    //keep up to five cycles for historical sake
    int nLimit = std::max(int(mnodeman.size() * 1.25), 1000);

    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;

        if (nHeight - winner.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CMasternodePayments::CleanPaymentList - Removing old Masternode payment - block %d\n", winner.nBlockHeight);
            masternodeSync.mapSeenSyncMNW.erase((*it).first);
            mapMasternodePayeeVotes.erase(it++);
            mapMasternodeBlocks.erase(winner.nBlockHeight);
        } else {
            ++it;
        }
    }
}

bool CMasternodePaymentWinner::IsValid(CNode* pnode, std::string& strError)
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (!pmn) {
        strError = strprintf("Unknown Masternode %s", vinMasternode.prevout.hash.ToString());
        LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
        mnodeman.AskForMN(pnode, vinMasternode);
        return false;
    }

    if (pmn->protocolVersion < ActiveProtocol()) {
        strError = strprintf("Masternode protocol too old %d - req %d", pmn->protocolVersion, ActiveProtocol());
        LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    int n = mnodeman.GetMasternodeRank(vinMasternode, nBlockHeight - 100, ActiveProtocol());

    if (n > Params().GetMasternodePaymentSigTotal()) {
        //It's common to have masternodes mistakenly think they are in the top 10
        // We don't want to print all of these messages, or punish them unless they're way off
        if (n > Params().GetMasternodePaymentSigTotal() * 2) {
            strError = strprintf("Masternode not in the top %d (%d)", Params().GetMasternodePaymentSigTotal() * 2, n);
            LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
            //if (masternodeSync.IsSynced()) Misbehaving(pnode->GetId(), 20); this ban split network and leads to forks
        }
        return false;
    } else
        return true;
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    if (!fMasterNode) return false;

    //reference node - hybrid mode

    int n = mnodeman.GetMasternodeRank(activeMasternode.vin, nBlockHeight - 100, ActiveProtocol());

    if (n == -1) {
        LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Unknown Masternode\n");
        return false;
    }

    //DRAGAN: //if (n > MNPAYMENTS_SIGNATURES_TOTAL) { LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Masternode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, n);
    if (n > Params().GetMasternodePaymentSigTotal()) {
        LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Masternode not in the top %d (%d)\n", Params().GetMasternodePaymentSigTotal(), n);
        return false;
    }

    if (nBlockHeight <= nLastBlockHeight) return false;

    CMasternodePaymentWinner newWinner(activeMasternode.vin);

    if (budget.IsBudgetPaymentBlock(nBlockHeight)) {
        //is budget payment block -- handled by the budgeting software
    } else {
        LogPrint("masternode","CMasternodePayments::ProcessBlock() Start nHeight %d - vin %s. \n", nBlockHeight, activeMasternode.vin.prevout.hash.ToString());

        // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
        int nCount = 0;
        CMasternode* pmn = mnodeman.GetNextMasternodeInQueueForPayment(nBlockHeight, true, nCount);

        if (pmn != NULL) {
            LogPrint("masternode","CMasternodePayments::ProcessBlock() Found by FindOldestNotInVec \n");

            newWinner.nBlockHeight = nBlockHeight;

            CScript payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());
            newWinner.AddPayee(payee);

            CTxDestination address1;
            ExtractDestination(payee, address1);
            CBitcoinAddress address2(address1);

            LogPrint("masternode","CMasternodePayments::ProcessBlock() Winner payee %s nHeight %d. \n", address2.ToString().c_str(), newWinner.nBlockHeight);
        } else {
            LogPrint("masternode","CMasternodePayments::ProcessBlock() Failed to find masternode to pay\n");
        }
    }

    std::string errorMessage;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!obfuScationSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode)) {
        LogPrint("masternode","CMasternodePayments::ProcessBlock() - Error upon calling SetKey: %s\n", errorMessage.c_str());
        return false;
    }

    LogPrint("masternode","CMasternodePayments::ProcessBlock() - Signing Winner\n");
    if (newWinner.Sign(keyMasternode, pubKeyMasternode)) {
        LogPrint("masternode","CMasternodePayments::ProcessBlock() - AddWinningMasternode\n");

        if (AddWinningMasternode(newWinner)) {
            newWinner.Relay();
            nLastBlockHeight = nBlockHeight;
            return true;
        }
    }

    return false;
}

void CMasternodePaymentWinner::Relay()
{
    CInv inv(MSG_MASTERNODE_WINNER, GetHash());
    RelayInv(inv);
}

bool CMasternodePaymentWinner::SignatureValid()
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (pmn != NULL) {
        std::string strMessage = vinMasternode.prevout.ToStringShort() +
                                 boost::lexical_cast<std::string>(nBlockHeight) +
                                 payee.ToString();

        std::string errorMessage = "";
        if (!obfuScationSigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
            return error("CMasternodePaymentWinner::SignatureValid() - Got bad Masternode address signature %s\n", vinMasternode.prevout.hash.ToString());
        }

        return true;
    }

    return false;
}

void CMasternodePayments::Sync(CNode* node, int nCountNeeded)
{
    LOCK(cs_mapMasternodePayeeVotes);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL) return;
        nHeight = chainActive.Tip()->nHeight;
    }

    int nCount = (mnodeman.CountEnabled() * 1.25);
    if (nCountNeeded > nCount) nCountNeeded = nCount;

    int nInvCount = 0;
    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;
        if (winner.nBlockHeight >= nHeight - nCountNeeded && winner.nBlockHeight <= nHeight + 20) {
            node->PushInventory(CInv(MSG_MASTERNODE_WINNER, winner.GetHash()));
            nInvCount++;
        }
        ++it;
    }
    node->PushMessage("ssc", MASTERNODE_SYNC_MNW, nInvCount);
}

std::string CMasternodePayments::ToString() const
{
    std::ostringstream info;

    info << "Votes: " << (int)mapMasternodePayeeVotes.size() << ", Blocks: " << (int)mapMasternodeBlocks.size();

    return info.str();
}


int CMasternodePayments::GetOldestBlock()
{
    LOCK(cs_mapMasternodeBlocks);

    int nOldestBlock = std::numeric_limits<int>::max();

    std::map<int, CMasternodeBlockPayees>::iterator it = mapMasternodeBlocks.begin();
    while (it != mapMasternodeBlocks.end()) {
        if ((*it).first < nOldestBlock) {
            nOldestBlock = (*it).first;
        }
        it++;
    }

    return nOldestBlock;
}


int CMasternodePayments::GetNewestBlock()
{
    LOCK(cs_mapMasternodeBlocks);

    int nNewestBlock = 0;

    std::map<int, CMasternodeBlockPayees>::iterator it = mapMasternodeBlocks.begin();
    while (it != mapMasternodeBlocks.end()) {
        if ((*it).first > nNewestBlock) {
            nNewestBlock = (*it).first;
        }
        it++;
    }

    return nNewestBlock;
}
