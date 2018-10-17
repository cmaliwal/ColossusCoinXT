// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCETABLEMODEL_H
#define BITCOIN_GOVERNANCETABLEMODEL_H

#include <QAbstractTableModel>
#include <QStringList>
#include <QVector>

class ClientModel;
class CBudgetProposal;

class GovernanceTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    GovernanceTableModel(ClientModel *model, QObject *parent = nullptr);

    ~GovernanceTableModel() override;

    int rowCount(const QModelIndex& index = QModelIndex()) const override ;

    int columnCount(const QModelIndex& index = QModelIndex()) const override;

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    enum TableColumns {
        name = 0,
        block_start = 1,
        block_end = 2,
        vote_yes = 3,
        vote_no = 4,
        vote_absolute = 5,
        monthly_payment = 6,
        total_payment = 7,
        link = 8,
        hash = 9,
        column_count = 10
    };

    void updateModel();

    void setShowPrevious(bool show);

    void setFilter(const QString& str);

    std::vector<int> columnWidth() const;

    QString dataAt(int i, int j) const;

private:
    QVariant dataDisplay(const QModelIndex& index) const;
    QVariant dataDecoration(const QModelIndex& index) const;
    QVariant dataAlignment(const QModelIndex& index) const;
    QStringList proposal2string(const CBudgetProposal& pp) const;

    bool passFilter(
            const CBudgetProposal& pp,
            const QStringList& pps,
            bool showPrevious,
            const QString& filter) const;

private:
    ClientModel *model_ = nullptr;
    QVector<QStringList> data_;
    bool showPrevious_ = false;
    QString filter_;
};

#endif // BITCOIN_GOVERNANCETABLEMODEL_H
