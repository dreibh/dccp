Linux DCCP Kernel Module for Kernel >= 7.0.0
============================================

This repository contains Linux DCCP from Linux Kernel 6.15 ported to newer kernels (>= 6.16, including 7.x). Because DCCP was removed upstream in Linux 6.16, the DCCP modules are built out-of-tree. It is particularly intended for use with [Network Performance Meter (NetPerfMeter)](https://www.nntb.no/~dreibh/netperfmeter/).

# Installation via DKMS (Recommended)

DKMS automatically builds and installs `dccp`, `dccp_ipv4`, `dccp_ipv6`, and `dccp_diag`, and rebuilds them on future kernel upgrades:

```bash
sudo dkms add .
sudo dkms autoinstall
```

To remove the DKMS registration later:

```bash
sudo dkms remove dccp/7.0.0 --all
```

# Manual Build and Installation via CMake

Configure, build, and install the kernel modules and UAPI header (`<linux/dccp.h>`):

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

Useful CMake options:
- `-DWITH_MODULES=OFF`: Install only the UAPI header (`<linux/dccp.h>`) and build `dccp_test`.
- `-DWITH_TESTS=OFF`: Skip building `dccp_test`.
- `-DKERNEL_UNAME=<version>`: Target a specific installed kernel version instead of `uname -r`.
- `-DINSTALL_DKMS=ON`: Stage DKMS sources into `<prefix>/src/dccp-7.0.0` (for packaging).

# Loading the Module

```bash
sudo modprobe dccp
sudo modprobe dccp_ipv6
sudo modprobe dccp_diag
lsmod | grep ^dccp
```

# Testing with NetPerfMeter

Server:
```bash
netperfmeter 9000 -control-over-tcp
```

Client:
```bash
netperfmeter $SERVER:9000 -control-over-tcp \
   -dccp const10:const1000:const5:const500
```
