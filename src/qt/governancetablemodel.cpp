// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "governancetablemodel.h"
#include "tinyformat.h"

GovernanceTableModel::GovernanceTableModel(ClientModel *model, QObject *parent):
    QAbstractTableModel(parent),
    model_(model)
{
    if (!model_)
        throw std::runtime_error(strprintf("%s: model is nullptr", __func__));
}

GovernanceTableModel::~GovernanceTableModel()
{}

int GovernanceTableModel::rowCount(const QModelIndex& index) const
{
    return 2;
}

int GovernanceTableModel::columnCount(const QModelIndex& index) const
{
    return 3;
}

QVariant GovernanceTableModel::data(const QModelIndex& index, int role) const
{
    if (role == Qt::DisplayRole)
        return dataDisplay(index);
    else
        return QVariant();
}

QVariant GovernanceTableModel::dataDisplay(const QModelIndex& index) const
{
   return QString("Row%1, Column%2")
               .arg(index.row() + 1)
               .arg(index.column() +1);
}
