// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "governancetable.h"
#include "governancetablemodel.h"
#include "ui_interface.h"
#include "tinyformat.h"
#include "guiutil.h"

#include <QMenu>
#include <QLabel>
#include <QCheckBox>
#include <QLineEdit>
#include <QTableView>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QModelIndex>
#include <QDebug>

static GovernanceTable *gView = nullptr;
static const char *stylesheet = "QPushButton {background-color: transparent;} QPushButton:hover { background-color:qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: .01 #346337, stop: .1 #38693b, stop: .95 #457c49, stop: 1 #4b814f); }";

static void BlockNotifyCallback(const uint256& hashNewTip)
{
    qDebug() << "BlockNotifyCallback()";
    if (gView)
        QMetaObject::invokeMethod(gView, "onBlockNotify", Qt::QueuedConnection);
}

GovernanceTable::GovernanceTable(QWidget* parent):
    QWidget(parent)
{
    memset(&ui, 0, sizeof(ui));

    setupUI();
    setupLayout();
    updateUI();

    gView = this;
    uiInterface.NotifyBlockTip.connect(BlockNotifyCallback);
}

GovernanceTable::~GovernanceTable()
{
    uiInterface.NotifyBlockTip.disconnect(BlockNotifyCallback);
    gView = nullptr;
}

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
    ui.tableProposal->setSelectionMode(QAbstractItemView::SingleSelection);
    ui.tableProposal->setContextMenuPolicy(Qt::CustomContextMenu);

    ui.voteYes = new QPushButton(this);
    ui.voteYes->setText(tr("Vote Yes"));

    ui.voteNo = new QPushButton(this);
    ui.voteNo->setText(tr("Vote No"));

    ui.voteAbstain = new QPushButton(this);
    ui.voteAbstain->setText(tr("Vote Abstain"));

    ui.btnUpdateTable = new QPushButton(this);
    ui.btnUpdateTable->setText(tr("Update Governance"));

    QAction* actionVoteYes = new QAction(tr("Vote Yes"), this);
    QAction* actionVoteNo = new QAction(tr("Vote No"), this);
    QAction* actionVoteAbstain = new QAction(tr("Vote Abstain"), this);
    QAction* actionUrl = new QAction(tr("Open URL"), this);
    QAction* actionInfo = new QAction(tr("Show Information"), this);

    ui.menu = new QMenu(this);
    ui.menu->addAction(actionVoteYes);
    ui.menu->addAction(actionVoteNo);
    ui.menu->addAction(actionVoteAbstain);
    ui.menu->addAction(actionUrl);
    ui.menu->addAction(actionInfo);

    connect(ui.tableProposal,
            SIGNAL(customContextMenuRequested(const QPoint&)),
            this,
            SLOT(onShowTableContextMenu(const QPoint&)));

    connect(ui.showPrevious, SIGNAL(clicked(bool)), this, SLOT(onShowPrevious(bool)));
    connect(ui.editSearch, SIGNAL(textEdited(const QString&)), this, SLOT(onSearch(const QString&)));

    connect(actionVoteYes, SIGNAL(triggered()), this, SLOT(onVoteYes()));
    connect(actionVoteNo, SIGNAL(triggered()), this, SLOT(onVoteNo()));
    connect(actionVoteAbstain, SIGNAL(triggered()), this, SLOT(onVoteAbstain()));
    connect(actionUrl, SIGNAL(triggered()), this, SLOT(onUrl()));
    connect(actionInfo, SIGNAL(triggered()), this, SLOT(onShowInfo()));

    connect(ui.voteYes, SIGNAL (released()), this, SLOT (onVoteYes()));
    connect(ui.voteNo, SIGNAL (released()), this, SLOT (onVoteNo()));
    connect(ui.voteAbstain, SIGNAL (released()), this, SLOT (onVoteAbstain()));
    connect(ui.btnUpdateTable, SIGNAL (released()), this, SLOT (onUpdateTable()));
}

void GovernanceTable::updateUI()
{
    if (!model_) {
        ui.tableProposal->setModel(nullptr);
        ui.btnUpdateTable->setEnabled(false);
        ui.voteYes->setEnabled(false);
        ui.voteNo->setEnabled(false);
        ui.voteAbstain->setEnabled(false);
    } else {
        ui.tableProposal->setModel(model_.get());
        ui.tableProposal->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        std::vector<int> columnWidth = model_->columnWidth();
        for (int i = 0; i < columnWidth.size(); ++i)
            ui.tableProposal->setColumnWidth(i, columnWidth[i]);

        connect(ui.tableProposal->selectionModel(),
                SIGNAL(currentRowChanged(const QModelIndex&, const QModelIndex&)),
                this,
                SLOT(onTableRowChanged(const QModelIndex&, const QModelIndex&)));

        ui.btnUpdateTable->setEnabled(true);
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

void GovernanceTable::updateModel()
{
    if (model_) {
        model_->updateModel();
        for (int i = 0; i < ui.tableProposal->model()->rowCount(); ++i) {
            QModelIndex index1 = ui.tableProposal->model()->index(i, GovernanceTableModel::link);
            ui.tableProposal->setIndexWidget(index1, createUrlButton());
            QModelIndex index2 = ui.tableProposal->model()->index(i, GovernanceTableModel::hash);
            ui.tableProposal->setIndexWidget(index2, createInfoButton());
        }
    }
}

QPushButton* GovernanceTable::createUrlButton()
{
    QPushButton *btn = new QPushButton(QIcon(":/icons/link"), "", this);
    btn->setFlat(true);
    btn->setStyleSheet(stylesheet);
    btn->setToolTip(tr("Open website"));
    connect(btn, SIGNAL (released()), this, SLOT (onUrl()));
    return btn;
}

QPushButton* GovernanceTable::createInfoButton()
{
    QPushButton *btn = new QPushButton(QIcon(":/icons/info"), "", this);
    btn->setFlat(true);
    btn->setStyleSheet(stylesheet);
    btn->setToolTip(tr("Show details"));
    connect(btn, SIGNAL (released()), this, SLOT (onShowInfo()));
    return btn;
}

void GovernanceTable::onTableRowChanged(const QModelIndex &current, const QModelIndex &previous)
{
    bool enable = current.isValid();
    ui.voteYes->setEnabled(enable);
    ui.voteNo->setEnabled(enable);
    ui.voteAbstain->setEnabled(enable);
}

void GovernanceTable::onShowTableContextMenu(const QPoint& point)
{
    QModelIndex index = ui.tableProposal->indexAt(point);
    if (index.isValid())
        ui.menu->exec(QCursor::pos());
}

void GovernanceTable::onVoteYes()
{}

void GovernanceTable::onVoteNo()
{}

void GovernanceTable::onVoteAbstain()
{}

void GovernanceTable::onUpdateTable()
{
    updateModel();
}

void GovernanceTable::onUrl()
{
    if (!model_)
        return;

    QString url;
    QModelIndexList indexes = ui.tableProposal->selectionModel()->selectedRows();
    if (!indexes.isEmpty()) {
        url = model_->dataAt(indexes.front().row(), GovernanceTableModel::link);
    } else {
        QPoint pt = ui.tableProposal->mapFromGlobal(QCursor::pos());
        pt.setY(pt.y() - ui.tableProposal->horizontalHeader()->frameRect().height());
        QModelIndex index = ui.tableProposal->indexAt(pt);
        if (index.isValid())
            url = model_->dataAt(index.row(), GovernanceTableModel::link);
    }

    if (!url.isEmpty())
        GUIUtil::openURL(url);
    else
        QMessageBox::information(this, tr("Information!"), tr("URL is not found."), QMessageBox::Ok, QMessageBox::Ok);
}

void GovernanceTable::onShowInfo()
{}

void GovernanceTable::onBlockNotify()
{
    updateModel();
}

void GovernanceTable::onShowPrevious(bool show)
{
    if (model_) {
        model_->setShowPrevious(show);
        updateModel();
    }
}

void GovernanceTable::onSearch(const QString& str)
{
    if (model_) {
        model_->setFilter(str);
        updateModel();
    }
}
