Linux DCCP Module for Kernel 6.16+
==================================

This repository contains Linux DCCP from the Linux Kernel 6.15. Since DCCP was removed in Linux Kernel 6.16, the DCCP module needs to be compiled externally. It is particularly intended to be used by [Network Performance Meter&nbsp;(NetPerfMeter)](https://www.nntb.no/~dreibh/netperfmeter/), the network performance meter tool for the TCP, MPTCP, SCTP, UDP, and DCCP transport protocols over IPv4 and IPv6.

# Building the Module

Configure, build and install:

```bash
./autogen.sh
./configure --prefix=/usr
make
sudo make install
```

# Loading the Module

```bash
sudo modprobe dccp
lsmod | grep ^dccp
```

# Testing with NetPerfMeter

NetPerfMeter: [https://www.nntb.no/~dreibh/netperfmeter/](https://www.nntb.no/~dreibh/netperfmeter/)

Server:
```bash
netperfmeter 9000 -control-over-tcp
```

Client:
```bash
netperfmeter $SERVER:9000 -control-over-tcp \
   -dccp const10:const1000:const10:const1000
```
