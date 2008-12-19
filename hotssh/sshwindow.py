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
import datetime,gettext,sha,commands,errno
import urllib
from StringIO import StringIO
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

def glibc_backtrace():
    from ctypes import cdll
    libc = cdll.LoadLibrary("libc.so.6")
    btcount = 100
    btdata = (c_void_p * btcount)()
    libc.backtrace(btdata, c_int(btcount))
    symbols = libc.backtrace_symbols(btdata, c_int(btcount))
    print symbols

def userhost_pair_to_string(user, host, port=None):
    host = hostport_pair_to_string(host, port)
    if user:
        return user + '@' + host
    return host

def hostport_pair_to_string(host, port=None):
    hostport = host
    if port is not None:
        hostport += (':%s' % (port,))
    return hostport

class HotSshAboutDialog(gtk.AboutDialog):
    def __init__(self):
        super(HotSshAboutDialog, self).__init__()
        dialog = self
        import hotssh.version
        dialog.set_property('website', 'http://www.gnome.org/projects/hotssh')
        dialog.set_property('version', hotssh.version.__version__)
        dialog.set_property('authors', ['Colin Walters <walters@verbum.org>'])
        dialog.set_property('copyright', u'Copyright \u00A9 2007,2008 Colin Walters <walters@verbum.org>')
        dialog.set_property('logo-icon-name', 'hotwire-openssh')
        dialog.set_property('license',
                            '''Hotwire is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.\n
Hotwire is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.\n
You should have received a copy of the GNU General Public License
along with Hotwire; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA''')
        dialog.set_property('name', "About Secure Shell")
        comments = _("An interface to OpenSSH, a Secure Shell client\n\n")
        if hotssh.version.svn_version_info:
            comments += "changeset: %s\ndate: %s\n" % (hotssh.version.svn_version_info['Revision'], hotssh.version.svn_version_info['Last Changed Date'],)
        dialog.set_property('comments', comments)

class SshConnectionHistory(object):
    def __init__(self):
        # We want to upgrade from the old Hotwire-derived location into the new ~/.hotssh
        self.__oldstatedir = os.path.expanduser('~/.hotwire/state')
        self.__statedir = os.path.expanduser('~/.hotssh')
        try:
            os.makedirs(self.__statedir)
        except:
            pass
        self.__path = path = os.path.join(self.__statedir, 'history.sqlite')
        oldpath = os.path.join(self.__oldstatedir, 'ssh.sqlite')
        if os.path.exists(oldpath) and not os.path.exists(self.__path):
            shutil.copy2(oldpath, path)
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
            if user and host:
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

    def add_connection(self, user, host, options):
        if host is None or host == '':
            return
        cursor = self.__conn.cursor()
        cursor.execute('''BEGIN TRANSACTION''')
        cursor.execute('''INSERT INTO Connections VALUES (NULL, ?, ?, ?, ?)''', (host, user, ' '.join(options), datetime.datetime.now()))
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

class OpenSSHKnownHostsDB(gobject.GObject):
    __gsignals__ = {
        "changed" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, []),
    }
    def __init__(self):
        super(OpenSSHKnownHostsDB, self).__init__()
        self.__path = os.path.expanduser('~/.ssh/known_hosts')
        self.__hostcache_ts_size = (None, None)
        self.__hostcache = None
        self.__faviconcache = os.path.expanduser('~/.hotssh/favicons')
        self.__pixbufcache = {}

    def __get_favicon_cache(self):
        try:
            os.makedirs(self.__faviconcache)
        except OSError, e:
            if e.errno == errno.EEXIST:
                pass
            else:
                raise
        return self.__faviconcache

    def __read_hosts(self):
        try:
            _logger.debug("reading %s", self.__path)
            f = open(self.__path)
        except:
            _logger.debug("failed to open known hosts")
            return
        hosts = {}
        for line in f:
            if not line or line.startswith('|'):
                continue
            hostip,rest = line.split(' ', 1)
            if hostip.find(',') > 0:
                host = hostip.split(',', 1)[0]
            else:
                host = hostip
            hostkey_type,hostkey = rest.split(' ', 1)
            hostkey_type = hostkey_type.strip()
            hostkey = hostkey.strip()
            host = host.strip()
            hosts[host] = (hostkey_type, hostkey)
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
        else:
            return []
        return self.__hostcache.iterkeys()

    def __force_host_read(self):
        self.get_hosts()

    def get_favicon_for_host(self, host, port=None):
        cache = self.__get_favicon_cache()
        if cache is None:
            return None
        path = self.__get_favicon_path(host, port)
        try:
            stbuf = os.stat(path)
        except:
            return None
        return (path, stbuf[stat.ST_MTIME])

    def render_cached_favicon(self, path):
        bn = os.path.basename(path)
        if bn not in self.__pixbufcache:
            self.__pixbufcache[bn] = gtk.gdk.pixbuf_new_from_file_at_size(path, 16, 16)
        return self.__pixbufcache[bn]

    def __get_favicon_path(self, hostname, port):
        key = urllib.quote_plus(hostname)
        return os.path.join(self.__get_favicon_cache(), key + '.png')

    def save_favicon(self, host, port, favicon_tmppath):
        cache = self.__get_favicon_cache()
        if cache is None:
            return None
        path = self.__get_favicon_path(host, port)
        shutil.move(favicon_tmppath, path)
        self.emit('changed')
        return path

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

        try:
            sysbus = dbus.SystemBus()
            avahi_service = sysbus.get_object(avahi.DBUS_NAME, avahi.DBUS_PATH_SERVER)
            self.__avahi = dbus.Interface(avahi_service, avahi.DBUS_INTERFACE_SERVER)
        except dbus.exceptions.DBusException:
            return

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

class ConnectDialog(gtk.Dialog):
    def __init__(self, parent=None, history=None, local_avahi=None):
        super(ConnectDialog, self).__init__(title=_("New Secure Shell Connection"),
                                            parent=parent,
                                            flags=gtk.DIALOG_DESTROY_WITH_PARENT,
                                            buttons=(gtk.STOCK_CANCEL, gtk.RESPONSE_CANCEL))

        self.__history = history
        self.__local_avahi = local_avahi
        self.__local_avahi.connect('changed', self.__on_local_avahi_changed)

        self.__default_username = pwd.getpwuid(os.getuid()).pw_name

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

        self.__response_value = None

        self.__viewmode = 'history'

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

        self.__conntype_notebook = gtk.Notebook()
        self.__conntype_notebook.set_scrollable(True)
        self.__conntype_notebook.connect('switch-page', self.__on_switch_page)
        vbox.pack_start(self.__conntype_notebook, expand=True)

        history_label = gtk.Label(_('History'))
        tab = gtk.ScrolledWindow()
        tab.set_policy(gtk.POLICY_AUTOMATIC, gtk.POLICY_ALWAYS)
        #frame.set_label_widget(history_label)
        self.__recent_model = gtk.ListStore(object, str,  object)
        self.__recent_view = gtk.TreeView(self.__recent_model)
        self.__recent_view.get_selection().connect('changed', self.__on_recent_selected)
        self.__recent_view.connect('row-activated', self.__on_recent_activated)
        tab.add(self.__recent_view)
        colidx = self.__recent_view.insert_column_with_data_func(-1, '',
                                                          gtk.CellRendererPixbuf(),
                                                          self.__render_favicon)
        colidx = self.__recent_view.insert_column_with_data_func(-1, _('Connection'),
                                                          gtk.CellRendererText(),
                                                          self.__render_userhost)
        colidx = self.__recent_view.insert_column_with_data_func(-1, _('Time'),
                                                          gtk.CellRendererText(),
                                                          self.__render_time_recency, datetime.datetime.now())

        self.__conntype_notebook.append_page(tab, history_label)

        local_label = gtk.Label(_('Local'))
        tab = gtk.ScrolledWindow()
        tab.set_policy(gtk.POLICY_AUTOMATIC, gtk.POLICY_ALWAYS)
        self.__local_model = gtk.ListStore(str, str, str)
        self.__local_view = gtk.TreeView(self.__local_model)
        self.__local_view.get_selection().connect('changed', self.__on_local_selected)
        self.__local_view.connect('row-activated', self.__on_local_activated)
        tab.add(self.__local_view)
        colidx = self.__local_view.insert_column_with_attributes(-1, _('Name'),
                                                          gtk.CellRendererText(),
                                                          text=0)
        colidx = self.__local_view.insert_column_with_attributes(-1, _('Address'),
                                                          gtk.CellRendererText(),
                                                          text=1)
        self.__conntype_notebook.append_page(tab, local_label)

        self.__on_entry_modified()
        self.__force_idle_search()

        self.__options_expander = SshOptions()
        self.__options_entry = self.__options_expander.get_entry()

        vbox.pack_start(self.__options_expander, expand=False)

        self.set_default_size(640, 480)

    @log_except(_logger)
    def __on_switch_page(self, nb, p, pn):
        _logger.debug("got page switch, pn=%d", pn)
        self.__suppress_recent_search = True
        widget = self.__conntype_notebook.get_nth_page(pn)
        self.__viewmode = (pn == 0 and 'history' or 'local')
        # If we're switching, particularly from history->local it's
        # unlikely we want to keep the active search; the assumption here
        # is that the history search failed.
        self.__entry.child.set_text('')
        self.__force_idle_search()
        self.__suppress_recent_search = False

    def __on_browse_local(self, b):
        pass

    def __on_local_avahi_changed(self, *args):
        if self.__viewmode == 'local':
            self.__force_idle_search()

    def __render_userhost(self, col, cell, model, iter):
        user = model.get_value(iter, 0)
        host = model.get_value(iter, 1)
        if user and host:
            userhost = user + '@' + host
        else:
            userhost = host
        cell.set_property('text', userhost)

    def __set_user(self, name):
        if name is None:
            name = self.__default_username
        self.__user_entry.set_text(name)

    def __render_favicon(self, col, cell, model, it):
        user = model.get_value(it, 0)
        host = model.get_value(it, 1)
        favicondata = _openssh_hosts_db.get_favicon_for_host(host, None)
        if favicondata is not None:
            (favicon_path,mtime) = favicondata
            pixbuf = _openssh_hosts_db.render_cached_favicon(favicon_path)
            cell.set_property('pixbuf',pixbuf)
        else:
            cell.set_property('pixbuf', None)

    def __render_time_recency(self, col, cell, model, it, curtime):
        val = model.get_value(it, 2)
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
        text = self.__user_entry.get_text()
        self.__custom_user = text != self.__default_username

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
        self.__suppress_recent_search = True
        if self.__viewmode == 'history':
            self.__idle_update_search_history()
        else:
            self.__idle_update_search_local()
        self.__suppress_recent_search = False

    def __idle_update_search_local(self):
        self.__local_view.get_selection().unselect_all()
        hosttext = self.__entry.get_active_text()
        self.__local_model.clear()
        for name,host,port in self.__local_avahi:
            if host.find(hosttext) >= 0:
                self.__local_model.append((name,host,port))

    def __idle_update_search_history(self):
        self.__recent_view.get_selection().unselect_all()
        host = self.__entry.get_active_text()
        if host:
            usernames = list(self.__history.get_users_for_host_search(host))
        else:
            usernames = []
        _logger.debug("doing history update, usernames:%r", usernames)
        if len(usernames) > 0:
            last_user = usernames[0]
            if last_user is None:
                last_user = self.__default_username
            self.__user_entry.set_text(last_user)
            model = gtk.ListStore(gobject.TYPE_STRING)
            for uname in usernames:
                if uname:
                    model.append((uname,))
            self.__user_completion.set_model(model)
        else:
            self.__user_entry.set_text(self.__default_username)
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

    def __on_local_selected(self, ts):
        if self.__suppress_recent_search:
            return
        _logger.debug("local selected: %s", ts)
        (tm, seliter) = ts.get_selected()
        if seliter is None:
            return
        host = self.__local_model.get_value(seliter, 1)
        port = self.__local_model.get_value(seliter, 2)
        self.__suppress_recent_search = True
        self.__entry.child.set_text(host)
        self.__suppress_recent_search = False

    def __on_local_activated(self, tv, path, vc):
        self.activate_default()

    def __on_recent_selected(self, ts):
        if self.__suppress_recent_search:
            return
        _logger.debug("recent selected: %s", ts)
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

    def __reset(self):
        self.__entry.child.set_text('')
        self.__user_entry.set_text(self.__default_username)

    def run_get_cmd(self):
        self.__reset()
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

class AsyncCommandWithOutput(gobject.GObject):
    __gsignals__ = {
        "timeout" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, [gobject.TYPE_STRING]),

        "complete" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, (bool,
                                                                   int,
                                                                   gobject.TYPE_STRING))
    }

    user = property(lambda self: self.__user)
    host = property(lambda self: self.__host)
    port = property(lambda self: self.__port)
    output = property(lambda self: self.__output.get_value())

    def __init__(self, user, host, port, cmdstart, cmdend, timeout=7000, hostappend='', getoutput=True):
        super(AsyncCommandWithOutput, self).__init__()
        self.__user = user
        self.__host = host
        self.__port = port
        cmd = list(cmdstart)
        cmd.extend(get_connection_sharing_args())
        if port:
            cmd.extend(['-p', str(port)])
        if user:
            cmd.append('-oUser=' + user)
        # hostappend is a dirty hack to handle scp's syntax
        cmd.extend(['-oBatchMode=true', host+hostappend])
        cmd.extend(cmdend)
        self.__cmd = cmd
        self.__starttime = time.time()
        nullf = open(os.path.devnull, 'w')
        _logger.debug("starting subprocess cmd=%r", cmd)
        if getoutput:
            stdout_target = subprocess.PIPE
        else:
            stdout_target = nullf
        self.__subproc = subprocess.Popen(cmd, stdout=stdout_target, stderr=nullf)
        nullf.close()
        if getoutput:
            self.__io_watch_id = gobject.io_add_watch(self.__subproc.stdout,
                                                      gobject.IO_IN|gobject.IO_ERR|gobject.IO_HUP,
                                                      self.__on_io)
        self.__output = StringIO()
        self.__child_watch_id = gobject.child_watch_add(self.__subproc.pid, self.__on_exited)
        self.__timeout_id = gobject.timeout_add(timeout, self.__on_timeout)

    def __on_io(self, source, condition):
        have_read = condition & gobject.IO_IN
        if have_read:
            _logger.debug("got status output")
            self.__output.write(os.read(source.fileno(), 8192))
        if ((condition & gobject.IO_HUP) or (condition & gobject.IO_ERR)):
            source.close()
            _logger.debug("got condition %s, cancelling status io check", condition)
            return False
        else:
            return have_read

    def __on_timeout(self):
        _logger.debug("timeout for host=%r cmd=%r", self.__host, self.__cmd)
        try:
            os.kill(self.__subproc.pid, signal.SIGHUP)
        except OSError, e:
            _logger.debug("failed to signal pid %s", pid, exc_info=True)
            pass
        self.emit('timeout', self.__output.getvalue())
        self.__timeout_id = 0
        return False

    def __on_exited(self, pid, condition):
        _logger.debug("command exited host=%r condition=%r cmd=%r", self.__host, condition, self.__cmd)
        if self.__timeout_id == 0:
            _logger.debug("command exited (but timeout already run")
            return
        gobject.source_remove(self.__timeout_id)
        self.__timeout_id = 0
        self.emit('complete', condition == 0, time.time() - self.__starttime, self.__output.getvalue())
        return False

class FaviconRetriever(gobject.GObject):
    __gsignals__ = {
        "favicon-loaded" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, [str]),
    }

    user = property(lambda self: self.__user)
    host = property(lambda self: self.__host)
    port = property(lambda self: self.__port)

    def __init__(self, user, host, port=None):
        super(FaviconRetriever, self).__init__()
        self.__user = user
        self.__host = host
        self.__port = port
        self.__cached_mtime = None
        self.__favicon_mtime = None
        self.__tmp_favicon_path = None

        hostport = userhost_pair_to_string(user, host, port)
        _logger.debug("loading favicon for %s", hostport)
        cmd = commands.mkarg('''python -c 'import os,sys,stat; print os.stat(sys.argv[1])[stat.ST_MTIME]' /etc/favicon.png''')
        req = AsyncCommandWithOutput(user, host, port, ['ssh'], ['sh', '-c', cmd])
        req.connect('timeout', self.__on_mtime_timeout)
        req.connect('complete', self.__on_mtime_complete)
        self.__active_req = req

    def __on_mtime_timeout(self, req, curoutput):
        _logger.debug("favicon mtime retrieval timeout")
        return False

    def __on_mtime_complete(self, req, status, elapsed_time, output):
        _logger.debug("favicon mtime retrieval complete, status: %r", status)
        if not status:
            return False
        mtime = int(output.strip())
        self.__favicon_mtime = mtime
        gobject.idle_add(self.__idle_start_scp)

    @log_except(_logger)
    def __idle_start_scp(self):
        current = _openssh_hosts_db.get_favicon_for_host(self.__host, self.__port)
        cached_mtime = 0
        if current is not None:
            (_, mtime) = current
            cached_mtime = mtime
        if self.__favicon_mtime > cached_mtime:
            (fd, tmppath) = tempfile.mkstemp('.png', 'favicon')
            self.__tmp_favicon_path = tmppath
            os.close(fd)

            _logger.debug("creating scp request")
            req = AsyncCommandWithOutput(self.__user, self.__host, self.__port,
                                         ['scp', '-p', '-q'], [tmppath],
                                         hostappend=':/etc/favicon.png', timeout=15000, getoutput=False)
            req.connect('timeout', self.__on_favicon_timeout)
            req.connect('complete', self.__on_favicon_complete)
            self.__active_req = req
        else:
            _logger.debug("favicon is up to date")
        return False

    def __on_favicon_timeout(self, req, elapsed):
        _logger.debug("favicon data retrieval timeout (%r elapsed)", elapsed)
        return False

    def __on_favicon_complete(self, req, status, elapsed_time, output):
        _logger.debug("favicon data retrieval complete; elapsed=%r status=%r", elapsed_time, status)
        if not status:
            return False
        self.emit('favicon-loaded', self.__tmp_favicon_path)
        return False

class HostConnectionMonitor(gobject.GObject):
    __gsignals__ = {
        "host-status" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, (gobject.TYPE_PYOBJECT,
                                                                      gobject.TYPE_BOOLEAN,
                                                                      gobject.TYPE_PYOBJECT,
                                                                      gobject.TYPE_PYOBJECT)),
    }
    def __init__(self):
        super(HostConnectionMonitor, self).__init__()
        
        self._CHECK_MS = 15000
        
        # map userhost -> failure count
        self._host_failures = {}
        # map userhost -> ids
        self.__host_monitor_ids = {}
        # map userhost -> various idle ids, etc
        self.__check_statuses = {}
        # Map userhost -> w output
        self.__check_status_output = {}
        

    def start_monitor(self, user, host):
        userhost = userhost_pair_to_string(user, host)
        if not (userhost in self.__host_monitor_ids or userhost in self.__check_statuses):
            _logger.debug("adding monitor for %s", userhost)
            self._host_failures[userhost] = 0
            self.__host_monitor_ids[userhost] = gobject.timeout_add(self._CHECK_MS, self.__check_host, userhost)

    def stop_monitor(self, user, host):
        userhost = userhost_pair_to_string(user, host)        
        _logger.debug("stopping monitor for %s", userhost)
        if userhost in self.__host_monitor_ids:
            monid = self.__host_monitor_ids[userhost]
            gobject.source_remove(monid)
            del self.__host_monitor_ids[userhost]
        if userhost in self.__check_statuses:
            del self.__check_statusesuser[userhost]

    def get_monitors(self):
        return self.__host_monitor_ids

    def __on_check_io(self, source, condition, userhost):
        have_read = condition & gobject.IO_IN
        if have_read:
            _logger.debug("got status output")
            self.__check_status_output[userhost] += os.read(source.fileno(), 8192)
        if ((condition & gobject.IO_HUP) or (condition & gobject.IO_ERR)):
            source.close()
            _logger.debug("got condition %s, cancelling status io check", condition)
            return False
        else:
            return have_read

    def __check_host(self, userhost):
        _logger.debug("performing check for %s", userhost)
        del self.__host_monitor_ids[userhost]
        cmd = list(get_base_sshcmd())
        cmd.extend(get_connection_sharing_args())
        starttime = time.time()
        # This is a hack.  Blame Adam Jackson.
        cmd.extend(['-oBatchMode=true', userhost, 'uptime'])
        nullf = open(os.path.devnull, 'w')
        subproc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=nullf)
        nullf.close()
        io_watch_id = gobject.io_add_watch(subproc.stdout,
                                           gobject.IO_IN|gobject.IO_ERR|gobject.IO_HUP,
                                           self.__on_check_io, userhost)
        self.__check_status_output[userhost] = ""
        child_watch_id = gobject.child_watch_add(subproc.pid, self.__on_check_exited, userhost)
        timeout_id = gobject.timeout_add(7000, self.__check_timeout, userhost)
        self.__check_statuses[userhost] = [starttime, subproc.pid, timeout_id, child_watch_id, io_watch_id]
        return False

    def __check_timeout(self, userhost):
        _logger.debug("timeout for host=%r", userhost)
        try:
            (starttime, pid, timeout_id, child_watch_id, io_watch_id) = self.__check_statuses[userhost]
        except KeyError, e:
            return False
        self.__check_statuses[2] = 0
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError, e:
            _logger.debug("failed to signal pid %s", pid, exc_info=True)
            pass
        return False

    def __on_check_exited(self, pid, condition, userhost):
        _logger.debug("check exited, pid=%s condition=%s host=%s", pid, condition, userhost)
        try:
            (starttime, pid, timeout_id, child_watch_id, io_watch_id) = self.__check_statuses[userhost]
        except KeyError, e:
            _logger.debug("confused; no status for userhost=%s", userhost)
            return False
        if timeout_id > 0:
            gobject.source_remove(timeout_id)
        del self.__check_statuses[userhost]
        abort = False
        if condition == 0:
            self._host_failures[userhost] = 0
        else:
            self._host_failures[userhost] += 1
            if self._host_failures[userhost] > 3:
                _logger.debug("too many failures for host=%s", userhost)
                abort = True
        if not abort:
            self.__host_monitor_ids[userhost] = gobject.timeout_add(self._CHECK_MS, self.__check_host, userhost)
        self.emit('host-status', userhost, condition == 0, time.time()-starttime, self.__check_status_output[userhost])
        del self.__check_status_output[userhost]
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
        "status-changed" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, []),
        "close" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, []),
        # Emitted when we do a reconnect so that other tabs for this host can pick it up
        "reconnect" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, []),
        "metadata-changed" : (gobject.SIGNAL_RUN_LAST, gobject.TYPE_NONE, ()),
    }

    latency = property(lambda self: self.__latency)
    status_line = property(lambda self: self.__status_output_first)
    connecting_state = property(lambda self: self.__connecting_state)
    connected = property(lambda self: self.__connected)
    ssh_options = property(lambda self: self.__sshopts)

    ssh_opt_with_args = "bcDeFiLlmOopRSw"
    ssh_opt_witouth_args = "1246AaCfgKkMNnqsTtVvXxY"

    def __init__(self, args, cwd, actions=None, inituser=None, inithost=None):
        super(SshTerminalWidget, self).__init__()

        self.__args = args
        self.__sshcmd = list(get_base_sshcmd())
        self.__cwd = cwd
        self.__user = inituser
        self.__host = inithost
	self.__port = None
        self.__sshopts = []
        self.__actions = actions
        self.__idle_start_favicon_id = 0
        self.__favicon_path = None
        self.__favicon_pixbuf = None
        self.__favicon_retriever = None
        self.__favicon_retriever_connections = []

        self.__init_state()

        enable_connection_sharing = True
        need_arg = False
        this_is_port = False
        port_in_args = False

        for arg in args:
            if not need_arg and not arg.startswith('-'):
                need_arg = False
                if self.__host is None:
                    host_port = arg.split(":")
                    if len(host_port) == 2:
                        self.__host = host_port[0]
                        self.__port = host_port[1]
                    else:
                        self.__host = arg
                    try:
                        (self.__user,self.__host) = self.__host.split('@', 1)
                    except ValueError, e:
                        pass
            elif this_is_port:
                self.__port = arg
                this_is_port = False
                need_arg = False
                port_in_args = True
                self.__sshcmd.append(arg)
            else:
                if arg == '-oControlPath=none':
                    enable_connection_sharing = False
                elif self.ssh_opt_with_args.find(arg[1:]) != -1:
                    need_arg = True
                    if arg == "-p":
                        this_is_port = True
                else:
                    need_arg = False
                self.__sshcmd.append(arg)

        if self.__user:
            self.__sshcmd.append('-oUser=' + self.__user)
        self.__sshcmd.append(self.__host)

        if not port_in_args and self.__port:
            self.__sshcmd.append("-p")
            self.__sshcmd.append(self.__port)

        if enable_connection_sharing:
            self.__sshcmd.extend(get_connection_sharing_args())

        header = gtk.VBox()
        self.__msg = gtk.Label()
        self.__msg.set_alignment(0.0, 0.5)
        self.__msgarea_mgr = MsgAreaController()
        #header.pack_start(self.__msg)
        header.pack_start(self.__msgarea_mgr)
        self.pack_start(header, expand=False)
        self.ssh_connect()

    def __init_state(self):
        if self.__idle_start_favicon_id > 0:
            gobject.source_remove(self.__idle_start_favicon_id)
            self.__idle_start_favicon_id = 0
        if self.__favicon_retriever is not None:
            for sigid in self.__favicon_retriever_connections:
                self.__favicon_retriever.disconnect(sigid)
            self.__favicon_retriever_connections = []
        self.__favicon_retriever = None

        self.__global_connection_changed = False
        self.__connecting_state = False
        self.__connected = None
        self.__cmd_exited = False
        self.__latency = None
        self.__status_output_first = None

    def set_global_connection_changed(self, changed):
        self.__global_connection_changed = changed
        if changed:
            self.__msgarea_mgr.clear()
        elif self.__cmd_exited:
            self.__show_disconnected()

    def set_status(self, connected, latency, status_output=None):
        if not connected and self.__connecting_state:
            return
        self.__connecting_state = False
        connected_changed = self.__connected != connected
        latency_changed = (not self.__latency) or (self.__latency*0.9 > latency) or (self.__latency*1.1 < latency)
        if not (connected_changed or latency_changed):
            return
        self.__connected = connected
        self.__latency = latency
        buf = StringIO(status_output)
        firstline = buf.readline().strip()
        self.__status_output_first = firstline
        self.emit('status-changed')
        self.__sync_msg()

    def __sync_msg(self):
        return

    def __on_favicon_loaded(self, retriever, faviconpath):
        _logger.debug("favicon loaded; path=%r", faviconpath)
        self.__favicon_path = _openssh_hosts_db.save_favicon(self.__host, self.__port, faviconpath)
        self.__favicon_pixbuf = _openssh_hosts_db.render_cached_favicon(self.__favicon_path)
        self.emit('metadata-changed')

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

        current = _openssh_hosts_db.get_favicon_for_host(self.__host, self.__port)
        if current is not None:
            (pixbufpath, mtime) = current
            self.__favicon_path = pixbufpath
            self.__favicon_pixbuf = _openssh_hosts_db.render_cached_favicon(pixbufpath)
        self.__idle_start_favicon_id = gobject.timeout_add(3000, self.__idle_start_favicon)

    def __idle_start_favicon(self):
        if self.__favicon_retriever is not None:
            return
        self.__favicon_retriever = FaviconRetriever(self.__user, self.__host, self.__port)
        sigid = self.__favicon_retriever.connect('favicon-loaded', self.__on_favicon_loaded)
        self.__favicon_retriever_connections.append(sigid)

    def ssh_reconnect(self):
        # TODO - do this in a better way
        if not self.__term.exited and self.__term.pid:
            os.kill(self.__term.pid, signal.SIGTERM)
        self.remove(self.__term)
        self.__term.destroy()
        self.__init_state()
        self.ssh_connect()
        self.emit('reconnect')

    def __on_child_exited(self, term):
        _logger.debug("disconnected")
        self.__cmd_exited = True
        self.__show_disconnected()

    def __show_disconnected(self):
        self.__msg.hide()
        if self.__global_connection_changed:
            return
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

    def get_pixbuf(self):
        return self.__favicon_pixbuf

    def get_host(self):
        return self.__host

    def get_user(self):
        return self.__user

    def get_port(self):
        return self.__port

    def get_options(self):
        return self.__sshopts

class SshWindow(VteWindow):
    def __init__(self, **kwargs):
        super(SshWindow, self).__init__(title='Secure Shell', icon_name='hotwire-openssh', **kwargs)

        self.__ui_string = """
<ui>
  <menubar name='Menubar'>
    <menu action='FileMenu'>
      <placeholder name='FileAdditions'>
        <menuitem action='NewConnection'/>
        <menuitem action='CopyConnection'/>
        <menuitem action='OpenSFTP'/>
        <separator/>
        <menuitem action='Reconnect'/>
        <menuitem action='ReconnectAll'/>
        <separator/>
      </placeholder>
    </menu>
  </menubar>
</ui>
"""

        self._get_notebook().connect('switch-page', self.__on_page_switch)

        try:
            self.__cached_nm_connected = None
            self.__nm_proxy = dbus.SystemBus().get_object('org.freedesktop.NetworkManager', '/org/freedesktop/NetworkManager')
            self.__nm_proxy.connect_to_signal('StateChange', self.__on_nm_state_change)
            dbus.Interface(self.__nm_proxy, 'org.freedesktop.DBus.Properties').Get('org.freedesktop.NetworkManager',
                                                                                   'State', reply_handler=self.__on_nm_state_change,
                                                                                   error_handler=self.__on_dbus_error)
        except dbus.DBusException, e:
            _logger.debug("Couldn't find NetworkManager")
            self.__nm_proxy = None

        self.__status_hbox = gtk.HBox()
        self._get_vbox().pack_start(self.__status_hbox, expand=False)
        self.__statusbar = gtk.Statusbar()
        self.__status_hbox.pack_start(self.__statusbar, expand=True)
        self.__statusbar_ctx = self.__statusbar.get_context_id("HotSSH")

        self.__in_reconnect = False
        self.__idle_stop_monitoring_id = 0

        self.connect("notify::is-active", self.__on_is_active_changed)
        _hostmonitor.connect('host-status', self.__on_host_status)

        self.__connhistory = get_history()
        self.__local_avahi = get_local_avahi()

        self.__merge_ssh_ui()

    def __add_to_history(self, args, inituser=None, inithost=None):
        user = inituser
        host = inithost
        need_arg = False
        options = []

        for arg in args:
            if arg.startswith('-') and SshTerminalWidget.ssh_opt_with_args.find(arg[1:]) >= 0:
                options.append(arg)
                if SshTerminalWidget.ssh_opt_with_args.find(arg[1:]) >= 0:
                    need_arg = True
                continue

            if need_arg:
                options.append(arg)
            elif host is None:
                if arg.find('@') >= 0:
                    (user, host) = arg.split('@', 1)
                else:
                    host = arg
            need_arg = False

        self.__connhistory.add_connection(user, host, options)

    def remote_new_tab(self, args, cwd):
        return self.new_tab(args, cwd, exit_on_cancel=False)

    def new_tab(self, args, cwd, exit_on_cancel=True, **kwargs):
        if len(args) == 0 and len(kwargs) == 0:
            self.open_connection_dialog(exit_on_cancel=exit_on_cancel)
        else:
            self.__add_to_history(args, **kwargs)
            term = SshTerminalWidget(args=args, cwd=cwd, actions=self.__action_group, **kwargs)
            self.append_widget(term)

    def append_widget(self, w):
        super(SshWindow, self).append_widget(w)
        w.connect('status-changed', self.__on_widget_status_changed)
        w.connect('reconnect', self.__on_widget_reconnect)
        self.__sync_status_display()

    def __on_widget_status_changed(self, w):
        self.__sync_status_display()

    def __on_widget_reconnect(self, changed_widget):
        if self.__in_reconnect:
            return
        self.__in_reconnect = True
        changed_user = changed_widget.get_user()
        changed_host = changed_widget.get_host()
        changed_userhost = userhost_pair_to_string(changed_user, changed_host)
        _logger.debug("reconnecting all widgets for host %s", changed_userhost)
        for widget in self._get_notebook().get_children():
            if changed_widget is widget:
                continue
            host = widget.get_host()
            user = widget.get_user()
            userhost = userhost_pair_to_string(user, host)
            if userhost != changed_userhost:
                continue
            widget.ssh_reconnect()
        self.__in_reconnect = False

    def __sync_status_display(self):
        notebook = self._get_notebook()
        pn = notebook.get_current_page()
        if pn < 0:
            return
        widget = notebook.get_nth_page(pn)
        if widget.connecting_state:
            text = _('Connecting')
        elif widget.connected is True:
            text = _('Connected; %.2fs latency; %s') % (widget.latency, widget.status_line)
        elif widget.connected is False:
            text = _('Connection timed out')
        elif widget.connected is None:
            text = _('Checking connection')
        #if len(widget.ssh_options) > 1:
        #    text += _('; Options: ') + (' '.join(map(gobject.markup_escape_text, widget.ssh_options)))
        id = self.__statusbar.push(self.__statusbar_ctx, text)

    def __on_dbus_error(self, *args, **kwargs):
        _logger.error("DBus error: %r %r", args, kwargs)

    @log_except(_logger)
    def __on_nm_state_change(self, *args):
        if len(args) > 1:
            (_, curstate) = args
        else:
            curstate = args[0]
        _logger.debug("nm state: %s", curstate)
        connected = curstate == 3
        if (self.__cached_nm_connected is not None) and \
            (not self.__cached_nm_connected) and connected:
            self.__do_network_change_notify()
        self.__cached_nm_connected = connected
        for child in self._get_notebook():
            child.set_global_connection_changed(True)

    def __do_network_change_notify(self):
        mgr = self._get_msgarea_mgr()
        msgarea = mgr.new_from_text_and_icon(gtk.STOCK_INFO, _('Network connection changed'),
                                                            buttons=[(gtk.STOCK_CLOSE, gtk.RESPONSE_CLOSE)])
        reconnect = self.__action_group.get_action('ReconnectAll')
        msgarea.add_stock_button_with_text(reconnect.get_property('label'),
                                           reconnect.get_property('stock-id'), gtk.RESPONSE_ACCEPT)
        msgarea.connect('response', self.__on_msgarea_response)
        msgarea.show_all()

    def __on_msgarea_response(self, msgarea, respid):
        mgr = self._get_msgarea_mgr()
        mgr.clear()
        if respid == gtk.RESPONSE_ACCEPT:
            for child in self._get_notebook():
                child.set_global_connection_changed(False)
            reconnect = self.__action_group.get_action('ReconnectAll')
            reconnect.activate()

    @log_except(_logger)
    def __on_host_status(self, hostmon, userhost, connected, latency, status_output):
        _logger.debug("got host status host=%s conn=%s latency=%s", userhost, connected, latency)
        for widget in self._get_notebook().get_children():
            child_userhost = userhost_pair_to_string(widget.get_user(), widget.get_host())
            if child_userhost != userhost:
                continue
            widget.set_status(connected, latency, status_output)

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
        self.__sync_monitoring(new_pn=pn)
        self.__sync_status_display()

    def __get_userhost_for_pn(self, pn):
        notebook = self._get_notebook()
        if pn >= 0:
            widget = notebook.get_nth_page(pn)
            return userhost_pair_to_string(widget.get_user(), widget.get_host())
        return None

    def __stop_monitoring(self):
        notebook = self._get_notebook()
        pn = notebook.get_current_page()
        if pn >= 0:
            prev_widget = notebook.get_nth_page(pn)
            prev_user = prev_widget.get_user()
            prev_host = prev_widget.get_host()
            _hostmonitor.stop_monitor(prev_user, prev_host)
            prev_widget.set_status(None, None)

    def __start_monitoring(self, pn=None):
        notebook = self._get_notebook()
        if pn is not None:
            pagenum = pn
        else:
            pagenum = notebook.get_current_page()
        widget = notebook.get_nth_page(pagenum)
        _hostmonitor.start_monitor(widget.get_user(), widget.get_host())

    def __sync_monitoring(self, new_pn):
        prev_userhost = self.__get_userhost_for_pn(self._get_notebook().get_current_page())
        new_userhost = self.__get_userhost_for_pn(new_pn)
        if prev_userhost != new_userhost:
            self.__stop_monitoring()
            self.__start_monitoring(pn=new_pn)

    def __merge_ssh_ui(self):
        self.__using_accels = True
        self.__actions = actions = [
            ('NewConnection', gtk.STOCK_NEW, _('Connect to server'), '<control><shift>O',
             _('Open a new Secure Shell connection'), self.__new_connection_cb),
            ('CopyConnection', gtk.STOCK_JUMP_TO, _('New tab for connection'), '<control><shift>T',
             _('Open a new tab for the same remote computer'), self.__copy_connection_cb),
            ('OpenSFTP', gtk.STOCK_OPEN, _('Open SFTP'), '<control><shift>S',
             _('Open a SFTP connection'), self.__open_sftp_cb),
            ('ConnectionMenu', None, _('Connection')),
            ('Reconnect', gtk.STOCK_CONNECT, _('_Reconnect'), '<control><shift>R', _('Reset connection to server'), self.__reconnect_cb),
            ('ReconnectAll', gtk.STOCK_CONNECT, _('R_econnect All'), None, _('Reset all connections'), self.__reconnect_all_cb),
            ]
        self.__action_group = self._merge_ui(self.__actions, self.__ui_string)

    @log_except(_logger)
    def __copy_connection_cb(self, action):
        notebook = self._get_notebook()
        widget = notebook.get_nth_page(notebook.get_current_page())
        user = widget.get_user()
        host = widget.get_host()
        opts = widget.get_options()
        self.new_tab(widget.get_options(), None, inituser=user, inithost=host)

    def open_connection_dialog(self, exit_on_cancel=False):
        win = ConnectDialog(parent=self, history=self.__connhistory, local_avahi=self.__local_avahi)
        sshargs = win.run_get_cmd()
        if not sshargs:
            # We get here when called with no arguments, and we're the main instance.
            if exit_on_cancel:
                sys.exit(0)
            return
        self.new_tab(sshargs, None, exit_on_cancel=exit_on_cancel)

    @log_except(_logger)
    def __new_connection_cb(self, action):
        self.open_connection_dialog()

    @log_except(_logger)
    def __open_sftp_cb(self, action):
        notebook = self._get_notebook()
        widget = notebook.get_nth_page(notebook.get_current_page())
        host = widget.get_host()
        port = widget.get_port()
        if port:
            subprocess.Popen(['nautilus', 'sftp://%s:%s' % (host,port)])
        else:
            subprocess.Popen(['nautilus', 'sftp://%s' % (host,)])

    @log_except(_logger)
    def __reconnect_cb(self, a):
        notebook = self._get_notebook()
        widget = notebook.get_nth_page(notebook.get_current_page())
        widget.ssh_reconnect()

    @log_except(_logger)
    def __reconnect_all_cb(self, a):
        for widget in self._get_notebook().get_children():
            widget.ssh_reconnect()

    def _do_about(self):
        dlg = HotSshAboutDialog()
        dlg.run()
        dlg.destroy()

class SshApp(VteApp):
    def __init__(self):
        super(SshApp, self).__init__(SshWindow)
        self.__old_sessionpath = os.path.expanduser('~/.hotwire/hotssh.session')
        self.__sessionpath = os.path.expanduser('~/.hotssh/hotssh-session.xml')
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
        alltabs_count = 0
        allhosts = set()
        if savedsession:
            for window in savedsession:
                for connection in window:
                    allhosts.add(connection['userhost'])
                    alltabs_count += 1
        if len(allhosts) > 0:
            dlg = gtk.MessageDialog(parent=None, flags=0, type=gtk.MESSAGE_QUESTION,
                                    buttons=gtk.BUTTONS_CANCEL,
                                    message_format=_("Restore saved session?"))
            button = dlg.add_button(_('_Reconnect'), gtk.RESPONSE_ACCEPT)
            button.set_property('image', gtk.image_new_from_stock('gtk-connect', gtk.ICON_SIZE_BUTTON))
            dlg.set_default_response(gtk.RESPONSE_ACCEPT)
            allhosts_count = len(allhosts)
            # Translators: %d is the number of hosts we are about to reconnect to.
            text = gettext.ngettext('Reconnect to %d host' % (allhosts_count,),
                                    'Reconnect to %d hosts' % (allhosts_count,), len(allhosts))
            dlg.format_secondary_markup(text)

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
            try:
                f = open(self.__old_sessionpath)
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
                user = child.getAttribute('user')
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
                userhost = userhost_pair_to_string(user, host)
                kwargs = {'userhost': userhost}
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
        tempf_path = tempfile.mktemp('.xml.tmp', 'hotssh-session', os.path.dirname(self.__sessionpath))
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
                user = widget.get_user()
                if user is not None:
                    connection.setAttribute('user', user)
                host = widget.get_host()
                if host is not None:
                    connection.setAttribute('host', host)
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
