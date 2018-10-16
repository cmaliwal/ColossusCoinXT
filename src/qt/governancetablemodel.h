// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCETABLEMODEL_H
#define BITCOIN_GOVERNANCETABLEMODEL_H

#include <QAbstractTableModel>

class ClientModel;

class GovernanceTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    GovernanceTableModel(ClientModel *model, QObject *parent = nullptr);

    ~GovernanceTableModel() override;

    int rowCount(const QModelIndex& index = QModelIndex()) const override ;

    int columnCount(const QModelIndex& index = QModelIndex()) const override;

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

private:
    QVariant dataDisplay(const QModelIndex& index) const;

private:
    ClientModel *model_ = nullptr;
};

#endif // BITCOIN_GOVERNANCETABLEMODEL_H
