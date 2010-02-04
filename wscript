#! /usr/bin/env python
# encoding: utf-8

import sys
import Configure
import gnome, python, intltool, misc

VERSION='0.2.6'
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
	obj = bld.new_task_gen('py')
	obj.find_sources_in_dirs(['hotssh'], exts=['.py'])
	obj.install_path = '${PYTHONDIR}/hotssh'
	obj = bld.new_task_gen('py')
	obj.find_sources_in_dirs(['hotssh/hotlib'], exts=['.py'])
	obj.install_path = '${PYTHONDIR}/hotssh/hotlib'
	obj = bld.new_task_gen('py')
	obj.find_sources_in_dirs(['hotssh/hotlib_ui'], exts=['.py'])
	obj.install_path = '${PYTHONDIR}/hotssh/hotlib_ui'
	obj = bld.new_task_gen('py')
	obj.find_sources_in_dirs(['hotssh/hotvte'], exts=['.py'])
	obj.install_path = '${PYTHONDIR}/hotssh/hotvte'
	# process desktop.in file
	obj=bld.new_task_gen('intltool_in')
	obj.source  = 'hotssh.desktop.in'
	obj.install_path = '${PREFIX}/share/applications'
	obj.subdir  = 'share/applications'
	obj.podir   = 'po'
	obj.flags   = '-d'
	bld.install_files('${PREFIX}/share/doc/hotssh-' + VERSION, 'COPYING')
	bld.add_subdirs('bin po')
