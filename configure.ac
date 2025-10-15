AC_INIT([dccp], [6.15], [dreibh@simula.no])
AM_INIT_AUTOMAKE([foreign])

AC_PROG_CC
AM_PROG_AR
LT_INIT

AC_ARG_WITH([modules], AS_HELP_STRING([--without-modules], [skip building kernel modules]))

if test "x$with_modules" != "xno"; then
   kernel="/lib/modules/`uname -r`"
   if test -d "$kernel/kernel/net/dccp" && test "x$with_modules" != "xyes" ; then
      BUILTIN_MODULES=yes
      AC_MSG_NOTICE([DCCP module found in kernel, skipping build])
      test -f /usr/include/linux/dccp.h || AC_MSG_ERROR([no kernel-headers found])
   else
      AC_SUBST([MODULES], [modules])
      AC_SUBST([KERNEL_EXTRA], [$kernel/extra])

      if test -e "$kernel/build/Makefile" ; then
         # Ubuntu/Debian:
         AC_SUBST([KERNEL_BUILD], [$kernel/build])
      elif test -e "/usr/src/kernels/`uname -r`/Makefile" ; then
         # Fedora:
         AC_SUBST([KERNEL_BUILD], [/usr/src/kernels/$kernel])
      else
         AC_MSG_ERROR([Unable to find the sources of the currently running kernel!])
      fi

      AC_CONFIG_FILES([modules/Makefile])
   fi
else
   AC_MSG_NOTICE([--without-modules specified, skipping kernel module build])
fi

AM_CONDITIONAL([BUILTIN_MODULES], [test "x$BUILTIN_MODULES" = "xyes"])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
