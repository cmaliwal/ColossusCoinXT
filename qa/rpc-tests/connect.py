#!/usr/bin/env python2
# Copyright (c) 2014 The Bitcoin Core developers
# Copyright (c) 2014-2015 The Dash developers
# Copyright (c) 2015-2017 The PIVX developers
# Copyright (c) 2017 The COLX developers
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test RPC connection
#

import sys
import logging
import traceback
from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException


def main(proxy):
    try:
        assert proxy is not None

        print("Starting...")
        print proxy.getinfo()
        print ("Connect success.")

    except AssertionError as e:
        print("Assertion failed: " + e.message)
    except JSONRPCException as e:
        print("JSONRPC error: " + e.error['message'])
        traceback.print_tb(sys.exc_info()[2])
    except Exception as e:
        print("Unexpected exception caught during testing: " + str(sys.exc_info()[0]))
        traceback.print_tb(sys.exc_info()[2])


if __name__ == '__main__':
    logging.basicConfig(level=logging.DEBUG)
    proxy = AuthServiceProxy("http://colxrpc:HXQ4E4En87xaBth9atGnNH27958E3ep8MgnvERaskZcG@127.0.0.1:51475")
    main(proxy)
