#!/bin/bash

#
# variables
#
commit=""
branch="none"

#
# functions
#

print_log()
{
    if [[ -e 'var/install.log' ]]
    then
        echo ""
        echo "install.log"
        echo ""
        tail -100 var/install.log
        echo ""
    fi

    if [[ -e 'var/build.log' ]]
    then
        echo ""
        echo "build.log"
        echo ""
        tail -100 var/build.log
        echo ""
    fi
}

copy_build_out()
{
    dir="${RELEASEDIR}/$commit/$1" # $1 first param, platform name
    echo "Creating release directory: $dir"
    mkdir -p $dir

    echo "Copying log files..."
    tar cvzf $dir/install.log.tar.gz var/install.log
    tar cvzf $dir/build.log.tar.gz var/build.log

    if [[ 0 -lt $(ls build/out/colx-* 2>/dev/null | wc -w) ]]
    then
        echo "Copying files to release directory..."
        mv build/out/* $dir
    else
        echo "build/out does not contain required files, looks like build failed."
        echo `ls -l build/out`
    fi
}

update_index()
{
    now=$(date +"%Y-%m-%d %H:%M:%S")
    echo "$now,$branch,$commit" >> "${RELEASEDIR}/index.csv"
}

#
# entry point
#

set -e
echo 'Build started...'

if [[ -z "${RELEASEDIR}" ]]
then
    echo "Release dir is not specified, exit."
    echo "Set release dir in the environment variable: RELEASEDIR"
    exit 1
fi

if [[ ! -e 'bin/make-base-vm' ]]
then
    echo 'Run this script from gitian-builder root folder.'
    exit 1
fi

if [[ ! -e 'ColossusCoinXT' ]]
then
    echo "Clonning git repository..."
    git clone https://github.com/ColossusCoinXT/ColossusCoinXT.git 
else
    echo "Updating git repository..."
    pushd `pwd`
    cd ColossusCoinXT
    git pull
    popd
fi

if [[ ! -z "${BRANCH}" ]]
then
    echo "Switching to the branch=${BRANCH}..."
    pushd `pwd`
    cd ColossusCoinXT
    git checkout "${BRANCH}"
    git pull
    popd
fi

if [[ -z "${COMMIT}" ]]
then
    echo "Commit variable is not specified, extracting from repo..."
    pushd `pwd`
    cd ColossusCoinXT
    commit=`git rev-parse HEAD`
    branch=`git rev-parse --abbrev-ref HEAD`
    popd
else
    echo "Commit variable is specified, COMMIT=${COMMIT}"
    commit="${COMMIT}"
    pushd `pwd`
    cd ColossusCoinXT
    git checkout "$commit"
    branch=`git rev-parse --abbrev-ref HEAD`
    popd
fi

echo "Commit hash to build from is: $commit, len=${#commit}"
if [[ ${#commit} -eq 40 ]]
then
    echo "Commit hash has accepted."
else
    echo "Commit hash is wrong, stop."
    exit 1
fi

if [[ -e "${RELEASEDIR}/$commit" ]]
then
    echo 'Current revision has already built: $commit. See release dir: ${RELEASEDIR}. Stop.'
    exit 0
fi

if [[ -e 'base-trusty-amd64' ]]
then
    echo 'Removing old vm...'
    rm base-trusty-amd64
fi

echo 'Creating base vm...'
bin/make-base-vm --lxc --arch amd64 --suite trusty


if [[ ! -e 'inputs/MacOSX10.11.sdk.tar.gz' ]]
then
    echo 'Downloading MacOS sdk...'
    mkdir -p inputs
    wget -N -P inputs https://github.com/phracker/MacOSX-SDKs/releases/download/10.13/MacOSX10.11.sdk.tar.xz
    mv inputs/MacOSX10.11.sdk.tar.xz inputs/MacOSX10.11.sdk.tar.gz
else
    echo 'MacOS SDK is up to date.'
fi

echo 'Building dependencies...'
rm -rf ColossusCoinXT/depends/work
rm -rf `pwd`/cache/common
make -C ColossusCoinXT/depends download SOURCES_PATH=`pwd`/cache/common

export USE_LXC=1
set +e # we have to collect and print logs

echo ""
echo "Compiling for Mac OSX"
echo ""
bin/gbuild --commit ColossusCoinXT=$commit  ColossusCoinXT/contrib/gitian-descriptors/gitian-osx.yml
copy_build_out "osx"
print_log

echo ""
echo "Compiling for Linux"
echo ""
bin/gbuild --commit ColossusCoinXT=$commit  ColossusCoinXT/contrib/gitian-descriptors/gitian-linux.yml
copy_build_out "linux"
print_log

echo ""
echo "Compiling for Windows"
echo ""
bin/gbuild --commit ColossusCoinXT=$commit  ColossusCoinXT/contrib/gitian-descriptors/gitian-win.yml
copy_build_out "win"
print_log

update_index
echo 'Done'
