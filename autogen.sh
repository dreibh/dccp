#!/bin/sh

GEN="compile config.guess config.sub depcomp install-sh missing aclocal.m4 \
     configure config.h.in autom4te.cache ltmain.sh ar-lib INSTALL \
     Makefile.in modules/Makefile.in"

rm -rf $GEN

libtoolize --force --copy
aclocal
autoheader
automake --add-missing --copy --gnu
autoconf
