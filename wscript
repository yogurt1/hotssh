#! /usr/bin/env python
# encoding: utf-8

import sys
import Configure, Common, python, intltool, misc

VERSION='0.2'
APPNAME='hotssh'
srcdir = '.'
blddir = 'build'

def set_options(opt):
	opt.tool_options('python')

def configure(conf):
	conf.check_tool('gcc gnome python intltool misc')
	conf.check_python_version((2,4,2))

	conf.check_python_module('dbus')
	conf.check_python_module('gobject')

	conf.define('VERSION', VERSION)
	conf.define('GETTEXT_PACKAGE', 'hotssh')
	conf.define('PACKAGE', 'hotssh')

def build(bld):
	obj = bld.create_obj('py')
	obj.find_sources_in_dirs(['hotssh'], exts=['.py'])
	obj.inst_dir = 'hotssh'
	obj = bld.create_obj('py')
	obj.find_sources_in_dirs(['hotssh/hotlib'], exts=['.py'])
	obj.inst_dir = 'hotssh/hotlib'
	obj = bld.create_obj('py')
	obj.find_sources_in_dirs(['hotssh/hotlib_ui'], exts=['.py'])
	obj.inst_dir = 'hotssh/hotlib_ui'
	obj = bld.create_obj('py')
	obj.find_sources_in_dirs(['hotssh/hotvte'], exts=['.py'])
	obj.inst_dir = 'hotssh/hotvte'
	# process desktop.in file
	obj=bld.create_obj('intltool_in')
	obj.source  = 'hotssh.desktop.in'
	obj.destvar = 'PREFIX'
	obj.subdir  = 'share/applications'
	obj.podir   = 'po'
	obj.flags   = '-d'
	install_files('PREFIX', 'share/applications', 'hotssh.desktop')
	install_files('PREFIX', 'share/doc/hotssh-' + VERSION, 'COPYING')
	if bld.env()['PREFIX'] == '/usr':
		install_files('PREFIX', '../etc/profile.d', 'hotssh.csh')
		install_files('PREFIX', '../etc/profile.d', 'hotssh.sh')
	bld.add_subdirs('bin po')
