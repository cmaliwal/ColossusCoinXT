// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEXTBROWSERDIALOG_H
#define BITCOIN_TEXTBROWSERDIALOG_H

#include <QDialog>

class QTextBrowser;

class TextBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TextBrowserDialog(
            const QString& title,
            QWidget *parent = nullptr);

    virtual ~TextBrowserDialog();

    /**
      * Set title to the dialog
     */
    void setTitle(const QString& title);

    /**
      * Set content to the dialog
     */
    void setContent(const QString& content);

protected:
    void showEvent(QShowEvent *event);
    void closeEvent(QCloseEvent *event);

private:
    void setupUI();
    void setupLayout();
    void updateUI();

private:
    QString title_;
    QString content_;

    struct {
        QTextBrowser *textBrowser;
    } ui;
};

#endif // BITCOIN_TEXTBROWSERDIALOG_H
