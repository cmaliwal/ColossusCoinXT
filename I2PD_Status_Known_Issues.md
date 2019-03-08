I2PD Setup
=====================================

[![Build Status](https://travis-ci.org/COLX-Project/COLX.svg?branch=i2pd)](https://travis-ci.org/COLX-Project/COLX) [![GitHub version](https://badge.fury.io/gh/COLX-Project%2FCOLX.svg)](https://badge.fury.io/gh/COLX-Project%2FCOLX)


# Known Issues 
.   

## Compiling
Generally all should compile and run ok, but there're still some issues to keep in mind:  
- Only Linux is supported (or tested), Windows is mostly there, was working at some point but isn't working right now (Linux testing only for the moment). It should be next thing to come I hope if no major issues.
- make is not picking up changes in the src/i2pd folder (I'll investigate this when time). For now just remove *.a, *.o from it and recompile.  
- in certain cases after a while build will fail with libminzip related errors. Something due to make clean or make install. Fix is to do a clean clone and build.  
- i2pd branch is a bit behind the master, it's based on the pre-NY master, all the changes afterwards (including the major GUI/qt changes) are not yet merged in.  


## Stability, Performance
COLX should run relatively stable now, I've fixed couple major problems and things are running ok for a while.  
- it crashes on close, some i2pd Log issue (and we're not stopping the i2p threads on exit, something to fix).
- performance is not best yet, especially during syncing. I'll probably just need to adjust priorities for the i2pd threads and processing, or halt it somewhat during syncing, again mostly the time issue.

# What needs to be done (todos, plan, testing...) 
.   

