// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeentrydialog.h"
#include "guiutil.h"
#include "guiconstants.h"
#include "tinyformat.h"
#include "base58.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDebug>

using namespace std;

MasternodeEntryDialog::MasternodeEntryDialog(
    const std::vector<MasternodeOutput>& outputList,
    QWidget *parent):
    QDialog(parent),
    outputList_(outputList)
{
    this->setWindowTitle(tr("Masternode Entry"));

    memset(&ui, 0, sizeof(ui));
    setupUI();
    setupLayout();
}

MasternodeEntryDialog::MasternodeEntryDialog(
    const std::string& alias,
    const std::string& ip,
    const std::string& pk,
    const std::vector<MasternodeOutput>& outputList,
    QWidget *parent):
    QDialog(parent),
    outputList_(outputList)
{
    this->setWindowTitle(tr("Masternode Entry - %1").arg(alias.c_str()));

    memset(&ui, 0, sizeof(ui));
    setupUI();
    setupLayout();

    ui.eAlias->setText(alias.c_str());
    ui.eIP->setText(ip.c_str());
    ui.ePK->setText(pk.c_str());
}

MasternodeEntryDialog::~MasternodeEntryDialog()
{}

void MasternodeEntryDialog::setBusyAliases(const std::set<std::string>& aliases)
{
    busyAliases_ = aliases;
}

void MasternodeEntryDialog::setupUI()
{
    if (ui.labelAlias) // must be nullptr
        throw std::runtime_error("MasternodeEntryDialog: ui has already been initialized");

    const int WIDTH = 140;
    const char *stylesheet = "QLabel { color : black; }";

    ui.labelAlias = new QLabel(this);
    ui.labelAlias->setFixedWidth(WIDTH);
    ui.labelAlias->setText(tr("Alias:"));
    ui.labelAlias->setStyleSheet(stylesheet);

    ui.eAlias = new QLineEdit(this);
    ui.eAlias->setMaxLength(100);

    ui.labelIP = new QLabel(this);
    ui.labelIP->setFixedWidth(WIDTH);
    ui.labelIP->setText(tr("IP address:"));
    ui.labelIP->setStyleSheet(stylesheet);

    ui.eIP = new QLineEdit(this);
    ui.eIP->setToolTip(tr("IP:port"));

    ui.labelPK = new QLabel(this);
    ui.labelPK->setFixedWidth(WIDTH);
    ui.labelPK->setText(tr("Masternode private key:"));
    ui.labelPK->setStyleSheet(stylesheet);

    ui.ePK = new QLineEdit(this);
    ui.ePK->setMaxLength(100);

    ui.labelOutput = new QLabel(this);
    ui.labelOutput->setFixedWidth(WIDTH);
    ui.labelOutput->setText(tr("Collateral output:"));
    ui.labelOutput->setStyleSheet(stylesheet);

    ui.comboOutput = new QComboBox(this);
    for (const MasternodeOutput& out : outputList_)
        ui.comboOutput->addItem(QString("%1:%2").arg(out.txHash.c_str()).arg(out.index));

    if (outputList_.empty()) {
        ui.comboOutput->setEnabled(false);
        ui.comboOutput->setCurrentText(tr("Not found"));
    } else
        ui.comboOutput->setCurrentIndex(0);

    ui.btnOk = new QPushButton(this);
    ui.btnOk->setText(tr("Ok"));

    ui.btnCancel = new QPushButton(this);
    ui.btnCancel->setAutoDefault(false);
    ui.btnCancel->setText(tr("Cancel"));

    connect(ui.btnOk, SIGNAL (released()), this, SLOT (onButtonOk()));
    connect(ui.btnCancel, SIGNAL (released()), this, SLOT (onButtonCancel()));
}

void MasternodeEntryDialog::setupLayout()
{
    this->setFixedWidth(650);

    QHBoxLayout *layoutAlias = new QHBoxLayout;
    layoutAlias->addWidget(ui.labelAlias);
    layoutAlias->addWidget(ui.eAlias);

    QHBoxLayout *layoutIP = new QHBoxLayout;
    layoutIP->addWidget(ui.labelIP);
    layoutIP->addWidget(ui.eIP);

    QHBoxLayout *layoutPK = new QHBoxLayout;
    layoutPK->addWidget(ui.labelPK);
    layoutPK->addWidget(ui.ePK);

    QHBoxLayout *layoutOutput = new QHBoxLayout;
    layoutOutput->addWidget(ui.labelOutput);
    layoutOutput->addWidget(ui.comboOutput);

    QHBoxLayout *layoutButtons = new QHBoxLayout;
    layoutButtons->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding, QSizePolicy::Fixed));
    layoutButtons->addWidget(ui.btnOk);
    layoutButtons->addWidget(ui.btnCancel);

    // Combine all
    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->addLayout(layoutAlias);
    mainLayout->addLayout(layoutIP);
    mainLayout->addLayout(layoutPK);
    mainLayout->addLayout(layoutOutput);
    mainLayout->addLayout(layoutButtons);
    this->setLayout(mainLayout);
}

void MasternodeEntryDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
}

void MasternodeEntryDialog::closeEvent(QCloseEvent *event)
{
    QDialog::closeEvent(event);
}

void MasternodeEntryDialog::onButtonOk()
{
    if (getAlias().empty()) {
        ShowError(tr("Alias is required."));
        return;
    }

    if (busyAliases_.count(getAlias()) > 0) {
        ShowError(tr("Alias is busy."));
        return;
    }

    if (getIP().empty()) {
        ShowError(tr("IP address is required."));
        return;
    }

    const string pk = getPrivateKey();
    if (pk.empty()) {
        ShowError(tr("Private key is required."));
        return;
    }

    CBitcoinSecret vchSecret;
    if (!vchSecret.SetString(pk)) {
        ShowError(tr("Private key is not valid."));
        return;
    }

    accept();
}

void MasternodeEntryDialog::onButtonCancel()
{
    reject();
}

void MasternodeEntryDialog::ShowError(const QString& msg)
{
    if (msg.isEmpty())
        return;
    else
        QMessageBox::warning(this, this->windowTitle(), msg, QMessageBox::Ok, QMessageBox::Ok);
}

std::string MasternodeEntryDialog::getAlias() const
{
    return ui.eAlias->text().trimmed().toStdString();
}

std::string MasternodeEntryDialog::getIP() const
{
    return ui.eIP->text().trimmed().toStdString();
}

std::string MasternodeEntryDialog::getPrivateKey() const
{
    return ui.ePK->text().trimmed().toStdString();
}

int MasternodeEntryDialog::getOutputIndex() const
{
    return  ui.comboOutput->currentIndex();
}
