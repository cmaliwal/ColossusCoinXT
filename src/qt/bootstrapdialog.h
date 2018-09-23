// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BOOTSTRAPDIALOG_H
#define BITCOIN_BOOTSTRAPDIALOG_H

#include <QDialog>

#include <memory>

class QLabel;
class QRadioButton;
class QPushButton;
class QLineEdit;
class QProgressBar;

class BootstrapModel;
typedef std::shared_ptr<BootstrapModel> BootstrapModelPtr;

class BootstrapDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BootstrapDialog(BootstrapModelPtr model, QWidget *parent = nullptr);

    virtual ~BootstrapDialog();

    /**
      * Notify dialog that it is running at the app startup
     */
    void SetStartupFlag();

    /**
      * Initiate bootstrap process (intended to use at the app startup).
      * On success perform two stages of the bootstrap algorithm (see BootstrapModel for details).
      * If this method failed - wallet should be restarted.
     */
    static void bootstrapBlockchain(BootstrapModelPtr model);

protected:
    void showEvent(QShowEvent *event);
    void closeEvent(QCloseEvent *event);

    void ShowError(const std::string& msg);

private:
    void setupUI();
    void setupLayout();
    void updateUI();
    bool cancel();

private slots:
    void onButtonSelectFile();
    void onButtonRun();
    void onButtonHide();
    void onButtonCancel();
    void onButtonRadioCloud();
    void onButtonRadioFile();
    void onEditFilePathChanged(const QString& text);
    void onBootstrapModelChanged();
    void onBootstrapProgress(const QString& title, int nProgress);
    void onBootstrapCompleted(bool success, const QString& error);

private:
    BootstrapModelPtr model_;
    bool startup_ = false;

    struct {
        QLabel *labelTitle;
        QRadioButton *radioCloud, *radioFile;
        QLineEdit *editFilePath;
        QPushButton *btnSelectFile;
        QPushButton *btnRun, *btnHide, *btnCancel;
        QProgressBar *progress;
        QLabel *labelLink;
    } ui;
};

#endif // BITCOIN_BOOTSTRAPDIALOG_H
