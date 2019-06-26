// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODEENTRYDIALOG_H
#define BITCOIN_MASTERNODEENTRYDIALOG_H

#include <QDialog>
#include <memory>
#include <set>

class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;

struct MasternodeOutput
{
    MasternodeOutput(const std::string& hash, int i):
        txHash(hash), index(i) {}

    std::string txHash;
    int index = 0;
};

class MasternodeEntryDialog : public QDialog
{
    Q_OBJECT

public:
    MasternodeEntryDialog(
            const std::vector<MasternodeOutput>& outputList,
            QWidget *parent);

    MasternodeEntryDialog(
            const std::string& alias,
            const std::string& ip,
            const std::string& pk,
            const std::vector<MasternodeOutput>& outputList,
            QWidget *parent);

    ~MasternodeEntryDialog() override;

    void setBusyAliases(const std::set<std::string>& aliases);

    std::string getAlias() const;

    std::string getIP() const;

    std::string getPrivateKey() const;

    int getOutputIndex() const;

protected:
    void showEvent(QShowEvent *event);
    void closeEvent(QCloseEvent *event);

private:
    void setupUI();
    void setupLayout();
    void ShowError(const QString& msg);

private slots:
    void onButtonOk();
    void onButtonCancel();

private:
    std::vector<MasternodeOutput> outputList_;
    std::set<std::string> busyAliases_;

    struct {
        QLabel *labelAlias = nullptr;
        QLineEdit *eAlias = nullptr;
        QLabel *labelIP = nullptr;
        QLineEdit *eIP = nullptr;
        QLabel *labelPK = nullptr;
        QLineEdit *ePK = nullptr;
        QLabel *labelOutput = nullptr;
        QComboBox *comboOutput = nullptr;

        QPushButton *btnOk = nullptr;
        QPushButton *btnCancel = nullptr;
    } ui;
};

#endif // BITCOIN_MASTERNODEENTRYDIALOG_H
