# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, see <http://www.gnu.org/licenses/>.

include Makefile-decls.am

ACLOCAL_AMFLAGS += -I m4 ${ACLOCAL_FLAGS}
AM_CPPFLAGS += -DDATADIR='"$(datadir)"' -DLIBEXECDIR='"$(libexecdir)"'
AM_CFLAGS += $(WARN_CFLAGS)

EXTRA_DIST += autogen.sh COPYING
GITIGNOREFILES += aclocal.m4 build-aux config.h.in m4

libgsystem_srcpath := libgsystem
libgsystem_cflags = $(BUILDDEP_GIO_UNIX_CFLAGS) -I$(srcdir)/libgsystem
libgsystem_libs = $(BUILDDEP_GIO_UNIX_LIBS)
include libgsystem/Makefile-libgsystem.am
noinst_LTLIBRARIES += libgsystem.la

include Makefile-src.am

release-tag:
	git tag -m "Release $(VERSION)" v$(VERSION)

-include $(top_srcdir)/git.mk
