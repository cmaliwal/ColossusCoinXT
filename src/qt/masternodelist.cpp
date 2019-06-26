#include "masternodelist.h"
#include "ui_masternodelist.h"

#include "activemasternode.h"
#include "clientmodel.h"
#include "guiutil.h"
#include "init.h"
#include "masternode-sync.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "sync.h"
#include "wallet.h"
#include "walletmodel.h"
#include "askpassphrasedialog.h"
#include "masternodeentrydialog.h"

#include <QMessageBox>
#include <QTimer>

CCriticalSection cs_masternodes;

static std::vector<MasternodeOutput> FindAvailableMasternodeOutputs()
{
    set<string> busyOutputs;
    for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries())
        busyOutputs.insert(strprintf("%s:%s", mne.getTxHash(), mne.getOutputIndex()));

    vector<MasternodeOutput> outputList;
    vector<COutput> possibleCoins = activeMasternode.SelectCoinsMasternode();
    for (const COutput& out : possibleCoins)
        if (busyOutputs.find(strprintf("%s:%d", out.tx->GetHash().ToString(), out.i)) == busyOutputs.end())
            outputList.emplace_back(out.tx->GetHash().ToString(), out.i);

    return outputList;
}

MasternodeList::MasternodeList(QWidget* parent) : QWidget(parent),
                                                  ui(new Ui::MasternodeList),
                                                  clientModel(0),
                                                  walletModel(0)
{
    ui->setupUi(this);

    ui->startButton->setEnabled(false);

    int columnAliasWidth = 100;
    int columnAddressWidth = 200;
    int columnProtocolWidth = 60;
    int columnStatusWidth = 80;
    int columnActiveWidth = 130;
    int columnLastSeenWidth = 130;

    ui->tableWidgetMyMasternodes->setAlternatingRowColors(true);
    ui->tableWidgetMyMasternodes->setColumnWidth(0, columnAliasWidth);
    ui->tableWidgetMyMasternodes->setColumnWidth(1, columnAddressWidth);
    ui->tableWidgetMyMasternodes->setColumnWidth(2, columnProtocolWidth);
    ui->tableWidgetMyMasternodes->setColumnWidth(3, columnStatusWidth);
    ui->tableWidgetMyMasternodes->setColumnWidth(4, columnActiveWidth);
    ui->tableWidgetMyMasternodes->setColumnWidth(5, columnLastSeenWidth);

    ui->tableWidgetMyMasternodes->setContextMenuPolicy(Qt::CustomContextMenu);

    QAction* startAliasAction = new QAction(tr("Start alias"), this);
    QAction* modifyAction = new QAction(tr("Modify"), this);
    QAction* deleteAction = new QAction(tr("Delete"), this);

    contextMenu = new QMenu();
    contextMenu->addAction(startAliasAction);
    contextMenu->addAction(modifyAction);
    contextMenu->addAction(deleteAction);

    connect(ui->tableWidgetMyMasternodes, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(showContextMenu(const QPoint&)));
    connect(startAliasAction, SIGNAL(triggered()), this, SLOT(on_startButton_clicked()));
    connect(modifyAction, SIGNAL(triggered()), this, SLOT(on_modifyButton_clicked()));
    connect(deleteAction, SIGNAL(triggered()), this, SLOT(on_deleteButton_clicked()));

    connect(ui->addButton, SIGNAL(triggered()), this, SLOT(on_addButton_clicked()));
    connect(ui->modifyButton, SIGNAL(triggered()), this, SLOT(on_modifyButton_clicked()));
    connect(ui->deleteButton, SIGNAL(triggered()), this, SLOT(on_deleteButton_clicked()));

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateMyNodeList()));
    timer->start(1000);

    // Fill MN list
    fFilterUpdated = true;
    nTimeFilterUpdated = GetTime();
}

MasternodeList::~MasternodeList()
{
    delete ui;
}

void MasternodeList::setClientModel(ClientModel* model)
{
    this->clientModel = model;
}

void MasternodeList::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
}

void MasternodeList::showContextMenu(const QPoint& point)
{
    QTableWidgetItem* item = ui->tableWidgetMyMasternodes->itemAt(point);
    if (item) contextMenu->exec(QCursor::pos());
}

void MasternodeList::StartAlias(std::string strAlias)
{
    std::string strStatusHtml;
    strStatusHtml += "<center>Alias: " + strAlias;

    BOOST_FOREACH (CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
        if (mne.getAlias() == strAlias) {
            std::string strError;
            CMasternodeBroadcast mnb;

            bool fSuccess = CMasternodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb);

            if (fSuccess) {
                strStatusHtml += "<br>Successfully started masternode.";
                mnodeman.UpdateMasternodeList(mnb);
                mnb.Relay();
            } else {
                strStatusHtml += "<br>Failed to start masternode.<br>Error: " + strError;
            }
            break;
        }
    }
    strStatusHtml += "</center>";

    QMessageBox msg;
    msg.setText(QString::fromStdString(strStatusHtml));
    msg.exec();

    updateMyNodeList(true);
}

void MasternodeList::StartAll(std::string strCommand)
{
    int nCountSuccessful = 0;
    int nCountFailed = 0;
    std::string strFailedHtml;

    BOOST_FOREACH (CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
        std::string strError;
        CMasternodeBroadcast mnb;

        int nIndex;
        if(!mne.castOutputIndex(nIndex))
            continue;

        CTxIn txin = CTxIn(uint256S(mne.getTxHash()), uint32_t(nIndex));
        CMasternode* pmn = mnodeman.Find(txin);

        if (strCommand == "start-missing" && pmn) continue;

        bool fSuccess = CMasternodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb);

        if (fSuccess) {
            nCountSuccessful++;
            mnodeman.UpdateMasternodeList(mnb);
            mnb.Relay();
        } else {
            nCountFailed++;
            strFailedHtml += "\nFailed to start " + mne.getAlias() + ". Error: " + strError;
        }
    }
    pwalletMain->Lock();

    std::string returnObj;
    returnObj = strprintf("Successfully started %d masternodes, failed to start %d, total %d", nCountSuccessful, nCountFailed, nCountFailed + nCountSuccessful);
    if (nCountFailed > 0) {
        returnObj += strFailedHtml;
    }

    QMessageBox msg;
    msg.setText(QString::fromStdString(returnObj));
    msg.exec();

    updateMyNodeList(true);
}

void MasternodeList::updateMyMasternodeInfo(QString strAlias, QString strAddr, CMasternode* pmn)
{
    LOCK(cs_mnlistupdate);
    bool fOldRowFound = false;
    int nNewRow = 0;

    for (int i = 0; i < ui->tableWidgetMyMasternodes->rowCount(); i++) {
        if (ui->tableWidgetMyMasternodes->item(i, 0)->text() == strAlias) {
            fOldRowFound = true;
            nNewRow = i;
            break;
        }
    }

    if (nNewRow == 0 && !fOldRowFound) {
        nNewRow = ui->tableWidgetMyMasternodes->rowCount();
        ui->tableWidgetMyMasternodes->insertRow(nNewRow);
    }

    QTableWidgetItem* aliasItem = new QTableWidgetItem(strAlias);
    QTableWidgetItem* addrItem = new QTableWidgetItem(pmn ? QString::fromStdString(pmn->addr.ToString()) : strAddr);
    QTableWidgetItem* protocolItem = new QTableWidgetItem(QString::number(pmn ? pmn->protocolVersion : -1));
    QTableWidgetItem* statusItem = new QTableWidgetItem(QString::fromStdString(pmn ? pmn->GetStatus() : "MISSING"));
    GUIUtil::DHMSTableWidgetItem* activeSecondsItem = new GUIUtil::DHMSTableWidgetItem(pmn ? (pmn->lastPing.sigTime - pmn->sigTime) : 0);
    QTableWidgetItem* lastSeenItem = new QTableWidgetItem(QString::fromStdString(DateTimeStrFormat("%Y-%m-%d %H:%M", pmn ? pmn->lastPing.sigTime : 0)));
    QTableWidgetItem* pubkeyItem = new QTableWidgetItem(QString::fromStdString(pmn ? CBitcoinAddress(pmn->pubKeyCollateralAddress.GetID()).ToString() : ""));

    ui->tableWidgetMyMasternodes->setItem(nNewRow, 0, aliasItem);
    ui->tableWidgetMyMasternodes->setItem(nNewRow, 1, addrItem);
    ui->tableWidgetMyMasternodes->setItem(nNewRow, 2, protocolItem);
    ui->tableWidgetMyMasternodes->setItem(nNewRow, 3, statusItem);
    ui->tableWidgetMyMasternodes->setItem(nNewRow, 4, activeSecondsItem);
    ui->tableWidgetMyMasternodes->setItem(nNewRow, 5, lastSeenItem);
    ui->tableWidgetMyMasternodes->setItem(nNewRow, 6, pubkeyItem);
}

void MasternodeList::updateMyNodeList(bool fForce)
{
    static int64_t nTimeMyListUpdated = 0;

    // automatically update my masternode list only once in MY_MASTERNODELIST_UPDATE_SECONDS seconds,
    // this update still can be triggered manually at any time via button click
    int64_t nSecondsTillUpdate = nTimeMyListUpdated + MY_MASTERNODELIST_UPDATE_SECONDS - GetTime();
    ui->secondsLabel->setText(QString::number(nSecondsTillUpdate));

    if (nSecondsTillUpdate > 0 && !fForce) return;
    nTimeMyListUpdated = GetTime();

    ui->tableWidgetMyMasternodes->setSortingEnabled(false);
    BOOST_FOREACH (CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
        int nIndex;
        if(!mne.castOutputIndex(nIndex))
            continue;

        CTxIn txin = CTxIn(uint256S(mne.getTxHash()), uint32_t(nIndex));
        CMasternode* pmn = mnodeman.Find(txin);
        updateMyMasternodeInfo(QString::fromStdString(mne.getAlias()), QString::fromStdString(mne.getIp()), pmn);
    }
    ui->tableWidgetMyMasternodes->setSortingEnabled(true);

    // reset "timer"
    ui->secondsLabel->setText("0");
}

void MasternodeList::LockCoin(
        bool lock,
        const std::string& txHash,
        const std::string& outputIndex) const
{
    LOCK(pwalletMain->cs_wallet);
    LogPrintf("%s - Locking/Unlocking(%d) Masternode Collateral: %s %s\n", __func__, lock, txHash, outputIndex);
    uint256 mnTxHash;
    mnTxHash.SetHex(txHash);
    COutPoint outpoint = COutPoint(mnTxHash, boost::lexical_cast<unsigned int>(outputIndex));
    if (lock)
        pwalletMain->LockCoin(outpoint);
    else
        pwalletMain->UnlockCoin(outpoint);
}

void MasternodeList::on_addButton_clicked()
{
    vector<MasternodeOutput> outputList = FindAvailableMasternodeOutputs();
    if (outputList.empty()) {
        QMessageBox::warning(this, this->windowTitle(), tr("Masternode outputs were not found."), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    set<std::string> aliases;
    for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries())
        aliases.insert(mne.getAlias());

    MasternodeEntryDialog dlg(outputList, this);
    dlg.setBusyAliases(aliases);
    int answer = dlg.exec();
    if (QDialog::Accepted == answer) {
        int txIndex = dlg.getOutputIndex();
        if (txIndex >= 0 && txIndex < outputList.size()) {
            const MasternodeOutput& out = outputList.at(txIndex);
            CMasternodeConfig::CMasternodeEntry mne(dlg.getAlias(), dlg.getIP(), dlg.getPrivateKey(), out.txHash, to_string(out.index));
            if (masternodeConfig.addEntry(mne)) {
                updateMyNodeList(true);
                if (GetBoolArg("-mnconflock", true) && pwalletMain)
                    LockCoin(true, mne.getTxHash(), mne.getOutputIndex());
            }
        }
    }
}

void MasternodeList::on_modifyButton_clicked()
{
    // Find selected node alias
    QItemSelectionModel* selectionModel = ui->tableWidgetMyMasternodes->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    if (selected.empty())
        return;

    QModelIndex index = selected.at(0);
    int nSelectedRow = index.row();
    std::string strAlias = ui->tableWidgetMyMasternodes->item(nSelectedRow, 0)->text().toStdString();
    CMasternodeConfig::CMasternodeEntry mne = masternodeConfig.findEntry(strAlias);
    if (!mne.isValid()) {
        QMessageBox::critical(this, tr("Error"), tr("%1 was not found.").arg(QString::fromStdString(strAlias)));
        return;
    }

    set<std::string> aliases;
    for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries())
        if (mne.getAlias() != strAlias)
            aliases.insert(mne.getAlias());

    vector<MasternodeOutput> outputList = FindAvailableMasternodeOutputs();
    outputList.insert(outputList.begin(), {mne.getTxHash(), stoi(mne.getOutputIndex())}); // zero index is our mne
    MasternodeEntryDialog dlg(mne.getAlias(), mne.getIp(), mne.getPrivKey(), outputList, this);
    dlg.setBusyAliases(aliases);
    int answer = dlg.exec();
    if (QDialog::Accepted == answer) {
        int txIndex = dlg.getOutputIndex();
        if (txIndex >= 0 && txIndex < outputList.size()) {
            const MasternodeOutput& out = outputList.at(txIndex);
            CMasternodeConfig::CMasternodeEntry mne(dlg.getAlias(), dlg.getIP(), dlg.getPrivateKey(), out.txHash, to_string(out.index));
            if (masternodeConfig.modifyEntry(strAlias, mne)) {
                if (strAlias != mne.getAlias())
                    ui->tableWidgetMyMasternodes->setItem(nSelectedRow, 0, new QTableWidgetItem(mne.getAlias().c_str()));

                updateMyNodeList(true);

                if (txIndex > 0 && GetBoolArg("-mnconflock", true) && pwalletMain) {
                    LockCoin(false, outputList.front().txHash, to_string(outputList.front().index)); // unlock old tx
                    LockCoin(true, mne.getTxHash(), mne.getOutputIndex()); // lock new tx
                }
            }
        }
    }
}

void MasternodeList::on_deleteButton_clicked()
{
    // Find selected node alias
    QItemSelectionModel* selectionModel = ui->tableWidgetMyMasternodes->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    if (selected.empty())
        return;

    QModelIndex index = selected.at(0);
    int nSelectedRow = index.row();
    std::string strAlias = ui->tableWidgetMyMasternodes->item(nSelectedRow, 0)->text().toStdString();

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm masternode delete"),
        tr("Are you sure you want to delete masternode %1?").arg(QString::fromStdString(strAlias)),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (retval != QMessageBox::Yes)
        return;

    CMasternodeConfig::CMasternodeEntry mne = masternodeConfig.findEntry(strAlias);
    if (masternodeConfig.deleteEntry(strAlias)) {
        ui->tableWidgetMyMasternodes->removeRow(nSelectedRow);
        if (GetBoolArg("-mnconflock", true) && pwalletMain && mne.isValid())
            LockCoin(false, mne.getTxHash(), mne.getOutputIndex());
    }
}

void MasternodeList::on_startButton_clicked()
{
    // Find selected node alias
    QItemSelectionModel* selectionModel = ui->tableWidgetMyMasternodes->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();

    if (selected.count() == 0) return;

    QModelIndex index = selected.at(0);
    int nSelectedRow = index.row();
    std::string strAlias = ui->tableWidgetMyMasternodes->item(nSelectedRow, 0)->text().toStdString();

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm masternode start"),
        tr("Are you sure you want to start masternode %1?").arg(QString::fromStdString(strAlias)),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if (encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForAnonymizationOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock(AskPassphraseDialog::Context::Unlock_Full));

        if (!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAlias(strAlias);
        return;
    }

    StartAlias(strAlias);
}

void MasternodeList::on_startAllButton_clicked()
{
    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm all masternodes start"),
        tr("Are you sure you want to start ALL masternodes?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if (encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForAnonymizationOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock(AskPassphraseDialog::Context::Unlock_Full));

        if (!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAll();
        return;
    }

    StartAll();
}

void MasternodeList::on_startMissingButton_clicked()
{
    if (!masternodeSync.IsMasternodeListSynced()) {
        QMessageBox::critical(this, tr("Command is not available right now"),
            tr("You can't use this command until masternode list is synced"));
        return;
    }

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this,
        tr("Confirm missing masternodes start"),
        tr("Are you sure you want to start MISSING masternodes?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if (encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForAnonymizationOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock(AskPassphraseDialog::Context::Unlock_Full));

        if (!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAll("start-missing");
        return;
    }

    StartAll("start-missing");
}

void MasternodeList::on_tableWidgetMyMasternodes_itemSelectionChanged()
{
    if (ui->tableWidgetMyMasternodes->selectedItems().count() > 0) {
        ui->startButton->setEnabled(true);
    }
}

void MasternodeList::on_UpdateButton_clicked()
{
    updateMyNodeList(true);
}
