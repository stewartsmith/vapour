# vapour
# Copyright (C) 2009, 2010 Monty Taylor
# Copyright (C) 2010 Stewart Smith
# All rights reserved.
#
# Use and distribution licensed under the BSD license.  See
# the COPYING.BSD file in this directory for full text.
#
# Parts also under GPL, see COPYING file in this directory for full text.

AC_INIT([vapour],[0.1],[http://launchpad.net/vapour])
AC_CONFIG_SRCDIR([src/vapour.c])
AC_CONFIG_AUX_DIR(config)
AM_CONFIG_HEADER([src/config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_PREFIX_DEFAULT([/usr])

PANDORA_CANONICAL_TARGET(ignore-shared-ptr)

PANDORA_REQUIRE_LIBMEMCACHED
PANDORA_REQUIRE_LIBMEMCACHEDPROTOCOL

WITH_MYSQL()

AC_CONFIG_FILES(Makefile)

AC_OUTPUT

echo "---"
echo "Configuration summary for $PACKAGE_NAME version $VERSION"
echo ""
echo "   * Installation prefix:       $prefix"
echo "   * System type:               $host_vendor-$host_os"
echo "   * Host CPU:                  $host_cpu"
echo "   * C Compiler:                $CC_VERSION"
echo "   * Assertions enabled:        $ac_cv_assert"
echo "   * Debug enabled:             $with_debug"
echo "   * Warnings as failure:       $ac_cv_warnings_as_errors"
echo ""
echo "---"

