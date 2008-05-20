# This file is part of the Hotwire Shell user interface.
#   
# Copyright (C) 2007 Colin Walters <walters@verbum.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

import os,sys,platform,logging,getopt,re
import locale,threading,subprocess,time
import signal,tempfile,shutil,stat,pwd
import datetime

import xml.dom.minidom

try:
    import sqlite3
except:
    from pysqlite2 import dbapi2 as sqlite3

import gtk,gobject,pango
import dbus,dbus.glib,dbus.service

try:
    import avahi
    avahi_available = True
except:
    avahi_available = False

from hotssh.hotvte.vteterm import VteTerminalWidget
from hotssh.hotvte.vtewindow import VteWindow
from hotssh.hotvte.vtewindow import VteApp
from hotssh.hotlib.logutil import log_except
from hotssh.hotlib.timesince import timesince
from hotssh.hotlib_ui.quickfind import QuickFindWindow
from hotssh.hotlib_ui.msgarea import MsgAreaController

_logger = logging.getLogger("hotssh.SshWindow")

_whitespace_re = re.compile('\s+')

class SshConnectionHistory(object):
    def __init__(self):
        self.__statedir = os.path.expanduser('~/.hotwire/state')
        try:            
            os.makedirs(self.__statedir)
        except:
            pass
        self.__path = path =os.path.join(self.__statedir, 'ssh.sqlite')
        self.__conn = sqlite3.connect(path, isolation_level=None)
        cursor = self.__conn.cursor()
        cursor.execute('''CREATE TABLE IF NOT EXISTS Connections (bid INTEGER PRIMARY KEY AUTOINCREMENT, host TEXT, user TEXT, options TEXT, conntime DATETIME)''')
        cursor.execute('''CREATE INDEX IF NOT EXISTS ConnectionsIndex1 on Connections (host)''')
        cursor.execute('''CREATE INDEX IF NOT EXISTS ConnectionsIndex2 on Connections (host,user)''')
        cursor.execute('''CREATE TABLE IF NOT EXISTS Colors (bid INTEGER PRIMARY KEY AUTOINCREMENT, pattern TEXT UNIQUE, fg TEXT, bg TEXT, modtime DATETIME)''')
        cursor.execute('''CREATE INDEX IF NOT EXISTS ColorsIndex on Colors (pattern)''')    

    def get_recent_connections_search(self, host, limit=10):
        cursor = self.__conn.cursor()
        params = ()
        q = '''SELECT user,host,options,conntime FROM Connections '''
        if host:
            q += '''WHERE host LIKE ? ESCAPE '%' '''
            params = ('%' + host.replace('%', '%%') + '%',)
        # We use a large limit here to avoid the query being totally unbounded
        q += '''ORDER BY conntime DESC LIMIT 1000'''
        def sqlite_timestamp_to_datetime(s):
            return datetime.datetime.fromtimestamp(time.mktime(time.strptime(s[:-7], '%Y-%m-%d %H:%M:%S')))
        # Uniquify, which we coudln't do in the SQL because we're also grabbing the conntime
        seen = set()         
        for user,host,options,conntime in cursor.execute(q, params):
            if len(seen) >= limit:
                break
            if user:
                uhost = user+'@'+host
            else:
                uhost = host
            if uhost in seen:
                continue
            seen.add(uhost)
            # We do this just to parse conntime
            yield user,host,options,sqlite_timestamp_to_datetime(conntime)  

    def get_users_for_host_search(self, host):
        for user,host,options,ts in self.get_recent_connections_search(host):
            yield user

    def add_connection(self, host, user, options):
        cursor = self.__conn.cursor()
        cursor.execute('''BEGIN TRANSACTION''')
        cursor.execute('''INSERT INTO Connections VALUES (NULL, ?, ?, ?, ?)''', (user, host, ' '.join(options), datetime.datetime.now()))
        cursor.execute('''COMMIT''')

    def get_color_for_host(self, host):
        cursor = self.__conn.cursor()
        res = cursor.execute('SELECT fg,bg FROM Colors WHERE pattern = ? ORDER BY conntime DESC LIMIT 10')
        if len(res) > 0:
            return res[0]
        return None

    def set_color_for_host(self, host, fg, bg):
        cursor.execute('''BEGIN TRANSACTION''')
        cursor.execute('''INSERT INTO Colors VALUES (NULL, ?, ?, ?, ?)''', (host, fg, bg, datetime.datetime.now()))
        cursor.execute('''COMMIT''')
_history_instance = None
def get_history():
    global _history_instance
    if _history_instance is None:
        _history_instance = SshConnectionHistory()
    return _history_instance

class OpenSSHKnownHostsDB(object):
    def __init__(self):
        super(OpenSSHKnownHostsDB, self).__init__()
        self.__path = os.path.expanduser('~/.ssh/known_hosts')
        self.__hostcache_ts_size = (None, None)
        self.__hostcache = None
        
    def __read_hosts(self):
        try:
            _logger.debug("reading %s", self.__path)
            f = open(self.__path)
        except:
            _logger.debug("failed to open known hosts")
            return
        hosts = set()
        for line in f:
            hostip,rest = line.split(' ', 1)
            if hostip.find(',') > 0:
                host = hostip.split(',', 1)[0]
            else:
                host = hostip
            host = host.strip()
            hosts.add(host)
        f.close()
        return hosts
        
    def get_hosts(self):
        try:
            stbuf = os.stat(self.__path)
            ts_size =  (stbuf[stat.ST_MTIME], stbuf[stat.ST_SIZE])
        except OSError, e:
            ts_size = None
        if ts_size is not None and self.__hostcache_ts_size != ts_size:
            self.__hostcache = self.__read_hosts()
            self.__hostcache_ts_size = ts_size
        return self.__hostcache

_openssh_hosts_db = OpenSSHKnownHostsDB()

class SshAvahiMonitor(gobject.GObject):
    __gsignals__ = {
        "changed" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, []),
    }
    def __init__(self):
        super(SshAvahiMonitor, self).__init__()
        # maps name->(addr,port)
        self.__cache = {}
        
        if not avahi_available:
            return
        
        sysbus = dbus.SystemBus()
        avahi_service = sysbus.get_object(avahi.DBUS_NAME, avahi.DBUS_PATH_SERVER)
        self.__avahi = dbus.Interface(avahi_service, avahi.DBUS_INTERFACE_SERVER)

        browser_ref = self.__avahi.ServiceBrowserNew(avahi.IF_UNSPEC,
                                                     avahi.PROTO_UNSPEC,
                                                     '_ssh._tcp', 'local',
                                                     dbus.UInt32(0))
        browser = sysbus.get_object(avahi.DBUS_NAME, browser_ref)
        self.__browser = dbus.Interface(browser, avahi.DBUS_INTERFACE_SERVICE_BROWSER)
        self.__browser.connect_to_signal('ItemNew', self.__on_item_new)
        self.__browser.connect_to_signal('ItemRemove', self.__on_item_remove)

    def __iter__(self):
        for k,(addr,port) in self.__cache.iteritems():
            yield (k, addr, port)

    def __on_resolve_reply(self, *args):
        addr, port = args[-4:-2]
        name = args[2].decode('utf-8', 'replace')
        _logger.debug("add Avahi SSH: %r %r %r", name, addr, port)
        self.__cache[name] = (addr, port)
        self.emit('changed')
        
    def __on_avahi_error(self, *args):
        _logger.debug("Avahi error: %s", args)
        
    def __on_item_new(self, interface, protocol, name, type, domain, flags):
        self.__avahi.ResolveService(interface, protocol, name, type, domain,
                                    avahi.PROTO_UNSPEC, dbus.UInt32(0),
                                    reply_handler=self.__on_resolve_reply,
                                    error_handler=self.__on_avahi_error)

    def __on_item_remove(self, interface, protocol, name, type, domain,server):
        uname = name.decode('utf-8', 'replace')
        if uname in self.__cache:
            _logger.debug("del Avahi SSH: %r", uname)            
            del self.__cache[uname]
            self.emit('changed')
            
_local_avahi_instance = None
def get_local_avahi():
    global _local_avahi_instance
    if _local_avahi_instance is None:
        _local_avahi_instance = SshAvahiMonitor()
    return _local_avahi_instance            
            
class SshOptions(gtk.Expander):
    def __init__(self):
        super(SshOptions, self).__init__(_('Options'))
        self.set_label('<b>%s</b>' % (gobject.markup_escape_text(self.get_label())))
        self.set_use_markup(True)
        options_vbox = gtk.VBox()
        options_hbox = gtk.HBox()
        options_vbox.add(options_hbox)
        self.__entry = gtk.Entry()
        options_hbox.pack_start(self.__entry, expand=True)
        options_help = gtk.Button(stock=gtk.STOCK_HELP)
        options_help.connect('clicked', self.__on_options_help_clicked)
        options_hbox.pack_start(options_help, expand=False)
        options_helplabel = gtk.Label(_('Example: '))
        options_helplabel.set_markup('<i>%s -C -Y -oTCPKeepAlive=false</i>' % (gobject.markup_escape_text(options_helplabel.get_label()),))
        options_helplabel.set_alignment(0.0, 0.5)
        options_vbox.add(options_helplabel)
        self.add(options_vbox)
        
    def get_entry(self):
        return self.__entry   
    
    def __on_options_help_clicked(self, b):
        # Hooray Unix!
        subprocess.Popen(['gnome-terminal', '-x', 'man', 'ssh'])
        
class LocalConnectDialog(gtk.Dialog):
    def __init__(self, parent=None, local_avahi=None):    
        super(LocalConnectDialog, self).__init__(title=_("New Local Secure Shell Connection"),
                                            parent=parent,
                                            flags=gtk.DIALOG_DESTROY_WITH_PARENT,
                                            buttons=(gtk.STOCK_CANCEL, gtk.RESPONSE_CANCEL))
        
        self.__local_avahi = local_avahi
        self.__custom_user = False
        
        self.connect('response', lambda *args: self.hide())
        self.connect('delete-event', self.hide_on_delete)
        button = self.add_button(_('C_onnect'), gtk.RESPONSE_ACCEPT)
        button.set_property('image', gtk.image_new_from_stock('gtk-connect', gtk.ICON_SIZE_BUTTON))
        self.set_default_response(gtk.RESPONSE_ACCEPT)
                
        self.set_has_separator(False)
        self.set_border_width(5)
        self.__vbox = vbox = gtk.VBox()
        self.vbox.add(self.__vbox)   
        self.vbox.set_spacing(6)
        
        frame = gtk.Frame()
        #frame.set_label_widget(history_label)
        self.__model = gtk.ListStore(str, str, int)
        self.__view = gtk.TreeView(self.__model)
        self.__view.connect('row-activated', self.__on_item_activated)
        frame.add(self.__view)
        colidx = self.__view.insert_column_with_attributes(-1, _('Name'),
                                                          gtk.CellRendererText(),
                                                          text=0)
        colidx = self.__view.insert_column_with_attributes(-1, _('Address'),
                                                          gtk.CellRendererText(),
                                                          text=1)
        vbox.pack_start(frame, expand=True)
        self.__reload_avahi()
        
        self.__options_expander = SshOptions()
        self.__options_entry = self.__options_expander.get_entry()           
        
        vbox.pack_start(gtk.Label(' '), expand=False)
        hbox = gtk.HBox()
        vbox.pack_start(hbox, expand=False)
        user_label = gtk.Label(_('User: '))
        user_label.set_markup('<b>%s</b>' % (gobject.markup_escape_text(user_label.get_text())))
        hbox.pack_start(user_label, expand=False)
        self.__user_entry = gtk.Entry()
        self.__set_user(None)
        
        hbox.pack_start(self.__user_entry, expand=False) 
  
        vbox.pack_start(self.__options_expander, expand=False)        

        self.set_default_size(640, 480)
        
    def __reload_avahi(self):
        self.__model.clear()
        for name,host,port in self.__local_avahi:
            self.__model.append((name,host,port))
        
    def __set_user(self, name):
        if name is None:
            name = pwd.getpwuid(os.getuid()).pw_name
        self.__user_entry.set_text(name)    

    def __on_user_modified(self, *args):   
        self.__custom_user = True
        
    def __on_item_activated(self, tv, path, vc):
        self.activate_default()
            
    def run_get_cmd(self):
        self.show_all()        
        resp = self.run()
        if resp != gtk.RESPONSE_ACCEPT:
            return None
        (model, seliter) = self.__view.get_selection().get_selected()
        if seliter is None:
            return None
        host = model.get_value(seliter, 1)
        port = model.get_value(seliter, 2)
        if port != 22:
            args = ['-p', '%s' % (port,)]
        else:
            args = []
        args.extend([x for x in _whitespace_re.split(self.__options_entry.get_text()) if x != ''])
        if self.__custom_user:
            args.append(self.__user_entry.get_text() + '@' + host)
        else:
            args.append(host)
        return args                    

class ConnectDialog(gtk.Dialog):
    def __init__(self, parent=None, history=None):
        super(ConnectDialog, self).__init__(title=_("New Secure Shell Connection"),
                                            parent=parent,
                                            flags=gtk.DIALOG_DESTROY_WITH_PARENT,
                                            buttons=(gtk.STOCK_CANCEL, gtk.RESPONSE_CANCEL))
        
        self.__history = history

        self.connect('response', lambda *args: self.hide())
        self.connect('delete-event', self.hide_on_delete)
        button = self.add_button(_('C_onnect'), gtk.RESPONSE_ACCEPT)
        button.set_property('image', gtk.image_new_from_stock('gtk-connect', gtk.ICON_SIZE_BUTTON))
        self.set_default_response(gtk.RESPONSE_ACCEPT)
                
        self.set_has_separator(False)
        self.set_border_width(5)
        
        self.__vbox = vbox =gtk.VBox()
        self.vbox.add(self.__vbox)   
        self.vbox.set_spacing(6)

        self.__response_value = None
   
        self.__suppress_recent_search = False
        self.__idle_search_id = 0
        self.__idle_update_search_id = 0
        
        self.__custom_user = False
   
        self.__hosts = _openssh_hosts_db

        sg = gtk.SizeGroup(gtk.SIZE_GROUP_HORIZONTAL)
        
        hbox = gtk.HBox()
        vbox.pack_start(hbox, expand=False)
        host_label = gtk.Label(_('Host: '))
        host_label.set_markup('<b>%s</b>' % (gobject.markup_escape_text(host_label.get_text())))
        sg.add_widget(host_label)
        hbox.pack_start(host_label, expand=False)
        self.__entry = gtk.combo_box_entry_new_text()
        self.__entry.child.connect('activate', self.__on_entry_activated)
        self.__entry.child.connect('notify::text', self.__on_entry_modified)
        self.__entrycompletion = gtk.EntryCompletion()
        self.__entrycompletion.set_property('inline-completion', True)
        self.__entrycompletion.set_property('popup-completion', False)
        self.__entrycompletion.set_property('popup-single-match', False)
        self.__entrycompletion.set_model(self.__entry.get_property('model'))
        self.__entrycompletion.set_text_column(0)     
        self.__entry.child.set_completion(self.__entrycompletion)
        hbox.add(self.__entry)
        self.__reload_entry()

        hbox = gtk.HBox()
        vbox.pack_start(hbox, expand=False)
        user_label = gtk.Label(_('User: '))
        user_label.set_markup('<b>%s</b>' % (gobject.markup_escape_text(user_label.get_text())))
        sg.add_widget(user_label)
        hbox.pack_start(user_label, expand=False)
        self.__user_entry = gtk.Entry()
        self.__set_user(None)
        self.__user_entry.connect('notify::text', self.__on_user_modified)
        self.__user_entry.set_activates_default(True)
        self.__user_completion = gtk.EntryCompletion()
        self.__user_completion.set_property('inline-completion', True)
        self.__user_completion.set_property('popup-single-match', False)
        self.__user_completion.set_model(gtk.ListStore(gobject.TYPE_STRING))
        self.__user_completion.set_text_column(0)     
        self.__user_entry.set_completion(self.__user_completion)

        hbox.pack_start(self.__user_entry, expand=False)

        vbox.pack_start(gtk.Label(' '), expand=False)

        history_label = gtk.Label(_('Connection History'))
        history_label.set_markup('<b>%s</b>' % (gobject.markup_escape_text(history_label.get_text())))        
        frame = gtk.Frame()
        #frame.set_label_widget(history_label)
        self.__recent_model = gtk.ListStore(gobject.TYPE_PYOBJECT, gobject.TYPE_STRING, gobject.TYPE_PYOBJECT)
        self.__recent_view = gtk.TreeView(self.__recent_model)
        self.__recent_view.get_selection().connect('changed', self.__on_recent_selected)
        self.__recent_view.connect('row-activated', self.__on_recent_activated)
        frame.add(self.__recent_view)
        colidx = self.__recent_view.insert_column_with_data_func(-1, _('Connection'),
                                                          gtk.CellRendererText(),
                                                          self.__render_userhost)
        colidx = self.__recent_view.insert_column_with_data_func(-1, _('Time'),
                                                          gtk.CellRendererText(),
                                                          self.__render_time_recency, datetime.datetime.now())
        vbox.pack_start(frame, expand=True)
        self.__on_entry_modified()
        self.__reload_connection_history()
        
        self.__options_expander = SshOptions()
        self.__options_entry = self.__options_expander.get_entry()

        vbox.pack_start(self.__options_expander, expand=False)        

        self.set_default_size(640, 480)
        
    def __on_browse_local(self, b):
        pass

    def __render_userhost(self, col, cell, model, iter):
        user = model.get_value(iter, 0)
        host = model.get_value(iter, 1)
        if user:
            userhost = user + '@' + host
        else:
            userhost = host
        cell.set_property('text', userhost)
        
    def __set_user(self, name):
        if name is None:
            name = pwd.getpwuid(os.getuid()).pw_name
        self.__user_entry.set_text(name) 
        
    def __render_time_recency(self, col, cell, model, iter, curtime):
        val = model.get_value(iter, 2)
        deltastr = timesince(val, curtime)
        cell.set_property('text', deltastr)
       
    def __reload_entry(self, *args, **kwargs):
        _logger.debug("reloading")
        # TODO do predictive completion here
        # For example, I have in my history:
        # foo.cis.ohio-state.edu
        # bar.cis.ohio-state.edu
        # Now I type baz.cis
        # The system notices that nothing matches baz.cis; however
        # the "cis" part does match foo.cis.ohio-state.edu, so 
        # we start offering a completion for it
        self.__entry.get_property('model').clear()
        for host in self.__hosts.get_hosts():
            self.__entry.append_text(host)
            
    def __on_user_modified(self, *args):
        if self.__suppress_recent_search:
            return        
        self.__custom_user = True

    def __on_entry_modified(self, *args):
        text = self.__entry.get_active_text()
        havetext = text != ''
        self.set_response_sensitive(gtk.RESPONSE_ACCEPT, havetext)
        if self.__suppress_recent_search:
            return        
        if havetext and self.__idle_update_search_id == 0:
            self.__idle_update_search_id = gobject.timeout_add(250, self.__idle_update_search)

    def __force_idle_search(self):
        if self.__idle_update_search_id > 0:
            gobject.source_remove(self.__idle_update_search_id)
        self.__idle_update_search()

    def __idle_update_search(self):
        self.__idle_update_search_id = 0
        host = self.__entry.get_active_text()
        usernames = list(self.__history.get_users_for_host_search(host))
        if len(usernames) > 0:
            last_user = usernames[0]
            if last_user:
                self.__user_entry.set_text(usernames[0])
            model = gtk.ListStore(gobject.TYPE_STRING)
            for uname in usernames:
                if uname:
                    model.append((uname,))
            self.__user_completion.set_model(model)
        self.__reload_connection_history()            

    def __reload_connection_history(self):
        hosttext = self.__entry.get_active_text()
        self.__recent_model.clear()
        for user,host,options,ts in self.__history.get_recent_connections_search(hosttext):
            self.__recent_model.append((user, host, ts))

    def __on_entry_activated(self, *args):
        self.__force_idle_search()
        hosttext = self.__entry.get_active_text()
        if hosttext.find('@') >= 0:
            (user, host) = hosttext.split('@', 1)
            self.__user_entry.set_text(user)
            self.__entry.child.set_text(host)
            self.__user_entry.activate()
        else:
            self.__user_entry.select_region(0, -1)
            self.__user_entry.grab_focus()
            
    def __on_recent_selected(self, ts):
        (tm, seliter) = ts.get_selected()
        if seliter is None: 
            return
        user = self.__recent_model.get_value(seliter, 0)
        host = self.__recent_model.get_value(seliter, 1)
        self.__suppress_recent_search = True
        self.__entry.child.set_text(host)
        if user:
            self.__user_entry.set_text(user)
        else:
            self.__set_user(None)
        self.__suppress_recent_search = False        
            
    def __on_recent_activated(self, tv, path, vc):
        self.activate_default()
            
    def run_get_cmd(self):
        self.show_all()        
        resp = self.run()
        if resp != gtk.RESPONSE_ACCEPT:
            return None
        host = self.__entry.get_active_text()
        if not host:
            return None
        args = [x for x in _whitespace_re.split(self.__options_entry.get_text()) if x != '']
        if self.__custom_user:
            args.append(self.__user_entry.get_text() + '@' + host)
        else:
            args.append(host)
        return args

_CONTROLPATH = None
def get_controlpath(create=True):
    global _CONTROLPATH
    if _CONTROLPATH is None and create:
        _CONTROLPATH = tempfile.mkdtemp('', 'hotssh')
    return _CONTROLPATH

def get_base_sshcmd():
    return ['ssh']

def get_connection_sharing_args():
    # TODO - openssh should really do this out of the box    
    return ['-oControlMaster=auto', '-oControlPath=' + os.path.join(get_controlpath(), 'master-%r@%h:%p')]

class HostConnectionMonitor(gobject.GObject):
    __gsignals__ = {
        "host-status" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, (gobject.TYPE_PYOBJECT,gobject.TYPE_BOOLEAN,gobject.TYPE_PYOBJECT)),
    }      
    def __init__(self):
        super(HostConnectionMonitor, self).__init__()
        self.__host_monitor_ids = {}
        self.__check_statuses = {}
    
    def start_monitor(self, host):
        if not (host in self.__host_monitor_ids or host in self.__check_statuses):
            _logger.debug("adding monitor for %s", host)            
            self.__host_monitor_ids[host] = gobject.timeout_add(3000, self.__check_host, host)
            
    def stop_monitor(self, host):
        _logger.debug("stopping monitor for %s", host)
        if host in self.__host_monitor_ids:
            monid = self.__host_monitor_ids[host]
            gobject.source_remove(monid)
            del self.__host_monitor_ids[host]
        if host in self.__check_statuses:
            del self.__check_statuses[host]
        
    def get_monitors(self):
        return self.__host_monitor_ids
            
    def __check_host(self, host):
        _logger.debug("performing check for %s", host)
        del self.__host_monitor_ids[host]
        cmd = list(get_base_sshcmd())
        starttime = time.time()
        # This is a hack.  Blame Adam Jackson.
        cmd.extend(['-oBatchMode=true', host, '/bin/true'])
        subproc = subprocess.Popen(cmd)
        child_watch_id = gobject.child_watch_add(subproc.pid, self.__on_check_exited, host)
        timeout_id = gobject.timeout_add(7000, self.__check_timeout, host)
        self.__check_statuses[host] = (starttime, subproc.pid, timeout_id, child_watch_id)
        return False
        
    def __check_timeout(self, host):
        _logger.debug("timeout for host=%s", host)
        try:
            (starttime, pid, timeout_id, child_watch_id) = self.__check_statuses[host]
        except KeyError, e:
            return False
        try:
            os.kill(pid, signal.SIGHUP)
        except OSError, e:
            _logger.debug("failed to signal pid %s", pid, exc_info=True)
            pass
        return False    
        
    def __on_check_exited(self, pid, condition, host):
        _logger.debug("check exited, pid=%s condition=%s host=%s", pid, condition, host)
        try:
            (starttime, pid, timeout_id, child_watch_id) = self.__check_statuses[host]
        except KeyError, e:
            return False
        gobject.source_remove(timeout_id)
        del self.__check_statuses[host]    
        self.__host_monitor_ids[host] = gobject.timeout_add(4000, self.__check_host, host)              
        self.emit('host-status', host, condition == 0, time.time()-starttime)
        return False
        
_hostmonitor = HostConnectionMonitor()

class SshTerminalWidgetImpl(VteTerminalWidget):
    def __init__(self, *args, **kwargs):
        self.__actions = kwargs['actions']
        super(SshTerminalWidgetImpl, self).__init__(*args, **kwargs)

    def _get_extra_context_menuitems(self):
        return [self.__actions.get_action(x).create_menu_item() for x in ['CopyConnection', 'OpenSFTP']]

class SshTerminalWidget(gtk.VBox):
    __gsignals__ = {
        "close" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, []),
    }       
    def __init__(self, args, cwd, actions=None):
        super(SshTerminalWidget, self).__init__()
        self.__init_state()
        self.__args = args        
        self.__sshcmd = list(get_base_sshcmd())
        self.__sshcmd.extend(args)
        self.__cwd = cwd
        self.__host = None
        self.__sshopts = []
        self.__actions = actions
        enable_connection_sharing = True
        for arg in args:
            if not arg.startswith('-'):
                if self.__host is None:                 
                    self.__host = arg
            else:
                if arg == '-oControlPath=none':
                    enable_connection_sharing = False
                self.__sshopts.append(arg)
        if enable_connection_sharing:
            self.__sshcmd.extend(get_connection_sharing_args())
        
        header = gtk.VBox()
        self.__msg = gtk.Label()
        self.__msg.set_alignment(0.0, 0.5)
        self.__msgarea_mgr = MsgAreaController()
        header.pack_start(self.__msg)
        header.pack_start(self.__msgarea_mgr)
        self.pack_start(header, expand=False)
        self.ssh_connect()
        
    def __init_state(self):
        self.__connecting_state = False
        self.__connected = None
        self.__cmd_exited = False
        self.__latency = None        
        
    def set_status(self, connected, latency):
        if not connected and self.__connecting_state:
            return
        self.__connecting_state = False
        connected_changed = self.__connected != connected
        latency_changed = (not self.__latency) or (self.__latency*0.9 > latency) or (self.__latency*1.1 < latency)
        if not (connected_changed or latency_changed):
            return        
        self.__connected = connected
        self.__latency = latency
        self.__sync_msg()
        
    def __sync_msg(self):
        if self.__cmd_exited:
            return
        if self.__connecting_state:
            text = _('Connecting')
        elif self.__connected is True:
            text = _('Connected (%.2fs latency)') % (self.__latency)
        elif self.__connected is False:
            text = '<span foreground="red">%s</span>' % (_('Connection timeout'))
        elif self.__connected is None:
            text = _('Checking connection')
        if len(self.__sshopts) > 1:
            text += _('; Options: ') + (' '.join(map(gobject.markup_escape_text, self.__sshopts)))
        self.__msg.show()
        self.__msg.set_markup(text)
        
    def ssh_connect(self):
        self.__connecting_state = True        
        self.__term = term = SshTerminalWidgetImpl(cwd=self.__cwd, cmd=self.__sshcmd, actions=self.__actions)
        term.connect('child-exited', self.__on_child_exited)
        term.show_all()
        self.pack_start(term, expand=True)
        # For some reason, VTE doesn't have the CAN_FOCUS flag set, so we can't call
        # grab_focus.  Do it manually then:
        term.emit('focus', True)
        self.__msgarea_mgr.clear()
        self.__sync_msg()
        
    def ssh_reconnect(self):
        # TODO - do this in a better way
        if not self.__term.exited:
            os.kill(self.__term.pid, signal.SIGTERM)
        self.remove(self.__term)
        self.__term.destroy()
        self.__init_state()
        self.ssh_connect()
        
    def __on_child_exited(self, term):
        _logger.debug("disconnected")
        self.__cmd_exited = True
        self.__msg.hide()
        msgarea = self.__msgarea_mgr.new_from_text_and_icon(gtk.STOCK_INFO, _('Connection closed'), 
                                                            buttons=[(gtk.STOCK_CLOSE, gtk.RESPONSE_CLOSE)])
        reconnect = self.__actions.get_action('Reconnect')
        msgarea.add_stock_button_with_text(reconnect.get_property('label'), 
                                           reconnect.get_property('stock-id'), gtk.RESPONSE_ACCEPT)
        msgarea.connect('response', self.__on_msgarea_response)
        msgarea.show_all()
        
    def __on_msgarea_response(self, msgarea, respid):
        if respid == gtk.RESPONSE_ACCEPT:
            self.ssh_reconnect()
        else:
            self.emit('close')

    def has_close(self):
        return True
        
    def get_exited(self):
        return self.__cmd_exited        
        
    def get_term(self):
        return self.__term
        
    def get_vte(self):
        return self.__term.get_vte()
        
    def get_title(self):
        return self.get_host()
    
    def get_host(self):
        return self.__host
    
    def get_options(self):
        return self.__sshopts

class SshWindow(VteWindow):
    def __init__(self, **kwargs):
        super(SshWindow, self).__init__(title='HotSSH', icon_name='hotwire-openssh', **kwargs)
        
        self.__ui_string = """
<ui>
  <menubar name='Menubar'>
    <menu action='FileMenu'>
      <placeholder name='FileAdditions'>
        <menuitem action='NewConnection'/>
        <menuitem action='NewLocalConnection'/>        
        <menuitem action='CopyConnection'/>    
        <menuitem action='OpenSFTP'/>
        <separator/>
        <menuitem action='Reconnect'/>
        <separator/>
      </placeholder>
    </menu>
  </menubar>
</ui>
"""       

        self._get_notebook().connect('switch-page', self.__on_page_switch)

        try:
            self.__nm_proxy = dbus.SystemBus().get_object('org.freedesktop.NetworkManager', '/org/freedesktop/NetworkManager')
            self.__nm_proxy.connect_to_signal('StateChange', self.__on_nm_state_change)
        except dbus.DBusException, e:
            _logger.debug("Couldn't find NetworkManager")
            self.__nm_proxy = None
        
        self.__idle_stop_monitoring_id = 0
        
        self.connect("notify::is-active", self.__on_is_active_changed)
        _hostmonitor.connect('host-status', self.__on_host_status)

        self.__connhistory = get_history()
        self.__local_avahi = get_local_avahi()
        
        self.__merge_ssh_ui()

    def __add_to_history(self, args):
        user = None
        host = None
        options = []
        for arg in args:
            if arg.startswith('-'): 
                options.extend(arg)
                continue
            if arg.find('@') >= 0:
                (user, host) = arg.split('@', 1)
            else:
                host = arg
        self.__connhistory.add_connection(user, host, options)

    def new_tab(self, args, cwd):
        if len(args) == 0:
            self.open_connection_dialog(exit_on_cancel=True)
        else:
            self.__add_to_history(args)
            term = SshTerminalWidget(args=args, cwd=cwd, actions=self.__action_group)
            self.append_widget(term)
        
    def __on_nm_state_change(self, *args):
        self.__sync_nm_state()
        
    def __sync_nm_state(self):
        self.__nm_proxy.GetActiveConnections(reply_handler=self.__on_nm_connections, error_handler=self.__on_dbus_error)
        
    def __on_dbus_error(self, *args):
        _logger.debug("caught DBus error: %r", args, exc_info=True)
        
    @log_except(_logger)        
    def __on_nm_connections(self, connections):
        _logger.debug("nm connections: %s", connections)    
        
    @log_except(_logger)        
    def __on_host_status(self, hostmon, host, connected, latency):
        _logger.debug("got host status host=%s conn=%s latency=%s", host, connected, latency)
        for widget in self._get_notebook().get_children():
            child_host = widget.get_host()
            if child_host != host:
                continue
            widget.set_status(connected, latency)
            
    @log_except(_logger)            
    def __on_is_active_changed(self, *args):
        isactive = self.get_property('is-active')
        if isactive:
            self.__start_monitoring()
            if self.__idle_stop_monitoring_id > 0:
                gobject.source_remove(self.__idle_stop_monitoring_id)
                self.__idle_stop_monitoring_id = 0
        elif self.__idle_stop_monitoring_id == 0:
            self.__idle_stop_monitoring_id = gobject.timeout_add(8000, self.__idle_stop_monitoring)
            
    def __idle_stop_monitoring(self):
        self.__idle_stop_monitoring_id = 0
        self.__stop_monitoring()
        
    @log_except(_logger)
    def __on_page_switch(self, n, p, pn):
        # Becuase of the way get_current_page() works in this signal handler, this
        # will actually disable monitoring for the previous tab, and enable it
        # for the new current one.
        self.__stop_monitoring()
        self.__start_monitoring(pn=pn)
            
    def __stop_monitoring(self):
        notebook = self._get_notebook()
        pn = notebook.get_current_page()
        if pn >= 0:
            prev_widget = notebook.get_nth_page(pn)
            prev_host = prev_widget.get_host()
            _hostmonitor.stop_monitor(prev_host)
            prev_widget.set_status(None, None)        
            
    def __start_monitoring(self, pn=None):
        notebook = self._get_notebook()
        if pn is not None:
            pagenum = pn
        else:
            pagenum = notebook.get_current_page()
        widget = notebook.get_nth_page(pagenum)
        _hostmonitor.start_monitor(widget.get_host())
        
    def __merge_ssh_ui(self):
        self.__using_accels = True
        self.__actions = actions = [
            ('NewConnection', gtk.STOCK_NEW, _('Connect to server'), '<control><shift>O',
             _('Open a new Secure Shell connection'), self.__new_connection_cb),
            ('NewLocalConnection', None, _('Connect to local server'), None,
             _('Open a new Secure Shell connection to local server'), self.__new_local_connection_cb),                
            ('CopyConnection', gtk.STOCK_JUMP_TO, _('New tab for connection'), '<control><shift>T',
             _('Open a new tab for the same remote computer'), self.__copy_connection_cb),              
            ('OpenSFTP', gtk.STOCK_OPEN, _('Open SFTP'), '<control><shift>S',
             _('Open a SFTP connection'), self.__open_sftp_cb),            
            ('ConnectionMenu', None, _('Connection')),
            ('Reconnect', gtk.STOCK_CONNECT, _('_Reconnect'), '<control><shift>R', _('Reset connection to server'), self.__reconnect_cb),
            ]
        self.__action_group = self._merge_ui(self.__actions, self.__ui_string)
        
    @log_except(_logger)        
    def __copy_connection_cb(self, action):
        notebook = self._get_notebook()
        widget = notebook.get_nth_page(notebook.get_current_page())
        host = widget.get_host()
        opts = widget.get_options()
        args = list(opts)
        args.append(host)
        self.new_tab(args, None)

    def open_connection_dialog(self, exit_on_cancel=False):
        win = ConnectDialog(parent=self, history=self.__connhistory)
        sshargs = win.run_get_cmd()
        if not sshargs:
            # We get here when called with no arguments, and we're the main instance.
            if exit_on_cancel:
                sys.exit(0)
            return
        self.new_tab(sshargs, None)

    @log_except(_logger)        
    def __new_connection_cb(self, action):
        self.open_connection_dialog()
        
    @log_except(_logger)        
    def __new_local_connection_cb(self, action):
        win = LocalConnectDialog(parent=self, local_avahi=self.__local_avahi)
        sshargs = win.run_get_cmd()
        if not sshargs:
            return
        self.new_tab(sshargs, None)  
        
    @log_except(_logger)        
    def __open_sftp_cb(self, action):
        notebook = self._get_notebook()        
        widget = notebook.get_nth_page(notebook.get_current_page())
        host = widget.get_host()
        subprocess.Popen(['nautilus', 'sftp://%s' % (host,)])
        
    @log_except(_logger)        
    def __reconnect_cb(self, a):
        notebook = self._get_notebook()        
        widget = notebook.get_nth_page(notebook.get_current_page())
        widget.ssh_reconnect()

class SshApp(VteApp):
    def __init__(self):
        super(SshApp, self).__init__(SshWindow)
        self.__sessionpath = os.path.expanduser('~/.hotwire/hotwire-ssh.session')
        self.__connhistory = get_history()
        self.__local_avahi = SshAvahiMonitor() 
        
    @staticmethod
    def get_name():
        return 'HotSSH'         
                
    def on_shutdown(self, factory):
        super(SshApp, self).on_shutdown(factory)
        cp = get_controlpath(create=False)
        if cp is not None:
            try:
                _logger.debug("removing %s", cp)
                shutil.rmtree(cp)
            except:
                pass
            
    def offer_load_session(self):
        savedsession = self._parse_saved_session()
        allhosts = set()        
        if savedsession:
            for window in savedsession:
                for connection in window:
                    allhosts.add(connection['userhost'])
        if len(allhosts) > 0:
            dlg = gtk.MessageDialog(parent=None, flags=0, type=gtk.MESSAGE_QUESTION, 
                                    buttons=gtk.BUTTONS_CANCEL,
                                    message_format=_("Restore saved session?"))
            button = dlg.add_button(_('_Reconnect'), gtk.RESPONSE_ACCEPT)
            button.set_property('image', gtk.image_new_from_stock('gtk-connect', gtk.ICON_SIZE_BUTTON))
            dlg.set_default_response(gtk.RESPONSE_ACCEPT)
            dlg.format_secondary_markup(_('Reconnect to %d hosts') % (len(allhosts),))
            
            #ls = gtk.ListStore(str)
            #gv = gtk.TreeView(ls)
            #colidx = gv.insert_column_with_attributes(-1, _('Connection'),
            #                                          gtk.CellRendererText(),
            #                                          text=0)
            #for host in allhosts:
            #    ls.append((host,))
            #dlg.add(gv)            
                     
            resp = dlg.run()
            dlg.destroy()
            if resp == gtk.RESPONSE_ACCEPT:
                self._load_session(savedsession)
                return
        w = self.get_factory().create_initial_window()
        w.new_tab([], os.getcwd())
        w.show_all()
        w.present()
        
    def _load_session(self, session):
        factory = self.get_factory()        
        for window in session:
            window_impl = factory.create_window()
            for connection in window:
                args = [connection['userhost']]
                if 'options' in connection:
                    args.extend(connection['options'])
                widget = window_impl.new_tab(args, os.getcwd())
            window_impl.show_all()
                
    #override
    @log_except(_logger)
    def _parse_saved_session(self):
        factory = self.get_factory()
        try:
            f = open(self.__sessionpath)
        except:
            return None
        doc = xml.dom.minidom.parse(f)
        saved_windows = []
        current_widget = None
        for window_child in doc.documentElement.childNodes:
            if not (window_child.nodeType == window_child.ELEMENT_NODE and window_child.nodeName == 'window'): 
                continue
            connections = []
            for child in window_child.childNodes:
                if not (child.nodeType == child.ELEMENT_NODE and child.nodeName == 'connection'): 
                    continue
                host = child.getAttribute('host')
                iscurrent = child.getAttribute('current')
                options = []
                for options_elt in child.childNodes:
                    if not (options_elt.nodeType == child.ELEMENT_NODE and options_elt.nodeName == 'options'): 
                        continue
                    for option_elt in child.childNodes:
                        if not (option_elt.nodeType == child.ELEMENT_NODE and options_elt.nodeName == 'option'): 
                            continue
                        options.append(option.firstChild.nodeValue)
                kwargs = {'userhost': host}
                if len(options) > 0:
                    kwargs['options'] = options
                if iscurrent:
                    kwargs['current'] = True
                connections.append(kwargs)
            saved_windows.append(connections)
        return saved_windows
        
    #override
    @log_except(_logger)    
    def save_session(self):
        _logger.debug("doing session save")
        tempf_path = tempfile.mktemp('.session.tmp', 'hotwire-session', os.path.dirname(self.__sessionpath))
        f = open(tempf_path, 'w')
        state = []
        doc = xml.dom.minidom.getDOMImplementation().createDocument(None, "session", None)
        root = doc.documentElement
        factory = self.get_factory()
        for window in factory._get_windows():
            notebook = window._get_notebook()
            current = notebook.get_nth_page(notebook.get_current_page())
            window_node = doc.createElement('window')
            root.appendChild(window_node)            
            for widget in notebook:
                connection = doc.createElement('connection')
                window_node.appendChild(connection)
                connection.setAttribute('host', widget.get_host())
                if current == widget:
                    connection.setAttribute('current', 'true')
                options = widget.get_options()
                if options:
                    options_elt = doc.createElement('options')
                    connection.appendChild(options_elt)
                    for option in options:
                        opt = doc.createElement('option')
                        options_elt.appendChild(opt)                    
                        opt.appendChild(doc.createTextNode(option))
        f.write(doc.toprettyxml())
        f.close()
        os.rename(tempf_path, self.__sessionpath)            
