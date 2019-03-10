I2PD Setup & Usage
=====================================

[![Build Status](https://travis-ci.org/COLX-Project/COLX.svg?branch=i2pd)](https://travis-ci.org/COLX-Project/COLX) [![GitHub version](https://badge.fury.io/gh/COLX-Project%2FCOLX.svg)](https://badge.fury.io/gh/COLX-Project%2FCOLX)


# Important 
- **Note: you do need the new binaries (for this i2pd branch) for any of this to work!**.
- run as `colx-qt -testnet -datadir=<path-to-.ColossusXT-folder>` - **datadir folder is important (to specify)** as it sets up things for both COLX and I2PD. If unspecified COLX goes into one place, and I2PD uses its own location for files.


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
mncache.dat | MN cache file (regenerated) | testnet4 sub directory | client, server | | 
  
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
- run with that datadir=... e.g. './colx-qt -testnet -datadir=...' or './colxd -daemon -testnet -datadir=...'.  
- .    
  
**Note: mncache.dat file, if reused from before (when copying your testnet4), will result in an early on error and app will fail to run (daemon or qt). Just delete the file to be recreated.**

ColossusXT.conf should look similar to this:  

```
listen=0 # listen=1
testnet=1
server=1
txindex=1
rpcuser=colxrpc
rpcpassword=m5JbgKWGs4VH8z9y329oTvNizvbMbUbRJRYK5aFrzbi
rpcport=51473
port=51572
rpcallowip=127.0.0.1
testnet=1
enablezeromint=0 # we should specify this for masternodes (COLX docs)
logtimestamps=1
staking=1
banscore=1000
bantime=10 # that's in seconds
#daemon=1
addnode=xqt6q6kten5nkzdo6ojxs5sbthtp7wjlnewid2l3iit55venn5pq.b32.i2p:6667
addnode=h722acegogbffjgwd52bydcycifugd5lmvdnsk7rtuk2vjpge5ea.b32.i2p:6667
addnode=ker57lgh3hl5jqfflpwuuhmopqe54f3x23hggd2prwovfo3byw3q.b32.i2p:6667
```

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
  
**Note: mncache.dat file, if reused from before (when copying your testnet4), will result in an early on error and app will fail to run (daemon or qt). Just delete the file to be recreated.**  
  
That is the way to create new keys and matching destinations/addresses. For each new node (of server/MN type) you need to repeat the procedure - do not reuse addresses.  
You only need to go through this for the server/MN. Client nodes have their local destinations craated as temporary ones, and those change on each run (can't be persisted accrosss sessions, not w/o some code adjustments, it'll be available for client/server mix nodes).  
  
It should look something like this:  
```
listen=1
server=1
daemon=1
txindex=1
rpcuser=colxrpc
rpcpassword=m5JbgKWGs4VH8z9y329oTvNizvbMbUbRJRYK5aFrzbi
rpcport=51473
port=6667 #51374
rpcallowip=127.0.0.1
testnet=1
logtimestamps=1
maxconnections=256
banscore=1000
bantime=10 # that's in seconds
externalip=h722acegogbffjgwd52bydcycifugd5lmvdnsk7rtuk2vjpge5ea.b32.i2p
bind=h722acegogbffjgwd52bydcycifugd5lmvdnsk7rtuk2vjpge5ea.b32.i2p:6667
masternode=1
masternodeaddr=h722acegogbffjgwd52bydcycifugd5lmvdnsk7rtuk2vjpge5ea.b32.i2p:6667
masternodeprivkey=92cWzpBA42MK3wzpctamzvzM44T6TkdyLpEcrPoUrnH6hAe33uX # important
addnode=ker57lgh3hl5jqfflpwuuhmopqe54f3x23hggd2prwovfo3byw3q.b32.i2p:6667
```  


# Server (regular-non-MN) Setup

**Note: this is the least tested option, but working ok so far.**  
This is a 'middle-way' variant. Basically just fill in the `bind` parameter (and possibly `externalip`) in the ColossusXT.conf - and leave out all the MN specifics. That way we should have a 'server' tunnel set up (with a visible public destination) w/o all the MN extra work.  
- do everything as for the 'Server (MN)' above (i.e. you need the new keys generated as described).
- change the ColossusXT.conf, remove all the mn specific attributes (private key, address, all w/ 'mn' in front). To something like this
```
listen=1
server=1
daemon=1
txindex=1
rpcuser=colxrpc
rpcpassword=m5JbgKWGs4VH8z9y329oTvNizvbMbUbRJRYK5aFrzbi
rpcport=51473
port=6667 #51374
rpcallowip=127.0.0.1
testnet=1
enablezeromint=0 # we should specify this for masternodes (COLX docs)
logtimestamps=1
staking=1
maxconnections=256
banscore=1000
bantime=10 # that's in seconds

externalip=xqt6q6kten5nkzdo6ojxs5sbthtp7wjlnewid2l3iit55venn5pq.b32.i2p
bind=xqt6q6kten5nkzdo6ojxs5sbthtp7wjlnewid2l3iit55venn5pq.b32.i2p:6667

addnode=ker57lgh3hl5jqfflpwuuhmopqe54f3x23hggd2prwovfo3byw3q.b32.i2p:6667
addnode=h722acegogbffjgwd52bydcycifugd5lmvdnsk7rtuk2vjpge5ea.b32.i2p:6667
```

# Windows Setup

Note: this is the latest and least tested.  
Setup is similar to the above (either 'client' or 'server') but there're some differences.
- unpack binaries into a folder, and best copy your .ColossusXT (made as per the above instructions) into that folder as well.  
- run as `colx-qt -testnet -datadir=<path-to-.ColossusXT-folder>`
- datadir folder is important as it sets up things for both COLX and I2PD.
- you can enable firewall or not, both should run ok (tested before and works on either linux or win w/o any firewall or NAT setup), performance may vary only.
- make sure that your i2pd logs and files are not being saved to (and picked up from) `c:\Users\<user>\AppData\Roaming\i2pd\` (or similarly check under AppData\Local) as that seems to be the default location on Win.


# Firewalls, Other Setup  

Nothing to do here. No firewall setup is needed (except if wishing to improve performance and for public nodes as per the official documentation, but I didn't explore that avenue).  


# Usage

All should be the same except for a few things:  
- visuals are slightly different (as instead of IP-s we have i2p addresses).  
- initialization is a bit slower, you need to wait a bit for i2p network to kick in and start synchronizing things. Sometimes that's right away, sometimes it's couple minutes, could be more in some cases, most of the time you won't notice big difference but just slightly slower.
- block syncing is slower and performance in general (i.e. things are slow in GUI), see known issues, this isn't optimized at all (should be an easy fix).  
- some network scenarios are not tested, there may be issues etc. (again see known issues for more). 

