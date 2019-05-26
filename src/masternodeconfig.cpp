// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// clang-format off
#include "net.h"
#include "masternodeconfig.h"
#include "util.h"
#include "ui_interface.h"
#include "base58.h"

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/classification.hpp>
// clang-format on

using CMasternodeEntry = CMasternodeConfig::CMasternodeEntry;

CMasternodeConfig masternodeConfig;

static const std::string strConfigHeader = "# Masternode config file\n"
                        "# Format: alias IP:port masternodeprivkey collateral_output_txid collateral_output_index\n"
                        "# Example: mn1 127.0.0.2:51572 93HaYBVUCYjEMeeH1Y4sBGLALQZE1Yc1K64xiqgX37tGBDQL8Xg 2bcd3c84c84f87eaa86e4e56834c92927a07f9e18718810b92e0d0324456a67c 0\n";

CMasternodeConfig::CMasternodeConfig():
    csEntries_(new CCriticalSection)
{}

std::vector<CMasternodeEntry> CMasternodeConfig::getEntries() const
{
    LOCK(*csEntries_);
    return entries_;
}

int CMasternodeConfig::getCount() const
{
    LOCK(*csEntries_);
    return entries_.size();
}

bool CMasternodeConfig::addEntry(const CMasternodeEntry& entry)
{
    if (!entry.isValid())
        return error("%s, entry is not valid\n", __func__);

    LOCK(*csEntries_);
    entries_.push_back(entry);
    return writeConfig();
}

bool CMasternodeConfig::deleteEntry(int index)
{
    LOCK(*csEntries_);
    if (index >= 0 && index < entries_.size()) {
        entries_.erase(entries_.begin() + index);
        return writeConfig();
    } else
        return error("%s, (%d) is out of range (%u)\n", __func__, index, entries_.size());
}

bool CMasternodeConfig::deleteEntry(const std::string& alias)
{
    LOCK(*csEntries_);
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_.at(i).getAlias() == alias) {
            entries_.erase(entries_.begin() + i);
            return writeConfig();
        }
    }

    return error("%s, alias %s was not found\n", __func__, alias);
}

CMasternodeEntry CMasternodeConfig::findEntry(const std::string& alias)
{
    LOCK(*csEntries_);
    for (int i = 0; i < entries_.size(); ++i)
        if (entries_.at(i).getAlias() == alias)
            return entries_.at(i);

    return {};
}

bool CMasternodeConfig::read(std::string& strErr)
{
    int linenumber = 1;
    boost::filesystem::path pathMasternodeConfigFile = GetMasternodeConfigFile();
    boost::filesystem::ifstream streamConfig(pathMasternodeConfigFile);

    if (!streamConfig.good()) {
        FILE* configFile = fopen(pathMasternodeConfigFile.string().c_str(), "a");
        if (configFile != nullptr) {
            fwrite(strConfigHeader.c_str(), strConfigHeader.size(), 1, configFile);
            fclose(configFile);
        }
        return true; // Nothing to read, so just return
    }

    std::vector<CMasternodeEntry> entries;
    for (std::string line; std::getline(streamConfig, line); linenumber++) {
        if (line.empty())
            continue;

        vector<string> words;
        boost::split(words, line, boost::algorithm::is_any_of(" \t"));
        if (words.empty())
            continue; // skip empty line
        if (words.front().front() == '#')
            continue; // skip comment

        if (words.size() < 5) {
            strErr = _("Could not parse masternode.conf") + "\n" +
                     strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"";
            streamConfig.close();
            return false;
        }

        string alias = boost::algorithm::join(vector<string>(words.begin(), words.begin() + words.size() - 4), " "); // space separated alias
        string ip = words.at(words.size() - 4);
        string privKey = words.at(words.size() - 3);
        string txHash = words.at(words.size() - 2);
        string outputIndex = words.at(words.size() - 1);

        // If port is not specified - append default
        if (ip.find(':') == string::npos)
            ip = strprintf("%s:%d", ip, Params().GetDefaultPort());

        int nPort = 0;
        std::string hostname = "";
        SplitHostPort(ip, nPort, hostname);
        if (nPort == 0 || hostname.empty()) {
            strErr = strprintf(_("Failed to parse host:port string (%s)\n"), ip) +
                     strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"";
            streamConfig.close();
            return false;
        }

        if (Params().NetworkID() == CBaseChainParams::MAIN) {
            if (nPort != Params().GetDefaultPort()) {
                strErr = strprintf(_("Invalid port %d detected in masternode.conf\n"), nPort) +
                         strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"" + "\n" +
                         strprintf(_("(must be %d)"), Params().GetDefaultPort());
                streamConfig.close();
                return false;
            }
        } else if (nPort == Params(CBaseChainParams::MAIN).GetDefaultPort()) {
            strErr = _("Invalid port detected in masternode.conf") + "\n" +
                     strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"" + "\n" +
                     strprintf("(%d could be used only on mainnet)", nPort);
            streamConfig.close();
            return false;
        }

        if (alias.empty()) {
            strErr = _("Alias cannot be empty in masternode.conf") + "\n" +
                     strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"";
            streamConfig.close();
            return false;
        }

        CMasternodeEntry cme(alias, ip, privKey, txHash, outputIndex);
        entries.push_back(cme);
    }

    streamConfig.close();

    {
        LOCK(*csEntries_);
        entries_ = entries;
    }

    return true;
}

bool CMasternodeConfig::CMasternodeEntry::castOutputIndex(int &n)
{
    try {
        n = std::stoi(outputIndex);
    } catch (const std::exception e) {
        LogPrintf("%s: %s on getOutputIndex\n", __func__, e.what());
        return false;
    }

    return true;
}

bool CMasternodeConfig::writeConfig() const
{
    boost::filesystem::path pathMasternodeConfigFile = GetMasternodeConfigFile();
    FILE* configFile = fopen(pathMasternodeConfigFile.string().c_str(), "w");
    if (configFile != nullptr) {
        fwrite(strConfigHeader.c_str(), strConfigHeader.size(), 1, configFile);
        for (const CMasternodeEntry& mne : entries_) {
            if (mne.isValid()) {
                string line = strprintf("%s %s %s %s %s\n", mne.getAlias(), mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex());
                fwrite(line.c_str(), line.size(), 1, configFile);
            }
        }

        fclose(configFile);
        return true;
    } else
        return false;
}
