dnl Process this file with autoconf to produce a configure script.

dnl Minimum Autoconf version required.
AC_PREREQ(2.59)

AC_INIT([fping],[5.3])

m4_ifdef([AC_AUTOCONF_VERSION],[AC_USE_SYSTEM_EXTENSIONS], [AC_GNU_SOURCE])

# Detect Operatingsystem
AC_CANONICAL_TARGET
only_clock_realtime=no

case "${target}" in
  *darwin*)
    only_clock_realtime=yes
    ;;
  *freebsd*)
    only_clock_realtime=yes
    ;;
  *openbsd*)
    only_clock_realtime=yes
    ;;
esac

dnl --disable-ipv4
AC_ARG_ENABLE([ipv4],
  AS_HELP_STRING([--disable-ipv4], [Disable support for pinging IPv4 hosts]))
AM_CONDITIONAL([IPV4], [test "x$enable_ipv4" != "xno"])
AM_COND_IF([IPV4], [AC_DEFINE([IPV4], [1], [IPv4 enabled])])

dnl --disable-ipv6
AC_ARG_ENABLE([ipv6],
  AS_HELP_STRING([--disable-ipv6], [Disable support for pinging IPv6 hosts]))
AS_IF([test "x$enable_ipv6" != "xno"], [
    dnl Test if IPv6 is supported
       AC_CHECK_HEADERS([netinet/icmp6.h], [have_ipv6="yes"], [], [[
      #include <netinet/in.h>
      #include <sys/types.h>
  ]])
])
dnl Can't disable both IPv4 and IPv6
AS_IF([test "x$enable_ipv4" = "xno" -a "x$enable_ipv6" = "xno"], [
    AC_MSG_ERROR([Need to enable IPv4 or IPv6. Can't disable both!)])
])
dnl IPv6 required, but not supported?
AS_IF([test \( "x$enable_ipv6" = "xyes" -o "x$enable_ipv4" = "xno" \) -a "x$have_ipv6" != "xyes" ], [
    AC_MSG_ERROR([IPv6 not supported on this platform (netinet/icmp6.h header not found)])
])
AM_CONDITIONAL([IPV6], [test "x$have_ipv6" = "xyes"])
AM_COND_IF([IPV6], [AC_DEFINE([IPV6], [1], [IPv6 enabled])])

AC_ARG_ENABLE([timestamp],
  AS_HELP_STRING([--disable-timestamp], [Disable kernel-based packet timestaping (SO_TIMESTAMPNS)]))
AS_IF([test "x$enable_timestamp" != "xno"], [
   AC_CHECK_DECL([SO_TIMESTAMPNS], [AC_DEFINE(HAVE_SO_TIMESTAMPNS, [1], [SO_TIMESTAMPNS is defined])], [have_so_timestamp="no"], [#include <sys/types.h>
#include <sys/socket.h>])
])
dnl Test if --enable-timestamp is explicitely enabled and make an error if this platform doesn't support it
AS_IF([test "x$enable_timestamp" = "xyes" -a "x$have_so_timestamp" = "xno"], [
  AC_MSG_ERROR([--enable-timestamp not supported on this platform])
])
AS_IF([test "x$only_clock_realtime" = "xyes"], [AC_DEFINE(ONLY_CLOCK_REALTIME, [1], [ONLY_CLOCK_REALTIME is defined])])

AC_ARG_ENABLE([safe-limits],
  AS_HELP_STRING([--enable-safe-limits], [Restrict timing parameters (-i, -p) within "safe" limits]))
AS_IF([test "x$enable_safe_limits" = "xyes"], [
   AC_DEFINE(FPING_SAFE_LIMITS, [1], [safe limits should be enforced])])

AC_ARG_ENABLE([debug],
  AS_HELP_STRING([--enable-debug], [enable debugging @<:@default=no@:>@]), [enable_debug=$enableval], [enable_debug=no])
AS_IF([test "x$enable_debug" = "xyes"], [
  AC_DEFINE([DEBUG], [1], [Define if debugging is enabled])])

AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AM_MAINTAINER_MODE

AC_CONFIG_HEADERS([config.h])

dnl Checks for programs.

AC_PROG_CC
AM_PROG_CC_C_O
m4_version_prereq([2.70],,[AC_PROG_CC_STDC])
AC_PROG_CPP
AC_PROG_INSTALL

dnl Checks for libraries.

AC_CHECK_FUNC(gethostbyname)
if test $ac_cv_func_gethostbyname = no; then
    AC_CHECK_LIB(nsl, gethostbyname)
fi
AC_CHECK_FUNC(connect)
if test $ac_cv_func_connect = no; then
    AC_CHECK_LIB(socket, connect)
fi
AC_CHECK_FUNC(sigaction)
if test $ac_cv_func_sigaction = yes; then
    AC_DEFINE([USE_SIGACTION],[1],[Define if sigaction is available.])
fi

AC_CHECK_FUNCS([strftime], [],
  [AC_MSG_ERROR([strftime function is required but not found])])

AH_TOP([
#ifndef CONFIG_H
#define CONFIG_H
])

AH_BOTTOM([
/* some OSes do not define this ... lets take a wild guess */

#ifndef INADDR_NONE
#  define INADDR_NONE 0xffffffffU
#endif

#endif /* CONFIG_H */

])

dnl Checks for header files.
AC_CHECK_HEADERS([unistd.h sys/file.h stdlib.h sys/select.h])

AC_CONFIG_FILES([Makefile
                 doc/Makefile
                 src/Makefile])

AC_OUTPUT
