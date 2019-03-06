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
- Copy all files from `i2pconfig/client` into your data directory. 
- Public/MN (masternode) (or also 'server' nodes): these nodes are 'published' and are accepting connections form others (client or server nodes).
- Routers: is i2p designation, and are nodes that support the network traffic to go through them. In our case that'd be public/MN-s + all others on the i2p network that have nothing to do w/ the i2p.
- Floodfills: only specially designated nodes (on the i2p) that hold the network database info. COLX will likely not participate in it (in most cases at least).  




## Server Setup
File / Directory | Description | Location



