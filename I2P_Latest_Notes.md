I2P Latest Notes (052419)
=====================================

[![Build Status](https://travis-ci.org/COLX-Project/COLX.svg?branch=i2pd)](https://travis-ci.org/COLX-Project/COLX) [![GitHub version](https://badge.fury.io/gh/COLX-Project%2FCOLX.svg)](https://badge.fury.io/gh/COLX-Project%2FCOLX)

This should serve as a concise (hopefully) summary of what's important, for details please look up the other documents (and they should be updated shortly w some more info).  
  
# Important 
- **You can't just start using it, as it requires some initial (configuration) setup** for anything to work. See below (docs) for more.
- always run as `colx-qt/colxd -testnet -datadir=<path-to-.ColossusXT-folder>` - **datadir parameter is important (to specify)** as it sets up things for both COLX and I2PD.
 

# Status, Stability, bug reports
Code, nodes and testnet seem relatively stable. I've had loads of issues (expected given the sensitive nature of networking changes) but that's largely cleared. I'm still running into occasional problems (e.g. node stopping after a long while, or lagging behind) so you may expect to hit some.  
- If on linux best way to report is turning on core dumps and sending it compressed to me (that's relatively simple, contact me if you need some help). Plus the logs (i2pd.log in the bins folder + testnet/debug.log).
- peers.dat and mncache... files may sometimes be causing issues on restart, just delete them and restart.
- app fails (generates aborted message) at the very end, that's largely inconsequential, ignore it for now (I'll fix it shortly).
- logging is really extensive at the moment, both i2pd.log and debug.log could get very big very fast (to 500MB or more). Stop the app and clear them out, I'll fix, minimize that w/ the next update but I need all the logging.
- call me! - I've probably seen it all by now so don't hesitate to ask...

# Setup, Usage, Blockchain...
1) unpack either client.ColossusXT.tar.gz or server.ColossusXT.tar.gz - and use that as a template for .ColossusXT (data dir).   
**This is important** as there's no other way (at the moment) to set up i2pd properly (i.e. it requires all files that are in there, if you're missing and address or peers dir it won't work and so on). Also most of the i2pd files (even i2pd.conf, tunnels.conf) shouldn't be touched or changed really (best params are already in place) or are auto-generated.
2) generate new (fixed) address - only if you wish to have fixed/shared address (and accept connections from others), recommended for testing (more on addresses later).  
Make sure my-keys.dat and destinations folder are removed. Start the app briefly, stop it right away (after a few seconds). my-keys.dat and destinations folder should be generated. Under destinations you'll get a <hash>.dat file generated. Copy the <hash> part (that's your public key hash - i.e. your unique address). Open the ColossusXT.conf, replace with your newly generated hash instead - i.e. in the form of `<new-hash-w/o.dat>.b32.i2p:6667` (port where needed, see .conf comments for where and how). This is the same for both client and server (just different params).
3) go to testnet4 folder and copy your wallet.dat and masternode.conf files in there - that's all you need. If you wish to start from scratch just leave it empty.
4) Now you can start the app again. It should start syncing within a few minutes (but it takes a bit to get started, that's the slowest bit, creating tunnels etc.), once it starts it's slower (than sockets) but it's going fine.
5) i2p testnet is based on the current testnet (forked at ~100K block), so all previous tx-s, MN collaterals should be in there, and you can use your existing wallets (from the testnet). But just wallets (not other files, as they're not compatible).
6) MN setup - that part is pretty much the same as before, it's just 'addresses' that are different. You can safely reuse your existing private keys, tx-s and the entire setup. Edit your masternode.conf file, replace the ip-address with the new i2p.b32.i2p:6667 style address, and that's pretty much it (and do the same on the MN side, the ColossusXT.conf file). The rest of the MN hot/cold setup is the same. Even more, setup is slightly easier as you don't have to worry about public ip/port exposing.
7) Addresses - you can share your generated i2p address with other nodes (addnode, peers) - as that's your 'public' address and it stays fixed until you remove the my-keys.dat and destinations dir. If you want to rotate that, just remove those files and create a new address. Or just don't specify `externalip` or `bind` (and remove my-keys/destinations) and new address should be generated each time (on restart). 
8) You don't really need to have public ip:port open but it may help w/ performance (and makes sense at least for MN-s and 'server' nodes). It all works nicely over NAT, I've tested behind couple routers, and bridged VM, all fine, not especially slow either (needs testing). I've tried adjusting some i2pd.conf params to lower tunnelling (through) but while that works it also significantly slows down COLX network as well, so that's an no go at the moment.
9) Blockchain is the same (forked) existing testnet, I didn't add any special blocks for funds (I wanted to have easy transition, reuse and avoid forking, seemed easier), you can reuse existing wallet funds (up to ~100K mark), or we should aks Aliaksei to log in and send some as before (or I just need some wealthy wallet.dat and I'll take care of it).

# Files 
- https://1drv.ms/f/s!AnrEFg2ff_U_h94nDSHB0yuzCPgaNQ - let me know if any issues and I'll set up an alternative. 
- no 'android' binaries (for the moment, i2p has some issues compiling, will be shortly/next).
- client.ColossusXT.tar.gz, server.ColossusXT.tar.gz - use these files to quickly setup data dir-s, basically use them as templates for either 'client' (non-MN) or 'server' (MN) setup. Read through the ColossusXT.conf as it contains necessary notes.
- snapshot_052419.tar.gz - only as a temp measure and if you really wish to get going fast and know what you're doing - (and may cause problems difficult to track down). I don't wish to encourage this, but just use it as a template, and add your wallet.dat/masternode.conf to the testnet4, nothing else (no other files, so 4 subdirs and wallet.dat, possibly masternode.conf, that's all). Start and it should start from the very recent block. But keep in mind this isn't ideal. You should ideally start from scratch, just wallet.dat (or no wallet), and let it sync from the start, that results in a more stable structure, less surprises. 

# Nodes
```
addnode=h722acegogbffjgwd52bydcycifugd5lmvdnsk7rtuk2vjpge5ea.b32.i2p:6667
addnode=ker57lgh3hl5jqfflpwuuhmopqe54f3x23hggd2prwovfo3byw3q.b32.i2p:6667
addnode=xqt6q6kten5nkzdo6ojxs5sbthtp7wjlnewid2l3iit55venn5pq.b32.i2p:6667
addnode=otpnrrgoueyirvtg6jn5hrs6qny4v5kqv2xvy4d3uqrv7d6y6zxa.b32.i2p:6667
addnode=o5nwexnmfj73nun24unqod72njlst4bz5fz6x6wkcpdp3qqrfn4q.b32.i2p:6667
addnode=jeipu2cxzqphi6q7fvzhzyjn2ndk5h7gxnxsa6gfccrmliqlcn5q.b32.i2p:6667
addnode=275ucmvmcp7lspeprvijbhnjlw74sihces5ayuxjmb7r7gxdncoq.b32.i2p:6667
addnode=esg42kihxfcwdi7xyygrf7kwylndw6li24adgw777yhfpxgs5jra.b32.i2p:6667
addnode=vflaa3ywbqpapcxaik2dowa57wb2nuqza7nrc3ch5wa7ck5qtyha.b32.i2p:6667
addnode=uwfo2s5ve2dkovci2dsy4axmfehka32jhe6xayjhhpvcrjkhwchq.b32.i2p:6667
addnode=pguwb5pqjpyi57eknlwulumf4hf4xozupxrtl4bc23mwifywngsa.b32.i2p:6667
```

# Testnet details
- 11 nodes at the moment - but most of them are minimal (1GB RAM) smallest VM-s, which is pretty much going into swap at all times (not ideal). I.e. we'll need more dedicated, larger nodes for speed to pick up somewhat.
- overall, I'm very happy w/ the testnet as it's working right now (wasn't the case last week or few days ago, but it's looking good now). There're still issues from time to time, but that something that needs filtering out and some time (the level of changes and in the very critical networking layer requires more testing, but I think I'm now pretty much done w/ the big problems).

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
mncache.dat | MN cache file (regenerated) * | testnet4 sub directory | client, server | | 
peers.dat | addresses (regenerated) * | testnet4 sub directory | client, server | | 
  
# Small Local Testnet Setup  
For quick testing (and testing purposes only) you can connect couple of nodes of your own (this is now getting to be slightly easier than before, as you don't need any public IP-s etc.). This gets real handy and will make our life much easier on a long-run (was slightly more pain for me this first time, but let me know and I'll walk you through if needed).


