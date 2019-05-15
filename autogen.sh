#!/bin/sh
# Copyright (c) 2009-2014 The Bitcoin developers
# Copyright (c) 2014-2015 The Dash developers
# Copyright (c) 2015-2017 The PIVX developers
# Copyright (c) 2017 The COLX Core Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C
set -e
srcdir="$(dirname $0)"
cd "$srcdir"
if [ -z ${LIBTOOLIZE} ] && GLIBTOOLIZE="`which glibtoolize 2>/dev/null`"; then
  LIBTOOLIZE="${GLIBTOOLIZE}"
  export LIBTOOLIZE
fi

which autoreconf >/dev/null || \
  (echo "configuration failed, please install autoconf first" && exit 1)

autoreconf --install --force --warnings=all
