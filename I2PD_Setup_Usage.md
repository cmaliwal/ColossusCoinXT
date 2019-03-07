I2PD Setup & Usage
=====================================

[![Build Status](https://travis-ci.org/COLX-Project/COLX.svg?branch=i2pd)](https://travis-ci.org/COLX-Project/COLX) [![GitHub version](https://badge.fury.io/gh/COLX-Project%2FCOLX.svg)](https://badge.fury.io/gh/COLX-Project%2FCOLX)



# I2P Relevant Files, Dirs (in order of importance) - Testnet

File / Directory | Description | Location | Node Type | Importance | Dir
------------ | ------------- | ------------- | ------------- | ------------- | -------------
ColossusXT.conf | outbound/addnode and inbound/mn binding conf. | data directory | client, server | * |
my-keys.dat | keys to sign address, generated | data directory | server | * |
peerProfiles | predefined and updated | data directory | client, server | * | yes
netDb | predefined and updated | data directory | client, server | * | yes
addressbook | predefined and updated | data directory | client, server | * | yes
i2pd.conf | main I2PD configuration file, will be used | data directory | client, server | ? |
tunnels.conf | predefined tunnels setup, not used | data directory | client, server |  | 
tags | predefined and updated | data directory | client, server |  | yes
destinations | generated, address dump | data directory | client, server |  | yes
router.keys | predefined or generated | data directory | client, server |  | 
ntcp2.keys | predefined or generated | data directory | client, server |  | 
router.info | predefined or generated | data directory | client, server |  | 
testnet4 | custom user data (supplied by user) | data directory | client, server | | yes
i2pd.log | i2pd log file (generated, appended) | bin / colxd directory | client, server | | 
debug.log | COLX log file (generated, appended) | testnet4 sub directory | client, server | | 
  
## Additional Explanations     
- i2pd.log path can also be setup in the i2pd.conf file - default is bin directory (colxd/colx-qt dir)
- bug report (testnet only): debug.log, i2pd.log and full datadir (including testnet4, wallet, blocks etc.) should be supplied (zip/tar). Don't store any sensitive data or real wallet, testsnet is for testing so use test wallets only. Minimum: both logs and datadir w/o testnet (but wallet/blocks help unwind or detect any specific problems). Core dumps if any major faults, crashes, if possible (more info to be supplied if needed).    
- logging is maxed out at the moment for testing purposes, should be watched and trimmed.  
- apart from the .conf files, my-keys.dat file - **peerProfiles, netDb, addressbook dirs are absolutely essential** - if you delete those i2p network won't work, these are like seeds that are required


# Client Setup

Client side setup is relatively painless.  

## Steps Summary
- Start with empty data directory
- Copy all files from `i2pconfig/client` (or client.ColossusXT.tar.gz) into your data directory. Make sure all subdirs are copies as well. 
- Copy your 'testnet4' (with your wallet, blocks etc.) into this new data directory.  
- Edit 'ColossusXT.conf': adjust 'addnode'-s to include the latest list of i2p addresses (to be supplied, or expanded as we go) or seeding when available.  
- run with that datadir=... e.g. './colx-qt -testnet -datadir=...' or colxd...  
- .  



# Server (MN) Setup

Same as for the client above, + some more...  
- Start with empty data directory
- Copy all files from `i2pconfig/client` (or server.ColossusXT.tar.gz) into your data directory. Make sure all subdirs are copies as well. 
- Copy your 'testnet4' (with your wallet, blocks etc.) into this new data directory.  
- now, remove the 'my-keys.dat' file.
- go to 'destinations' subidr, remove all .dat files from in there, i.e. to have empty dir.
- run the colx-qt or colxd, and as soon as it's up, close it.
- you'll find new my-keys.dat file created - that's your private/public key to match your new destination.
- go to destinations and you'll find exactly one file created in the form <hash>.dat, copy that <hash> (w/o the .dat, just numbers).
- open the ColossusXT.conf, replace all instances of 'ker57lgh3hl5jqfflpwuuhmopqe54f3x23hggd2prwovfo3byw3q' with the new <hash>.
- if any other server nodes are available add them to addnode-s, like - addnode=<other-hash>.b32.i2p:6667
- port is almost always the same (6667), can be changed though.  
- of course adjust your MN masternodeprivkey to match your wallets/setup (refer to MN setup).
  
That is the way to create new keys and matching destinations/addresses. For each new node (of server/MN type) you need to repeat the procedure - do not reuse addresses.  
You only need to go through this for the server/MN. Client nodes have their local destinations craated as temporary ones, and those change on each run (can't be persisted accrosss sessions, not w/o some code adjustments, it'll be available for client/server mix nodes).  


# Firewalls, Other Setup  

Nothing to do here. No firewall setup is needed (except if wishing to improve performance and for public nodes as per the official documentation, but I didn't explore that avenue).  


# Usage

All should be the same except for a few things:  
- visuals are slightly different (as instead of IP-s we have i2p addresses).  
- initialization is a bit slower, you need to wait a bit for i2p network to kick in and start synchronizing things. Sometimes that's right away, sometimes it's couple minutes, could be more in some cases, most of the time you won't notice big difference but just slightly slower.
- block syncing is slower and performance in general (i.e. things are slow in GUI), see known issues, this isn't optimized at all (should be an easy fix).  
- some network scenarios are not tested, there may be issues etc. (again see known issues for me). 

