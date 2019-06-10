I2P Latest Notes (061019)
=====================================

[![Build Status](https://travis-ci.org/COLX-Project/COLX.svg?branch=i2pd)](https://travis-ci.org/COLX-Project/COLX) [![GitHub version](https://badge.fury.io/gh/COLX-Project%2FCOLX.svg)](https://badge.fury.io/gh/COLX-Project%2FCOLX)

This is a concise summary of things important (for details please look up the other documents).  

#  Status (061019)
A rather substantial update:
- it's quite stable and reliable. 
- all known issues resolved (including a nagging and extremely rare and tricky stream close errors). 
- no real (breaking) issues for days now (there will be issues to tackle regularly but nothing major for a while and I don't expect those any more).
- Testnet is stable, nodes and blocks following each other closely. Quite a few changes, block download algorithm adjusted (to work better w/ i2p - it was somewhat flawed but that never manifested over sockets) etc. It should be resolved by now.   
- performance is ok enough (more under known issues).

#  Known Issues
1) **slow i2p network initialization** (on app start). It takes a few minutes for i2p to initialize. That's the slowest part and likely the most UX issue of all (but we should add some notifications, splash or something, so that the user is aware of what's going on). I didn't do any optimization on this, so there's space to improve (and ways to tackle this, I've elaborated on test channel) but it is inherently an i2p thing (i.e. it'll never be as fast as sockets, though I expect it to be faster).
2) **Slow recovery time, occasional ups and downs** - network sync slowdowns. I.e. once the 'flow' of info gets interrupted in some way (e.g. internet connection issues, forked nodes/blocks, or even i2p network slow patches etc.) it takes a bit for node to recover and rebuild the tunnels. This shouldn't normally happen though. The problem is w/ how i2p tunnels work (temporary and unstable, many hops and all) and rebuilding that is costly (if you have to reinit everything, couple tunnels it's not an issue). I've also noticed (I think) occasional i2p network slowdowns during the night (not confirmed though). Network latency (over i2p) is not really an issue (that I noticed) but it exists and it's not as prompt as over direct socket connection (when you need to recover fast). Couple testnet issues were directly related to all this (I've already fixed/adjusted the block downloading etc.), it's something we have to keep in mind.
3) qt, peers list, **addresses are wrong for 'Inbound' connections** - i.e. you get number of connections having the same 'address' in the list, but in fact that's showing the current node's address (as a 'server'). No brainer just to get to do it.
4) On Windows only: wallet stops syncing after initial sync (while showing as up-to-date) - I'm unable to repeat this (possibly a combo of factors, config and bad manual setup). Workaround (how to resolve): go to `i2pd.conf` and comment `nat=true` (that one should be off). Also reinstall from .exe properly. Firewall settings are irrelevant (app asks for firewall exception but that's not used any more and should be removed, only for local sockets). Let me know if you get to repeat this consistently (zip and send me your data dir).
5) Android - is available now under 'arm' subdir but not tested (I mean literally not tested, just managed to compile it).  

I think that's pretty much it at this point. Shutdown should now work fine (no errors any more), everything else is pretty much as over 'normal'/sockets based network (all messages, tx-s flow, MN-s etc).  
**There's room for improvement (mostly performance, network init, reconnecting times etc.) but it should all be stable and I'd say fully functional**.  

#  How To Generate New Public Address / Keys (the new way)
- Edit your ColossusXT.conf and make it look like this:
```
# for 'public' nodes:
port=6667 
externalip=
bind=
# bind= means literally that, leave the right side empty, the app will take care of it.
```
```
# or for MN-s:  
port=6667 
externalip=
bind=
masternodeaddr=
masternodeprivkey=<your MN-s private key>
masternode=1
```
```
# for 'anonymous' nodes make sure you DON't have any of the above!
```
- Start your wallet (colx-qt or colxd) with the following param `-generatenewi2p` - e.g. `./colx-qt -testnet -datadir=/home/user/i2p/.ColossusXT -generatenewi2p` (**one-time only** - remove the param on next run). That will generate the new public address (my-keys.dat and destinations/.dat) and make it immediatelly active (there's no need to restart the app, config or set up anything). 
- Open the RPC console (qt console or colx-cli) and type: `getserverdestination`. That will return your new public address (JSON list as we could have many). Copy and paste it to share with others (full **valid address always includes the port**).  

Repeat the process whenever you wish to make a new address.

#  Android Version
- It's under its own 'arm' subdir (https://1drv.ms/f/s!AnrEFg2ff_U_h98eiQniM60nL5ROKA).
- If someone could please test this and let me know if it works.


#  TESTNET Notes
Testnet is all new, it's using different blocks (other testnet latest up to 115379 or so), addresses are different (deliberately to avoid issues), i.e. **it's incompatible** with the previous one. 
1) **mandatory** new binaries (1.3.0-i2pd-testnet) - https://1drv.ms/f/s!AnrEFg2ff_U_h98eiQniM60nL5ROKA - please make sure you have the latest bins (**previous bins are not compatible and will lead to forks**).
2) Please! **sync from scratch**, don't reuse previous blocks as you'll just fork (or lead to testnet forks, performance issues). What I like to do is:  
- remove `testnet4` (just that subdir, keep all the other stuff in .ColossusXT, i2p stuff, conf-s etc.),
- make new `testnet4` (optionally copy just `wallet.dat` and `masternode.conf` from the old one, if you wish to retain the wallet/conf, but **nothing else**),
- adjust the .ColossusXT/ColossusXT.conf to include new nodes (addnode stuff),
- or instead of all this just use the new `client.ColossusXT.tar.gz` (to keep the address - copy over my-keys.dat, destinations/),
- start the app 
3) use **new nodes** (old ones are no long active):
```
addnode=wu4bhd35j52oyofsne6753itgdzluttdapbnofyhqfitdhuxavnq.b32.i2p:6667
addnode=fhlc56vugmxtvu7vcwjgeux5zo5sxmkyh22vlivxy6djzjwvog6a.b32.i2p:6667
addnode=r4tfv6o4r4nfcer5twa3c45zex65m6e5l4qwssqkgu5hglfazhza.b32.i2p:6667
addnode=7qyae3i62rzm3uepvi7rgve425xlwjdi2zszt53lfgg2bif43e2a.b32.i2p:6667
addnode=jaqbemi6w2ivfelk4y4s76dbzaf5mwrpbd747kuzyrdn53rnlu6q.b32.i2p:6667
addnode=4auuuzc53zwk4zi3ldvjlylcqr5cahm3ifqpdfbrhk3qpr5d3yta.b32.i2p:6667
addnode=zc5dbzjrvm2osaiti2xyz3qs63xj3boclp4cmdwxebl5l6hd4wsa.b32.i2p:6667
addnode=2enj3uuj4xbkktw3kf5jwirrr5zcycljptkha5ito5ffottdbyfq.b32.i2p:6667
addnode=ooaur4xf7udcnvchwtx6ivznfw4ol4w6vdsnglcg2bconft7ibcq.b32.i2p:6667
addnode=ks3rjjbbqxsc72qnqfz44jjovoumaotczmdzd3tvysoprvk6t32a.b32.i2p:6667
addnode=avdpfoyw5xoih737jxey24x2hd5nhthdvunrrgrwnzgdi2vdirfa.b32.i2p:6667
```

# Important 
- **You can't just start using it, as it requires some initial (configuration) setup** for anything to work. See below (docs) for more.
- always run as `colx-qt/colxd -testnet -datadir=<path-to-.ColossusXT-folder>` - **datadir parameter is important (to specify)** as it sets up things for both COLX and I2PD.
- if any issues syncing from scratch - try commenting the `nat = true` in the i2pd.conf file (.ColossusXT data dir). ~~By default I've made to be 'behind nat'~~ (that setting is problematic, should be commented out). 

# Bug Reports
- If on linux best way to report is turning on core dumps and sending it compressed to me (that's relatively simple, contact me if you need some help). Plus the logs (i2pd.log in the bins folder + testnet/debug.log).
- logging is really extensive at the moment, both i2pd.log and debug.log could get very big very fast (to 500MB or more). Stop the app and clear them out, I'll fix, minimize that w/ the next update but I need all the logging.

# Setup, Usage, Blockchain...
1) unpack either client.ColossusXT.tar.gz or server.ColossusXT.tar.gz - and use that as a template for .ColossusXT (data dir).   
**This is important** as there's no other way (at the moment) to set up i2pd properly (i.e. it requires all files that are in there, if you're missing and address or peers dir it won't work and so on). Also most of the i2pd files (even i2pd.conf, tunnels.conf) shouldn't be touched or changed really (best params are already in place) or are auto-generated.
2) generate new (fixed) address - only if you wish to have fixed/shared address (and accept connections from others), recommended for testing (more on addresses later).  
Make sure my-keys.dat and destinations folder are removed. Start the app briefly, stop it right away (after a few seconds). my-keys.dat and destinations folder should be generated. Under destinations you'll get a <hash>.dat file generated. Copy the <hash> part (that's your public key hash - i.e. your unique address). Open the ColossusXT.conf, replace with your newly generated hash instead - i.e. in the form of `<new-hash-w/o.dat>.b32.i2p:6667` (port where needed, see .conf comments for where and how). This is the same for both client and server (just different params).
3) (optional) go to testnet4 folder and copy your wallet.dat and masternode.conf files in there - that's all you need. If you wish to start from scratch just leave it empty.
4) Now you can start the app again. It should start syncing within a few minutes (but it takes a bit to get started, that's the slowest bit, creating tunnels etc.), once it starts it's slower (than sockets) but it's going fine. **Note** - if any issues starting (stuck at 0), check the connection (I know), make sure testnet4 is empty (delete it) and restart the app.
5) i2p testnet is based on the current testnet (forked at ~115K block), so all previous tx-s, MN collaterals should be in there, and you can use your existing wallets (from the testnet). But just wallet.dat-s (not other files, as they're not compatible).
6) MN setup - that part is pretty much the same as before, it's just 'addresses' that are different. You can safely reuse your existing private keys, tx-s and the entire setup. Edit your masternode.conf file, replace the ip-address with the new i2p.b32.i2p:6667 style address, and that's pretty much it (and do the same on the MN side, the ColossusXT.conf file). The rest of the MN hot/cold setup is the same. Even more, setup is slightly easier as you don't have to worry about public ip/port exposing.
7) Addresses - you can share your generated i2p address with other nodes (addnode, peers) - as that's your 'public' address and it stays fixed until you remove the my-keys.dat and destinations dir. If you want to rotate that, just remove those files and create a new address. Or just don't specify `externalip` or `bind` (and remove my-keys/destinations) and new address should be generated each time (on restart). 
8) You don't really need to have public ip:port open (firewall etc.) but it may help w/ performance (and makes sense at least for MN-s and 'server' nodes). It all works nicely over NAT, I've tested behind couple routers, and bridged VM, all fine, not especially slow either (needs testing). I've tried adjusting some i2pd.conf params to lower tunnelling (through) but while that works it also significantly slows down COLX network as well, so that's an no go at the moment.
9) Blockchain is the same (forked) existing testnet, so you can reuse existing wallet funds (up to ~115K mark), or let me know your account address and I'll reroute some funds.

# Files 
- https://1drv.ms/f/s!AnrEFg2ff_U_h98eiQniM60nL5ROKA - let me know if any issues and I'll set up an alternative. 
- client.ColossusXT.tar.gz, server.ColossusXT.tar.gz - use these files to quickly setup data dir-s, basically use them as templates for either 'client' (non-MN) or 'server' (MN) setup. Read through the ColossusXT.conf as it contains necessary notes.
- ~~snapshot_052419.tar.gz~~ (will be revisited) - only as a temp measure and if you really wish to get going fast and know what you're doing - (and may cause problems difficult to track down). I don't wish to encourage this, but just use it as a template, and add your wallet.dat/masternode.conf to the testnet4, nothing else (no other files, so 4 subdirs and wallet.dat, possibly masternode.conf, that's all). Start and it should start from the very recent block. But keep in mind this isn't ideal. You should ideally start from scratch, just wallet.dat (or no wallet), and let it sync from the start, that results in a more stable structure, less surprises. 

# Nodes
```
addnode=wu4bhd35j52oyofsne6753itgdzluttdapbnofyhqfitdhuxavnq.b32.i2p:6667
addnode=fhlc56vugmxtvu7vcwjgeux5zo5sxmkyh22vlivxy6djzjwvog6a.b32.i2p:6667
addnode=r4tfv6o4r4nfcer5twa3c45zex65m6e5l4qwssqkgu5hglfazhza.b32.i2p:6667
addnode=7qyae3i62rzm3uepvi7rgve425xlwjdi2zszt53lfgg2bif43e2a.b32.i2p:6667
addnode=jaqbemi6w2ivfelk4y4s76dbzaf5mwrpbd747kuzyrdn53rnlu6q.b32.i2p:6667
addnode=4auuuzc53zwk4zi3ldvjlylcqr5cahm3ifqpdfbrhk3qpr5d3yta.b32.i2p:6667
addnode=zc5dbzjrvm2osaiti2xyz3qs63xj3boclp4cmdwxebl5l6hd4wsa.b32.i2p:6667
addnode=2enj3uuj4xbkktw3kf5jwirrr5zcycljptkha5ito5ffottdbyfq.b32.i2p:6667
addnode=ooaur4xf7udcnvchwtx6ivznfw4ol4w6vdsnglcg2bconft7ibcq.b32.i2p:6667
addnode=ks3rjjbbqxsc72qnqfz44jjovoumaotczmdzd3tvysoprvk6t32a.b32.i2p:6667
addnode=avdpfoyw5xoih737jxey24x2hd5nhthdvunrrgrwnzgdi2vdirfa.b32.i2p:6667
```

# Testnet details
- 11 nodes at the moment - but most of them are minimal, smallest VM-s (1GB RAM, low CPU), which is not ideal. I.e. we'll need more dedicated, larger nodes for speed to pick up somewhat.

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


