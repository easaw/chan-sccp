# FILE: AutoMake Makefile for chan-sccp-b
# COPYRIGHT: chan-sccp-b.sourceforge.net group 2011
# CREATED BY: Diederik de Groot <ddegroot@sourceforge.net>
# LICENSE: This program is free software and may be modified and distributed under the terms of the GNU Public License version 3.
#          See the LICENSE file at the top of the source tree.
# NOTE: Process this file with automake to produce a makefile.in script.
AUTOMAKE_OPTS		= gnu silent-rules

bin_PROGRAMS 		= gen_sccpconf
man_MANS		= gen_sccpconf.1

gen_sccpconf_SOURCES 	= gen_sccpconf.c gen_sccpconf.h  @top_srcdir@/src/sccp_config_entries.hh @top_srcdir@/src/sccp_enum.h #@top_srcdir@/src/sccp_enum.c

gen_sccpconf_CPPFLAGS	= $(PBX_CPPFLAGS)
gen_sccpconf_CFLAGS	= $(AM_CFLAGS) $(PBX_CFLAGS) $(GDB_FLAGS) $(PTHREAD_CFLAGS) $(GCLIBS)
gen_sccpconf_CFLAGS	+= -D_REENTRANT -D_GNU_SOURCE -DCRYPTO -fPIC -pipe -Wall $(GCFLAGS) -I. -I../../src
gen_sccpconf_LDFLAGS	= -avoid-version -module  $(PBX_LDFLAGS) $(PTHREAD_LIBS) $(GCLIBS)

