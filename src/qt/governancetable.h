// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCETABLE_H
#define BITCOIN_GOVERNANCETABLE_H

#include <QWidget>
#include <memory>

class QLabel;
class QCheckBox;
class QPushButton;
class QLineEdit;
class QTimer;
class QMenu;
class QTableView;
class QModelIndex;

class GovernanceTableModel;
typedef std::shared_ptr<GovernanceTableModel> GovernanceTableModelPtr;

class GovernanceTable : public QWidget
{
    Q_OBJECT

public:
    explicit GovernanceTable(QWidget* parent = nullptr);

    ~GovernanceTable() override;

    void setModel(GovernanceTableModelPtr model);

private:
    void setupUI();
    void updateUI();
    void setupLayout();
    void updateModel();
    QPushButton* createUrlButton();
    QPushButton* createInfoButton();

private slots:
    void onVoteYes();
    void onVoteNo();
    void onVoteAbstain();
    void onUpdateTable();
    void onUrl();
    void onShowInfo();
    void onShowTableContextMenu(const QPoint& point);
    void onBlockNotify();
    void onTableRowChanged(const QModelIndex&, const QModelIndex&);
    void onShowPrevious(bool);
    void onSearch(const QString&);

private:
    GovernanceTableModelPtr model_;

    struct {
        QLabel *labelTitle;
        QLabel *labelNote;
        QLabel *labelSearch;
        QLineEdit *editSearch;
        QCheckBox *showPrevious;
        QTableView *tableProposal;
        QPushButton *voteYes;
        QPushButton *voteNo;
        QPushButton *voteAbstain;
        QPushButton *btnUpdateTable;
        QMenu* menu;
    } ui;
};

#endif // BITCOIN_GOVERNANCETABLE_H
