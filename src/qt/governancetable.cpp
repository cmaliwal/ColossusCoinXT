// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "governancetable.h"
#include "governancetablemodel.h"
#include "tinyformat.h"

#include <QLabel>
#include <QCheckBox>
#include <QLineEdit>
#include <QTableView>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMenu>

GovernanceTable::GovernanceTable(QWidget* parent):
    QWidget(parent)
{
    memset(&ui, 0, sizeof(ui));

    setupUI();
    setupLayout();
}

GovernanceTable::~GovernanceTable()
{}

void GovernanceTable::setupUI()
{
    if (ui.labelTitle) // must be nullptr
        throw std::runtime_error(strprintf("%s: ui has already been initialized", __func__));

    ui.labelTitle = new QLabel(this);
    ui.labelTitle->setText(tr("GOVERNANCE"));
    ui.labelTitle->setMinimumSize(QSize(464, 60));
    QFont font;
    font.setPointSize(20);
    font.setBold(true);
    font.setWeight(75);
    ui.labelTitle->setFont(font);

    ui.labelNote = new QLabel(this);
    ui.labelNote->setText(tr("Note: Governance objects in your local wallet can be potentially incorrect. Always wait for wallet sync and additional data before voting on any proposal."));

    ui.labelSearch = new QLabel(this);
    ui.labelSearch->setText(tr("Search:"));

    ui.editSearch = new QLineEdit(this);

    ui.showPrevious = new QCheckBox(this);
    ui.showPrevious->setText(tr("Show previous proposals"));

    ui.tableProposal = new QTableView(this);
    ui.tableProposal->setSelectionBehavior(QAbstractItemView::SelectRows);

    ui.voteYes = new QPushButton(this);
    ui.voteYes->setText(tr("Vote Yes"));

    ui.voteNo = new QPushButton(this);
    ui.voteNo->setText(tr("Vote No"));

    ui.voteAbstain = new QPushButton(this);
    ui.voteAbstain->setText(tr("Vote Abstain"));

    ui.btnUpdateTable = new QPushButton(this);
    ui.btnUpdateTable->setText(tr("Update Governance"));

    ui.menu = new QMenu(this);
}

void GovernanceTable::updateUI()
{
    if (!model_) {
        ui.tableProposal->setModel(nullptr);
    } else {
        ui.tableProposal->setModel(model_.get());
    }
}

void GovernanceTable::setupLayout()
{
    QHBoxLayout *layoutSearch = new QHBoxLayout;
    layoutSearch->addWidget(ui.labelSearch);
    layoutSearch->addWidget(ui.editSearch);
    layoutSearch->addWidget(ui.showPrevious);

    QHBoxLayout *layoutButtons = new QHBoxLayout;
    layoutButtons->addWidget(ui.voteYes);
    layoutButtons->addWidget(ui.voteNo);
    layoutButtons->addWidget(ui.voteAbstain);
    layoutButtons->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding, QSizePolicy::Fixed));
    layoutButtons->addWidget(ui.btnUpdateTable);

    QVBoxLayout *layoutTop = new QVBoxLayout;
    layoutTop->addWidget(ui.labelNote);
    layoutTop->addItem(new QSpacerItem(1, 10, QSizePolicy::Expanding, QSizePolicy::Fixed));
    layoutTop->addLayout(layoutSearch);
    layoutTop->addWidget(ui.tableProposal);
    layoutTop->addLayout(layoutButtons);
    layoutTop->setContentsMargins(10, 10, 10, 10);

    // Combine all
    QVBoxLayout *layoutMain = new QVBoxLayout;
    layoutMain->addWidget(ui.labelTitle);
    layoutMain->addLayout(layoutTop);
    layoutMain->setContentsMargins(10, 10, 10, 10);
    this->setLayout(layoutMain);
}

void GovernanceTable::setModel(GovernanceTableModelPtr model)
{
    model_ = model;
    updateUI();
}
