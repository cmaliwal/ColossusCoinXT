I2PD Notes
=====================================

[![Build Status](https://travis-ci.org/COLX-Project/COLX.svg?branch=i2pd)](https://travis-ci.org/COLX-Project/COLX) [![GitHub version](https://badge.fury.io/gh/COLX-Project%2FCOLX.svg)](https://badge.fury.io/gh/COLX-Project%2FCOLX)

# I2P / I2PD integration
I2PD is now part of the COLX networking layer and it's where bulk of the changes are. Old sockets-based networking is (fully) replaced and it's now using I2P network for messages and communication in between the nodes.  
Other parts (of the COLX) should pretty much work as before, only network related calls are replaced or adjusted to use new format and structures.

# I2P address vs IP
Major change is in how the nodes are identified in the I2P world. IP-s are no longer used (except for some initial seeding, downloads and similar) and should be replaced (see `I2PD_Setup_Usage.md`) with I2P equivalents.   
I2P 'address' (called destination) is sort of a 'i2p url' and looks e.g. like `ker57lgh3hl5jqfflpwuuhmopqe54f3x23hggd2prwovfo3byw3q.b32.i2p:6667` (i.e. <hash>.b32.i2p:<port>). Address is not an arbitrary string, it's actually a hash of a cryptographic key that is guaranteed to be unique and is generated based on the private/public key pair created for each node - i.e. only one (original) node that has the private key can claim that address. See `I2PD_Setup_Usage.md` for more.  
Each node can have more than one `destination` and usually does.  
Even though every i2p address is in a way private some nodes have to have their addresses public and published on the network - for network to function.  

# Types of I2P (and COLX) nodes 
Given COLX and I2P specifics we can have different combinations and types of nodes.
But for the moment nodes are roughly categorized as: 
- Private/Anonymous/Silent (or 'client-only' nodes): this node's i2p address is anonymous and is not published and other nodes can't connect to it or request info. Only routers and nodes that are directly contacted by this node can have and know its destination (and can communicate with it in both directions). 
- Public/MN (masternode) (or also 'server' nodes): these nodes are 'published' and are accepting connections form others (client or server nodes).
- Routers: is i2p designation, and are nodes that support the network traffic to go through them. In our case that'd be public/MN-s + all others on the i2p network that have nothing to do w/ the i2p.
- Floodfills: only specially designated nodes (on the i2p) that hold the network database info. COLX will likely not participate in it (in most cases at least).  

Private nodes (while not having published destinations) could also be 'listening' and participating in the COLX family traffic only - i.e. clients could also be servers and serve info to others. But for the moment we only have the above 2 types - pure clients and pure servers (i.e. that's tested, other combinations would need slight code and parameter adjustments, specific configuration and testing).  

# How it works 
Communication in between nodes is fully bi-directional, once connection is established - and doesn't require any firewall setup (though nodes may be setup with public ip:port with improved performance - but that wasn't explored at the moment, see previous discussion on this). For performance and more see the `I2PD_Setup_Usage.md`.   




