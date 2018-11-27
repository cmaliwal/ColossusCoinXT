// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2015-2018 The COLX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bootstrapmodel.h"
#include "tinyformat.h"
#include "chainparams.h"
#include "finally.h"
#include "ziputil.h"
#include "util.h"
#include "curl.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <openssl/sha.h>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/exception/all.hpp>

using namespace std;
using namespace boost;
using namespace boost::filesystem;

/** name of bootstrap file in the data directory */
static const char BOOTSTRAP_FILE_NAME[] = "bootstrap.zip" ;

/** name of bootstrap subdir in the data directory */
static const char BOOTSTRAP_DIR_NAME[] = "bootstrap" ;

/** name of bootstrap verified file */
static const char BOOTSTRAP_VERIFIED[] = "verified";

/** count number of instances of the model */
std::atomic<int> BootstrapModel::instanceNumber_(0);

//
// Implementation notes:
// - probably better to protect workerThread_/instanceNumber_ by mutex to prevent possible race condition;
//

/** compute the 256-bit hash of the ifstream */
inline std::string Hash(ifstream& ifs)
{
    const std::size_t CHUNK = 4096;

    if (!ifs.is_open() || ifs.eof()) {
        assert(false);
        return "0000000000000000000000000000000000000000000000000000000000000000";
    } else {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);

        std::string input;
        input.resize(CHUNK, 0);
        while (!ifs.eof()) {
            ifs.read(&input[0], CHUNK);
            if (ifs.gcount() <= CHUNK)
                SHA256_Update(&sha256, input.c_str(), ifs.gcount());
            else
                assert(false);
        }
        SHA256_Final(hash, &sha256);

        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            ss << hex << setw(2) << setfill('0') << (int)hash[i];

        return ss.str();
    }
}

BootstrapModel::BootstrapModel():
    bootstrapMode_(BootstrapMode::cloud),
    progress_(0),
    cancel_(false),
    configMerged_(false)
{
    if (instanceNumber_ > 0)
        throw logic_error(strprintf("%s: only single instance of the BootstrapModel can be created", __func__));

    if (!ParamsSelected())
        throw logic_error(strprintf("%s: network params has not been selected", __func__));

    datadirPath_ = GetDataDir();
    if (!exists(datadirPath_))
        throw logic_error(strprintf("%s: data dir does not exist: %s", __func__, datadirPath_.string()));

    instanceNumber_ += 1;
}

BootstrapModel::~BootstrapModel()
{
    instanceNumber_ -= 1;
    Cancel();
    Wait();
}

void BootstrapModel::ResetState()
{
    progress_ = 0;
    cancel_ = false;
    configMerged_ = false;
    latestRunError_.clear();
}

bool BootstrapModel::IsBootstrapRunning() const
{
    return workerThread_ && workerThread_->joinable() &&
            !workerThread_->try_join_for(boost::chrono::milliseconds(1));
}

int BootstrapModel::GetBootstrapProgress() const
{
    return progress_;
}

BootstrapMode BootstrapModel::GetBootstrapMode() const
{
    return bootstrapMode_;
}

bool BootstrapModel::SetBootstrapMode(BootstrapMode mode, string& err)
{
    if (IsBootstrapRunning()) {
        err = "Bootstrap is running";
        return false;
    } else {
        bootstrapMode_ = mode;
        NotifyModelChanged();
        return true;
    }
}

boost::filesystem::path BootstrapModel::GetBootstrapFilePath() const
{
    return bootstrapFilePath_;
}

bool BootstrapModel::SetBootstrapFilePath(const path& p, string& err)
{
    if (IsBootstrapRunning()) {
        err = "Bootstrap is running";
        return false;
    } else {
        bootstrapFilePath_ = p;
        NotifyModelChanged();
        return true;
    }
}

bool BootstrapModel::BootstrapFilePathOk() const
{
    return exists(GetBootstrapFilePath());
}

bool BootstrapModel::IsConfigMerged() const
{
    return configMerged_;
}

bool BootstrapModel::RunStageI(std::string& err)
{
    try {
        if (IsBootstrapRunning()) {
            err = "Bootstrap is running";
            return false;
        } else if (!RunStageIPossible(err))
            return false;
        else if (!CleanUp(err))
            return false;
        else if (bootstrapMode_ == BootstrapMode::file) {
            workerThread_.reset(new thread([this](){ RunFromFileThread(); }));
            return true;
        } else if (bootstrapMode_ == BootstrapMode::cloud) {
            workerThread_.reset(new thread([this](){ RunFromCloudThread(); }));
            return true;
        } else {
            err = "Unsupported bootstrap mode";
            return false;
        }
    } catch (const boost::exception& e) {
        err = boost::diagnostic_information(e);
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unexpected error";
    }

    return error("%s : %s", __func__, err);
}

bool BootstrapModel::RunStageIPossible(std::string& err) const
{
    try {
        if (IsBootstrapRunning()) {
            err = "Bootstrap is running";
            return false;
        } else if (!FreeSpaceOk(err))
            return false;
        else
            return true;
    } catch (const boost::exception& e) {
        err = boost::diagnostic_information(e);
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unexpected error";
    }

    return error("%s : %s", __func__, err);
}

bool BootstrapModel::RunStageII(std::string& err)
{
    try {
        if (IsBootstrapRunning()) {
            err = "Bootstrap is running";
            return false;
        } else if (!RunStageIIPossible(err)) {
            return false;
        }
        else {
            workerThread_.reset(new thread([this](){ RunStageIIThread(); }));
            return true;
        }
    } catch (const boost::exception& e) {
        err = boost::diagnostic_information(e);
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unexpected error";
    }

    return error("%s : %s", __func__, err);
}

bool BootstrapModel::RunStageIIPrepared() const
{
    return exists(datadirPath_ / BOOTSTRAP_DIR_NAME / BOOTSTRAP_VERIFIED);
}

bool BootstrapModel::RunStageIIPossible(std::string& err) const
{
    try {
        if (IsBootstrapRunning()) {
            err = "Bootstrap is running";
            return false;
        } else if (!FreeSpaceOk(err))
            return false;
        else
            return true;
    } catch (const boost::exception& e) {
        err = boost::diagnostic_information(e);
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unexpected error";
    }

    return error("%s : %s", __func__, err);
}

bool BootstrapModel::CleanUp(std::string& err) const
{
    try {
        if (IsBootstrapRunning()) {
            err = "Bootstrap is running";
            return false;
        } else
            return CleanUpImpl(err);
    } catch (const boost::exception& e) {
        err = boost::diagnostic_information(e);
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unexpected error";
    }

    return error("%s : %s", __func__, err);
}

bool BootstrapModel::CleanUpImpl(std::string& err) const
{
    remove_all(datadirPath_ / BOOTSTRAP_DIR_NAME);
    remove(datadirPath_ / BOOTSTRAP_FILE_NAME);
    return true;
}

bool BootstrapModel::IsLatestRunSuccess(std::string& err) const
{
    Wait();
    err = latestRunError_;
    return err.empty();
}

void BootstrapModel::Wait() const
{
    if (workerThread_ && workerThread_->joinable())
        workerThread_->join();
}

void BootstrapModel::Cancel()
{
    cancel_ = true;
}

bool BootstrapModel::IsCancelled() const
{
    return cancel_;
}

bool BootstrapModel::FreeSpaceOk(std::string& err) const
{
    space_info si = space(datadirPath_);
    if (si.available < GetBlockChainSize() * 2) {
        err = strprintf("Not enough free space avaialable in the directory: %s, required at least: %lld", datadirPath_.string(), GetBlockChainSize() * 2);
        return false;
    } else
        return true;
}

void BootstrapModel::RunFromFileThread()
{
    NotifyModelChanged(); // model running state changed

    string thName = strprintf("colx-%s", __func__);
    RenameThread(thName.c_str());
    LogPrintf("%s thread start\n", thName);

    try {
        ResetState();
        RunFromFileImpl(GetBootstrapFilePath(), latestRunError_);
    } catch (const std::exception& e) {
        latestRunError_ = e.what();
    } catch (...) {
        latestRunError_ = "unexpected error";
    }

    if (!latestRunError_.empty())
        error("%s : %s", __func__, latestRunError_);

    NotifyBootstrapCompletedI(latestRunError_.empty(), latestRunError_);
    NotifyModelChanged(); // model running state changed
    LogPrintf("%s thread exit\n", thName);
}

void BootstrapModel::RunFromCloudThread()
{
    NotifyModelChanged(); // model running state changed

    string thName = strprintf("colx-%s", __func__);
    RenameThread(thName.c_str());
    LogPrintf("%s thread start\n", thName);

    try {
        ResetState();
        RunFromCloudImpl(latestRunError_);
    } catch (const std::exception& e) {
        latestRunError_ = e.what();
    } catch (...) {
        latestRunError_ = "unexpected error";
    }

    if (!latestRunError_.empty())
        error("%s : %s", __func__, latestRunError_);

    NotifyBootstrapCompletedI(latestRunError_.empty(), latestRunError_);
    NotifyModelChanged(); // model running state changed
    LogPrintf("%s thread exit\n", thName);
}

void BootstrapModel::RunStageIIThread()
{
    NotifyModelChanged(); // model running state changed

    string thName = strprintf("colx-%s", __func__);
    RenameThread(thName.c_str());
    LogPrintf("%s thread start\n", thName);

    try {
        ResetState();
        RunStageIIImpl(latestRunError_);
    } catch (const std::exception& e) {
        latestRunError_ = e.what();
    } catch (...) {
        latestRunError_ = "unexpected error";
    }

    if (!latestRunError_.empty())
        error("%s : %s", __func__, latestRunError_);

    NotifyBootstrapCompletedII(latestRunError_.empty(), latestRunError_);
    NotifyModelChanged(); // model running state changed
    LogPrintf("%s thread exit\n", thName);
}

// unzip archive and prepare verified file
bool BootstrapModel::RunFromFileImpl(const boost::filesystem::path& zipPath, std::string& err)
{
    if (!exists(zipPath)) {
        err = strprintf("Path does not exist %s", zipPath.string());
        return error("%s : %s", __func__, err);
    }

    if (!VerifyZip(zipPath, err))
        return error("%s : %s", __func__, err);

    if (!VerifySignature(zipPath, err))
        return error("%s : %s", __func__, err);

    NotifyBootstrapProgress(strprintf("Unzipping %s...", zipPath.string()), 0);

    const path bootstrapDirPath = datadirPath_ / BOOTSTRAP_DIR_NAME;
    if (exists(bootstrapDirPath)) {
        assert(false); // must not exist at this point
        remove(bootstrapDirPath);
    }

    create_directory(bootstrapDirPath);
    if (!ZipExtract(zipPath.string(), bootstrapDirPath.string(), err)) {
        err = strprintf("Zip extract from %s to %s failed with error: %s", zipPath.string(), bootstrapDirPath.string(), err);
        return error("%s : %s", __func__, err);
    }

    NotifyBootstrapProgress(strprintf("Verifying %s...", bootstrapDirPath.string()), 0);

    if (!VerifyBootstrapFolder(bootstrapDirPath, err))
        return error("%s : %s", __func__, err);

    if (!VerifyNetworkType(bootstrapDirPath, err))
        return error("%s : %s", __func__, err);

    if (!BootstrapVerifiedCreate(zipPath, bootstrapDirPath / BOOTSTRAP_VERIFIED, err))
        return error("%s : %s", __func__, err);

    return true;
}

// download bootstrap and run RunFromFile()
bool BootstrapModel::RunFromCloudImpl(std::string& err)
{
    const string url = Params().GetBootstrapUrl();
    if (url.empty()) {
        err = "Bootstrap URL is empty";
        return error("%s : %s", __func__, err);
    }

    path zipPath = datadirPath_ / BOOTSTRAP_FILE_NAME;
    if (exists(zipPath)) {
        assert(false); // must not exist at this point
        remove(zipPath);
    }

    const string tmpPath = strprintf("%s.tmp", zipPath.string());
    if (exists(tmpPath)) {
        assert(false); // must not exist at this point
        remove(tmpPath);
    }

    NotifyBootstrapProgress(strprintf("Downloading %s", url), 0);

    int speed = 0, bytes = 0;
    using time_point = std::chrono::system_clock::time_point;
    time_point tp1 = std::chrono::system_clock::now();
    bool success = CURLDownloadToFile(url, tmpPath,
        [this, &speed, &bytes, &tp1](double total, double now)->int{
        if (cancel_)
            return CURL_CANCEL_DOWNLOAD;
        else {
            time_point tp2 = std::chrono::system_clock::now();
            size_t sec = std::chrono::duration_cast<std::chrono::seconds>(tp2 - tp1).count();
            if (sec > 5) {
                speed = (now - bytes) / sec;
                bytes = now;
                tp1 = std::chrono::system_clock::now();
            }

            if (now > 0 && total > 0 && total >= now)
                progress_ = static_cast<int>(100.0 * now / total);

            string str = strprintf("Downloading %s (%s/s)",
                HumanReadableSize(static_cast<int>(now), false), HumanReadableSize(speed, false));

            NotifyBootstrapProgress(str, progress_);
            return CURL_CONTINUE_DOWNLOAD;
        }
    } , err);

    if (success) {
        rename(tmpPath, zipPath);
        if (!VerifyZip(zipPath, err)) {
            err = strprintf("%s\nTry to download bootstrap file manually: %s.", err, url);
            return error("%s : %s", __func__, err);
        }
        else
            return RunFromFileImpl(zipPath, err);
    }
    else {
        remove(tmpPath);
        return error("%s : %s", __func__, err);
    }
}

// backup blockchain and replace it with bootstrap
bool BootstrapModel::RunStageIIImpl(std::string& err)
{
    // we don't perform additional checks here because if bootstrap
    // is interrupted in the middle - it will be able to complete on  the next run

    const path bootstrapDirPath = datadirPath_ / BOOTSTRAP_DIR_NAME;
    if (!exists(bootstrapDirPath)) {
        err = strprintf("Path does not exist %s", bootstrapDirPath.string());
        return error("%s : %s", __func__, err);
    }

    // replace blockchain files from bootstrap
    const vector<path> dirList = GetBootstrapDirList(bootstrapDirPath);
    for (const path& dir : dirList) {
        const path originPath = datadirPath_ / dir.leaf();
        const path bakPath = originPath.string() + ".bak";

         // remove old backup
        if (exists(dir) && exists(originPath))
            remove_all(bakPath);

        // create new backup
        if (exists(originPath) && !exists(bakPath))
            rename(originPath, bakPath);

        // move dir from bootstrap to origin
        if (exists(dir))
            rename(dir, originPath);
    }

    const path configPath = GetConfigFile();
    configMerged_ = MergeConfigFile(configPath, bootstrapDirPath / "ColossusXT.conf");

    remove(datadirPath_ / "peers.dat");
    remove(datadirPath_ / "banlist.dat");

    return CleanUpImpl(err);
}

bool BootstrapModel::VerifyZip(const boost::filesystem::path& zipPath, std::string& err) const
{
    ifstream ifs(zipPath.string());
    if (!ifs.is_open()) {
        err = strprintf("Failed to open path %s", zipPath.string());
        return error("%s : %s", __func__, err);
    }

    // auto-close
    Finally ifsClose([&ifs](){ ifs.close(); });

    // check it is .zip
    char pk[3] = {0};
    ifs.read(pk, 2);
    if (string(pk) != "PK") {
        err = strprintf("File %s is not a valid .zip archive", zipPath.string());
        return error("%s : %s", __func__, err);
    }

    // implement signature later
    return true;
}

bool BootstrapModel::VerifySignature(const boost::filesystem::path& zipPath, std::string& err) const
{
    // implement signature later
    return true;
}

bool BootstrapModel::VerifyBootstrapFolder(const boost::filesystem::path& bootstrapDir, std::string& err) const
{
    const vector<path> dirList = GetBootstrapDirList(bootstrapDir);
    for (const path& dir : dirList) {
        if (!exists(dir)) {
            err = strprintf("Verification failed, %s does not exist", dir.string());
            return error("%s : %s", __func__, err);
        }
    }

    return true;
}

bool BootstrapModel::VerifyNetworkType(const boost::filesystem::path& bootstrapDir, std::string& err) const
{
    if (!VerifyGenesisBlock(bootstrapDir.string(), Params().HashGenesisBlock(), err)) {
        err = strprintf("Bootstrap verification failed with reason: %s", err);
        return error("%s : %s", __func__, err);
    }
    else
        return true;
}

bool BootstrapModel::BootstrapVerifiedCreate(const boost::filesystem::path& zipPath, const boost::filesystem::path& verifiedPath, std::string& err) const
{
    ofstream ofs(verifiedPath.string());
    if (ofs.is_open()) {
        Finally ofsClose([&ofs](){ ofs.close(); });

        string hash;
        ifstream ifs(zipPath.string());
        if (ifs.is_open()) {
            Finally ifsClose([&ifs](){ ifs.close(); });
            hash = Hash(ifs);
        }

        ofs << hash;
        return true;
    } else {
        err = strprintf("Unable to create %s", verifiedPath.string());
        return error("%s : %s", __func__, err);
    }
}

bool BootstrapModel::BootstrapVerifiedCheck(const boost::filesystem::path& verifiedPath, std::string& err) const
{
    if (!exists(verifiedPath)) {
        err = strprintf("Path %s does not exist", verifiedPath.string());
        return error("%s : %s", __func__, err);
    } else
        return true;
}

// return true if merge success and false otherwise
bool BootstrapModel::MergeConfigFile(const boost::filesystem::path& original, const boost::filesystem::path& bootstrap) const
{
    if (!exists(bootstrap))
        return false; // nothing to merge

    if (!exists(original)) {
        boost::filesystem::copy(bootstrap, original);
        return true; // just raw copy, return true
    }

    const path originalBackup = original.string() + ".bak";

    // remove old backup of the original config
    boost::filesystem::remove(originalBackup);

    // make new backup of the original config
    boost::filesystem::rename(original, originalBackup);

    ifstream oldConf(originalBackup.string());
    if (!oldConf.is_open()) {
        error("%s : Failed to open %s", __func__, originalBackup.string());
        return false;
    }
    Finally oldConfClose([&oldConf](){ oldConf.close(); });

    ofstream newConf(original.string());
    if (!newConf.is_open()) {
        error("%s : Failed to open %s", __func__, original.string());
        return false;
    }
    Finally newConfClose([&newConf](){ newConf.close(); });

    // read all lines from the original config except 'addnode'
    string line;
    while (std::getline(oldConf, line))
        if (line.find("addnode") == string::npos)
            newConf << line << endl;

    // append all lines from the bootstrap config
    ifstream bootstrapConf(bootstrap.string());
    Finally bootstrapConfClose([&bootstrapConf](){ bootstrapConf.close(); });
    while (std::getline(bootstrapConf, line))
        newConf << line << endl;

    return true; // merge success
}

std::vector<boost::filesystem::path> BootstrapModel::GetBootstrapDirList(const boost::filesystem::path& bootstrapDir) const
{
    return {
        bootstrapDir / "blocks",
        bootstrapDir / "chainstate",
        bootstrapDir / "zerocoin"
    };
}
