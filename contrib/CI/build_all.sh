#!/bin/bash

set -e

cd ../..

# test if we are in the root directory of repository
if [[ ! -e "autogen.sh" ]]
then
    echo "autogen.sh is not found, not root directory of ColossusCoinXT repository?"
    exit 1
fi

# test if dependencies was built
if [[ -e depends/work/build ]]
then
    arch=`ls depends/work/build`
else
    arch=""
fi

if [[ -z "$arch" || ! -e "depends/$arch" ]]
then
    echo "Building dependencies..."
    pushd `pwd`
    cd depends
    make
    popd

    # test again
    arch=`ls depends/work/build`
    if [[ -z "$arch" || ! -e "depends/$arch" ]]
    then
        echo "Failed to build dependencies."
        exit 1
    fi
else
    echo "Dependencies are up to date: $arch"
fi

echo "Building sources..."

./autogen.sh
export HOSTS=$arch
export BASEPREFIX=`pwd`/depends
CONFIG_SITE=${BASEPREFIX}/`echo "${HOSTS}" | awk '{print $1;}'`/share/config.site ./configure --prefix=/ --enable-debug --with-gui=qt5
make

# test if binaries are here
if [[ ! -e "src/colx-cli" ]]
then
    echo "colx-cli was not found"
    exit 1
fi

if [[ ! -e "src/colx-tx" ]]
then
    echo "colx-tx was not found"
    exit 1 
fi

if [[ ! -e "src/colxd" ]]
then
    echo "colxd was not found"
    exit 1 
fi

if [[ ! -e "src/qt/colx-qt" ]]
then
    echo "colx-qt was not found"
    exit 1 
fi

echo "${0} [done]."
