// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "textbrowserdialog.h"

#include <QTextBrowser>
#include <QVBoxLayout>

using namespace std;

TextBrowserDialog::TextBrowserDialog(
    const QString& title,
    QWidget *parent):
    QDialog(parent),
    title_(title),
    content_(tr("No content available."))
{
    memset(&ui, 0, sizeof(ui));

    setupUI();
    setupLayout();
    updateUI();
}

TextBrowserDialog::~TextBrowserDialog()
{}

void TextBrowserDialog::setupUI()
{
    if (ui.textBrowser) // must be nullptr
        throw std::runtime_error("TextBrowserDialog: ui has already been initialized");

    this->setWindowTitle(title_);

    ui.textBrowser = new QTextBrowser(this);
    ui.textBrowser->setOpenExternalLinks(true);
}

void TextBrowserDialog::updateUI()
{
    this->setWindowTitle(title_);
    ui.textBrowser->setHtml(content_);
}

void TextBrowserDialog::setupLayout()
{
    this->setFixedWidth(800);
    this->setFixedHeight(400);

    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->addWidget(ui.textBrowser);
    this->setLayout(mainLayout);
}

void TextBrowserDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
}

void TextBrowserDialog::closeEvent(QCloseEvent *event)
{
    QDialog::closeEvent(event);
}

void TextBrowserDialog::setTitle(const QString& title)
{
    title_ = title;
    updateUI();
}

void TextBrowserDialog::setContent(const QString& content)
{
    content_ = content;
    updateUI();
}
