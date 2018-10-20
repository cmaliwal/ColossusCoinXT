// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "governancetablemodel.h"
#include "masternode-budget.h"
#include "utilmoneystr.h"
#include "tinyformat.h"

#include <QIcon>
#include <QPushButton>

GovernanceTableModel::GovernanceTableModel(ClientModel *model, QObject *parent):
    QAbstractTableModel(parent),
    model_(model)
{
    if (!model_)
        throw std::runtime_error(strprintf("%s: model is nullptr", __func__));

    updateModel();
}

GovernanceTableModel::~GovernanceTableModel()
{}

int GovernanceTableModel::rowCount(const QModelIndex& index) const
{
    return data_.size();
}

int GovernanceTableModel::columnCount(const QModelIndex& index) const
{
    return TableColumns::column_count;
}

QVariant GovernanceTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole) {
        if (orientation == Qt::Horizontal) {
            switch (section) {
            case TableColumns::name:
                return tr("Name");
            case TableColumns::block_start:
                return tr("Block Start");
            case TableColumns::block_end:
                return tr("Block End");
            case TableColumns::vote_yes:
                return tr("Yes");
            case TableColumns::vote_no:
                return tr("No");
            case TableColumns::vote_absolute:
                return tr("Sum");
            case TableColumns::monthly_payment:
                return tr("Monthly Payment");
            case TableColumns::total_payment:
                return tr("Total Payment");
            case TableColumns::link:
                return tr("URL");
            case TableColumns::hash:
                return tr("Info");
            }
        }
    }

    return QVariant();
}

QVariant GovernanceTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();

    if (role == Qt::DisplayRole)
        return dataDisplay(index);
    else if (role == Qt::DecorationRole)
        return dataDecoration(index);
    else if (role == Qt::TextAlignmentRole)
        return dataAlignment(index);
    else
        return QVariant();
}

QVariant GovernanceTableModel::dataDisplay(const QModelIndex& index) const
{
    const int i = index.row();
    const int j = index.column();

    if (j == TableColumns::link || j == TableColumns::hash)
        return QVariant();

    if (i >= 0 && i < data_.size())
        if (j >=0 && j < data_.at(i).size())
            return data_[i][j];

   return QVariant();
}

QVariant GovernanceTableModel::dataDecoration(const QModelIndex& index) const
{
    return QVariant();
}

QVariant GovernanceTableModel::dataAlignment(const QModelIndex& index) const
{
    if (index.column() == TableColumns::name)
        return Qt::AlignLeft + Qt::AlignVCenter;
    else
        return Qt::AlignCenter;
}

std::vector<int> GovernanceTableModel::columnWidth() const
{
    return {100, 90, 90, 50, 50, 50, 120, 120, 50, 50};
}

void GovernanceTableModel::updateModel()
{
    emit layoutAboutToBeChanged();

    vector<CBudgetProposal*> proposalList = budget.GetAllProposals();

    data_.clear();
    data_.reserve(proposalList.size());

    for (CBudgetProposal *pp : proposalList) {
        QStringList pps = proposal2string(*pp);
        if (passFilter(*pp, pps, showPrevious_, filter_))
            data_.push_back(pps);
    }

    emit layoutChanged();
}

QStringList GovernanceTableModel::proposal2string(const CBudgetProposal& pp) const
{
    QStringList ret;
    // mapping to the enum TableColumns
    ret.push_back(QString::fromStdString(pp.GetName())); // name
    ret.push_back(QString::number(pp.GetBlockStart()));
    ret.push_back(QString::number(pp.GetBlockEnd()));
    ret.push_back(QString::number(pp.GetYeas()));
    ret.push_back(QString::number(pp.GetNays()));
    ret.push_back(QString::number(pp.GetYeas() - pp.GetNays()));
    ret.push_back(QString::fromStdString(FormatMoney(pp.GetAmount())));
    ret.push_back(QString::fromStdString(FormatMoney(pp.GetAmount() * pp.GetTotalPaymentCount())));
    ret.push_back(QString::fromStdString(pp.GetURL()));
    ret.push_back(QString::fromStdString(pp.GetHash().ToString())); // hash

    assert(ret.size() == TableColumns::column_count);
    return ret;
}

void GovernanceTableModel::setShowPrevious(bool show)
{
    showPrevious_ = show;
}

void GovernanceTableModel::setFilter(const QString& str)
{
    filter_ = str;
}

bool GovernanceTableModel::passFilter(
        const CBudgetProposal& pp,
        const QStringList& pps,
        bool showPrevious,
        const QString& filter) const
{
    if (!pp.fValid && !showPrevious)
        return false;

    if (filter.trimmed().isEmpty())
        return true;

    for (const QString& s : pps)
        if (s.contains(filter, Qt::CaseInsensitive))
            return true;

    return false;
}

QString GovernanceTableModel::dataAt(int i, int j) const
{
    if (i >= 0 && i < data_.size())
        if (j >=0 && j < data_.at(i).size())
            return data_[i][j];

    assert(false);
    return QString();
}

QString GovernanceTableModel::formatProposal(int row) const
{
    QString hash = dataAt(row, GovernanceTableModel::hash);
    if (hash.isEmpty())
        return QString(tr("Information is not found."));

    CBudgetProposal *pp = budget.FindProposal(uint256(hash.toStdString()));
    if (!pp)
        return QString(tr("Proposal %1 is not found: ").arg(hash));

    const char htmlTemplate[] = "<tr><td>%s</td><td>%s</td></tr>";

    string err;
    CTxDestination address1;
    ExtractDestination(pp->GetPayee(), address1);
    CBitcoinAddress address2(address1);

    stringstream html("<table style=\"width:100%\">");
    html << strprintf(htmlTemplate, "Hash", hash.toStdString());
    html << strprintf(htmlTemplate, "Name", pp->GetName());
    html << strprintf(htmlTemplate, "URL", pp->GetURL());
    html << strprintf(htmlTemplate, "FeeHash", pp->nFeeTXHash.ToString());
    html << strprintf(htmlTemplate, "BlockStart", to_string(pp->GetBlockStart()));
    html << strprintf(htmlTemplate, "BlockEnd", to_string(pp->GetBlockEnd()));
    html << strprintf(htmlTemplate, "TotalPaymentCount", to_string(pp->GetTotalPaymentCount()));
    html << strprintf(htmlTemplate, "RemainingPaymentCount", to_string(pp->GetRemainingPaymentCount()));
    html << strprintf(htmlTemplate, "PaymentAddress", address2.ToString());
    html << strprintf(htmlTemplate, "Ratio", to_string(pp->GetRatio()));
    html << strprintf(htmlTemplate, "Yeas", to_string(pp->GetYeas()));
    html << strprintf(htmlTemplate, "Nays", to_string(pp->GetNays()));
    html << strprintf(htmlTemplate, "Abstains", to_string(pp->GetAbstains()));
    html << strprintf(htmlTemplate, "TotalPayment", FormatMoney(pp->GetAmount() * pp->GetTotalPaymentCount()));
    html << strprintf(htmlTemplate, "MonthlyPayment", FormatMoney(pp->GetAmount()));
    html << strprintf(htmlTemplate, "IsEstablished", pp->IsEstablished() ? "Yes" : "No");
    html << strprintf(htmlTemplate, "IsValid", pp->IsValid(err) ? "Yes" : "No");
    html << strprintf(htmlTemplate, "IsValidReason", err);
    html << strprintf(htmlTemplate, "fValid", pp->fValid ? "Yes" : "No");
    html << "</table>";

    return QString::fromStdString(html.str());
}
