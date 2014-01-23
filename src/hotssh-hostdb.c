/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <string.h>

#include "hotssh-hostdb.h"
#include "hotssh-win.h"
#include "hotssh-prefs.h"

#include "libgsystem.h"

#define WRITE_IDLE_SECONDS (5)

struct _HotSshHostDB
{
  GObject parent;
};

struct _HotSshHostDBClass
{
  GObjectClass parent_class;
};

typedef struct _HotSshHostDBPrivate HotSshHostDBPrivate;

struct _HotSshHostDBPrivate
{
  GtkListStore *model;
  GHashTable *openssh_knownhosts; /* str -> str */
  GKeyFile *hostdb;
  GFile *openssh_dir;
  GFile *openssh_knownhosts_path;
  GFile *hotssh_hostdb_path;
  GFileMonitor *knownhosts_monitor;

  GHashTable *add_knownhost_queue;

  guint idle_save_hostdb_id;
  guint idle_save_knownhosts_id;
  char *new_hostdb_contents;
};

G_DEFINE_TYPE_WITH_PRIVATE(HotSshHostDB, hotssh_hostdb, G_TYPE_OBJECT)

static char *
hostname_and_port_to_string (const char *hostname,
                             guint port)
{
  if (port == 22)
    return g_strdup (hostname);
  return g_strdup_printf ("[%s]:%u", hostname, port);
}

static char *
address_to_string (GNetworkAddress    *address)
{
  return hostname_and_port_to_string (g_network_address_get_hostname (address),
                                      g_network_address_get_port (address));
}

static void
mark_all_entries_unknown (HotSshHostDB    *self)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GtkTreeIter iter;

  if (!gtk_tree_model_get_iter_first ((GtkTreeModel*)priv->model, &iter))
    return;
  while (TRUE)
    {
      gtk_list_store_set (priv->model, &iter,
                          HOTSSH_HOSTDB_COLUMN_IS_KNOWN, FALSE,
                          -1);

      if (!gtk_tree_model_iter_next ((GtkTreeModel*)priv->model, &iter))
        break;
    }
}

static void
mark_all_entries_known_by_address (HotSshHostDB    *self,
                                   const char      *hostname,
                                   guint            port,
                                   guint            lineno,
                                   const char      *keytype,
                                   const char      *key_base64)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GtkTreeIter iter;

  if (!gtk_tree_model_get_iter_first ((GtkTreeModel*)priv->model, &iter))
    return;
  while (TRUE)
    {
      gs_free char *model_hostname = NULL;
      guint model_port;
      gboolean is_known = FALSE;

      gtk_tree_model_get ((GtkTreeModel*)priv->model, &iter,
                          HOTSSH_HOSTDB_COLUMN_HOSTNAME, &model_hostname,
                          HOTSSH_HOSTDB_COLUMN_PORT, &model_port,
                          HOTSSH_HOSTDB_COLUMN_IS_KNOWN, &is_known,
                          -1);

      if (!is_known &&
          g_ascii_strcasecmp (hostname, model_hostname) == 0 &&
          port == model_port)
        {
          gtk_list_store_set (priv->model, &iter,
                              HOTSSH_HOSTDB_COLUMN_IS_KNOWN, TRUE,
                              HOTSSH_HOSTDB_COLUMN_HOST_KEY_TYPE, keytype,
                              HOTSSH_HOSTDB_COLUMN_HOST_KEY_BASE64, key_base64,
                              HOTSSH_HOSTDB_COLUMN_OPENSSH_KNOWNHOST_LINE, lineno,
                              -1);
        }

      if (!gtk_tree_model_iter_next ((GtkTreeModel*)priv->model, &iter))
        break;
    }
}

static void
on_knownhosts_changed (GFileMonitor        *monitor,
                       GFile               *src,
                       GFile               *other,
                       GFileMonitorEvent    event,
                       gpointer             user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GError *local_error = NULL;
  gs_free char *contents = NULL;
  gsize len;
  char *iter;
  char *eol;
  char *carriage;
  guint lineno = 0;

  if (!g_file_load_contents (priv->openssh_knownhosts_path, NULL,
                             &contents, &len, NULL,
                             &local_error))
    goto out;

  mark_all_entries_unknown (self);

  g_hash_table_remove_all (priv->openssh_knownhosts);

  iter = contents;
  while (TRUE)
    {
      gs_strfreev char **parts = NULL;
      GNetworkAddress *address_obj = NULL;
      gs_free char *address_str = NULL;
      char *comma;
      const char *keytype;
      const char *key_base64;

      eol = strchr (iter, '\n');
      if (eol)
        *eol = '\0';
      
      carriage = strrchr (iter, '\r');
      if (carriage)
        *carriage = '\0';
      
      if (contents[0] == '#')
        goto next;

      parts = g_strsplit (iter, " ", -1);
      if (!parts || g_strv_length (parts) < 3)
        goto next;
      
      comma = strchr (parts[0], ',');
      if (comma)
        *comma = '\0';

      address_obj = (GNetworkAddress*)g_network_address_parse (parts[0], 22, NULL);
      if (!address_obj)
        goto next;
      address_str = address_to_string (address_obj);

      keytype = parts[1];
      key_base64 = parts[2];

      mark_all_entries_known_by_address (self,
                                         g_network_address_get_hostname (address_obj),
                                         g_network_address_get_port (address_obj),
                                         lineno,
                                         keytype,
                                         key_base64);

      g_hash_table_insert (priv->openssh_knownhosts, address_str, parts);
      address_str = NULL;
      parts = NULL;

    next:
      lineno++;
      if (eol)
        iter = eol + 1;
      else
        break;
    }

  g_debug ("Read %d known hosts", gtk_tree_model_iter_n_children ((GtkTreeModel*)priv->model, NULL));

 out:
  if (local_error)
    {
      g_debug ("Failed to read '%s': %s", gs_file_get_path_cached (priv->openssh_knownhosts_path),
               local_error->message);
      g_clear_error (&local_error);
    }
}

static gboolean
set_row_from_group (HotSshHostDB         *self,
                    const char           *groupname,
                    GtkTreeIter          *iter)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  gs_free char *hostname = NULL;
  gs_free char *username = NULL;
  gs_free char *port_str = NULL;
  gint64 last_used;
  int port;
  gboolean is_known;

  last_used = g_key_file_get_uint64 (priv->hostdb, groupname, "last-used", NULL);

  username = g_key_file_get_string (priv->hostdb, groupname, "username", NULL);
  if (!(username && username[0]))
    return FALSE;

  hostname = g_key_file_get_string (priv->hostdb, groupname, "hostname", NULL);
  if (!(hostname && hostname[0]))
    return FALSE;

  port = g_key_file_get_integer (priv->hostdb, groupname, "port", NULL);
  if (port <= 0 || port > G_MAXUINT16)
    port = 22;

  is_known = g_hash_table_contains (priv->openssh_knownhosts, hostname);

  gtk_list_store_set (priv->model, iter,
                      HOTSSH_HOSTDB_COLUMN_ID, groupname,
                      HOTSSH_HOSTDB_COLUMN_USERNAME, username,
                      HOTSSH_HOSTDB_COLUMN_HOSTNAME, hostname,
                      HOTSSH_HOSTDB_COLUMN_PORT, (guint)port,
                      HOTSSH_HOSTDB_COLUMN_LAST_USED, last_used,
                      HOTSSH_HOSTDB_COLUMN_IS_KNOWN, is_known,
                      -1);
  return TRUE;
}

static void
load_hostdb (HotSshHostDB         *self)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  gs_strfreev char **hostdb_groups = NULL;
  char **strviter;
  GError *local_error = NULL;

  g_clear_pointer (&priv->hostdb, g_key_file_unref);
  priv->hostdb = g_key_file_new ();
  
  if (!g_key_file_load_from_file (priv->hostdb, gs_file_get_path_cached (priv->hotssh_hostdb_path), 0, &local_error))
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_clear_error (&local_error);
      else
        goto out;
    }

  hostdb_groups = g_key_file_get_groups (priv->hostdb, NULL);
  for (strviter = hostdb_groups; strviter && *strviter; strviter++)
    {
      const char *group = *strviter;
      GtkTreeIter iter;

      if (!g_str_has_prefix (group, "entry "))
        continue;

      gtk_list_store_append (priv->model, &iter);
      if (!set_row_from_group (self, group, &iter))
        {
          gtk_list_store_remove (priv->model, &iter);
        }
    }
  
 out:
  if (local_error)
    {
      g_debug ("Failed to read '%s': %s", gs_file_get_path_cached (priv->openssh_knownhosts_path),
               local_error->message);
      g_clear_error (&local_error);
    }
}

static void
on_replace_hostdb_contents_complete (GObject                *src,
                                      GAsyncResult           *result,
                                      gpointer                user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GError *local_error = NULL;

  priv->idle_save_hostdb_id = 0;

  if (!g_file_replace_contents_finish ((GFile*)src, result, NULL, &local_error))
    goto out;

 out:
  g_clear_pointer (&priv->new_hostdb_contents, g_free);
  if (local_error)
    {
      g_warning ("Failed to save '%s': %s",
                 gs_file_get_path_cached (priv->hotssh_hostdb_path),
                 local_error->message);
      g_clear_error (&local_error);
    }
}

static gboolean
idle_save_hostdb (gpointer user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  gsize len;

  priv->new_hostdb_contents = g_key_file_to_data (priv->hostdb, &len, NULL);
  g_assert (priv->new_hostdb_contents);

  g_file_replace_contents_async (priv->hotssh_hostdb_path, priv->new_hostdb_contents, len, NULL,
                                 FALSE, 0, NULL,
                                 on_replace_hostdb_contents_complete, self);
  return FALSE;
}

static void
queue_save_hostdb (HotSshHostDB    *self)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  if (priv->idle_save_hostdb_id > 0)
    return;
  priv->idle_save_hostdb_id = g_timeout_add_seconds (WRITE_IDLE_SECONDS, idle_save_hostdb, self);
}

static void
append_randword (GString *buf)
{
  guint i;
  for (i = 0; i < 2; i++)
    g_string_append_printf (buf, "%02X", (guint8) g_random_int_range (0, 256));
}

static char *
allocate_groupname (HotSshHostDB       *self)
{
  GString *buf = g_string_new ("entry ");
  guint i;

  for (i = 0; i < 2; i++)
    append_randword (buf);
  g_string_append_c (buf, '-');
  for (i = 0; i < 3; i++)
    {
      append_randword (buf);
      g_string_append_c (buf, '-');
    }
  for (i = 0; i < 3; i++)
    append_randword (buf);
  
  return g_string_free (buf, FALSE);
}

static void
set_group_values (HotSshHostDB     *self,
                  const char       *groupname,
                  const char       *username,
                  GNetworkAddress  *address)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  guint port = g_network_address_get_port (address);
  g_key_file_set_string (priv->hostdb, groupname, "username",
                         username ? username : g_get_user_name ());
  g_key_file_set_string (priv->hostdb, groupname, "hostname",
                         g_network_address_get_hostname (address));
  if (port != 22)
    g_key_file_set_integer (priv->hostdb, groupname, "port", port);
}

void
hotssh_hostdb_add_entry (HotSshHostDB     *self,
                         const char       *username,
                         GNetworkAddress  *address,
                         char            **out_id)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GtkTreeIter iter;
  gs_free char *groupname = allocate_groupname (self);

  set_group_values (self, groupname, username, address);

  gtk_list_store_append (priv->model, &iter);
  (void) set_row_from_group (self, groupname, &iter);
  queue_save_hostdb (self);
  *out_id = groupname;
  groupname = NULL;
}

gboolean
hotssh_hostdb_lookup_by_id (HotSshHostDB   *self,
                            const char     *id,
                            GtkTreeIter    *out_iter)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  if (!gtk_tree_model_get_iter_first ((GtkTreeModel*)priv->model, out_iter))
    return FALSE;
  while (TRUE)
    {
      gs_free char *model_id = NULL;

      gtk_tree_model_get ((GtkTreeModel*)priv->model, out_iter,
                          HOTSSH_HOSTDB_COLUMN_ID, &model_id,
                          -1);

      if (model_id != NULL && strcmp (id, model_id) == 0)
        return TRUE;

      if (!gtk_tree_model_iter_next ((GtkTreeModel*)priv->model, out_iter))
        break;
    }
  return FALSE;
}

void
hotssh_hostdb_set_entry_basic (HotSshHostDB    *self,
                               const char      *id,
                               const char      *username,
                               GNetworkAddress *address)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GtkTreeIter iter;

  if (!hotssh_hostdb_lookup_by_id (self, id, &iter))
    return;

  set_group_values (self, id, username, address);
  (void) set_row_from_group (self, id, &iter);
  queue_save_hostdb (self);
}

void
hotssh_hostdb_update_last_used (HotSshHostDB    *self,
                                const char      *id)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GtkTreeIter iter;

  if (!hotssh_hostdb_lookup_by_id (self, id, &iter))
    return;

  g_key_file_set_uint64 (priv->hostdb, id, "last-used", g_get_real_time () / G_USEC_PER_SEC);

  (void) set_row_from_group (self, id, &iter);
  queue_save_hostdb (self);
}

static void
on_knownhosts_splice_complete (GObject            *src,
                               GAsyncResult       *result,
                               gpointer            user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  gssize bytes_written;
  GError *local_error = NULL;

  priv->idle_save_knownhosts_id = 0;

  g_debug ("knownhosts splice complete");

  bytes_written = g_output_stream_splice_finish ((GOutputStream*)src, result, &local_error);
  if (bytes_written == -1)
    goto out;

 out:
  if (local_error)
    {
      g_warning ("Failed to write '%s': %s",
                 gs_file_get_path_cached (priv->openssh_knownhosts_path),
                 local_error->message);
      g_error_free (local_error);
    }
}

static void
on_knownhosts_opened (GObject            *src,
                      GAsyncResult       *result,
                      gpointer            user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GHashTableIter hashiter;
  gpointer key, value;
  GError *local_error = NULL;
  gs_unref_object GFileOutputStream *fileout = NULL;
  gs_unref_object GMemoryInputStream *membuf = (GMemoryInputStream*)g_memory_input_stream_new ();

  g_debug ("knownhosts opened for append");

  fileout = g_file_append_to_finish ((GFile*)src, result, &local_error);
  if (!fileout)
    goto out;

  g_hash_table_iter_init (&hashiter, priv->add_knownhost_queue);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GtkTreeIter iter;
      const char *id = key;
      gs_free char *hostname = NULL;
      guint port;
      gs_free char *keytype = NULL;
      gs_free char *key_base64 = NULL;
      gs_free char *ipaddr = NULL;
      gs_free char *address = NULL;
      char *out_line;

      if (!hotssh_hostdb_lookup_by_id (self, id, &iter))
        continue;

      gtk_tree_model_get ((GtkTreeModel*)priv->model, &iter,
                          HOTSSH_HOSTDB_COLUMN_HOSTNAME, &hostname,
                          HOTSSH_HOSTDB_COLUMN_PORT, &port,
                          HOTSSH_HOSTDB_COLUMN_HOST_KEY_TYPE, &keytype,
                          HOTSSH_HOSTDB_COLUMN_HOST_KEY_BASE64, &key_base64,
                          HOTSSH_HOSTDB_COLUMN_HOST_KEY_IP_ADDRESS, &ipaddr,
                          -1);

      address = hostname_and_port_to_string (hostname, port);
      if (ipaddr)
        out_line = g_strdup_printf ("%s,%s %s %s\n", address, ipaddr,
                                    keytype, key_base64);
      else
        out_line = g_strdup_printf ("%s %s %s\n", address,
                                    keytype, key_base64);

      g_memory_input_stream_add_bytes (membuf, g_bytes_new_take (out_line, strlen (out_line)));

      g_hash_table_iter_remove (&hashiter);
    }

  g_output_stream_splice_async ((GOutputStream*)fileout, (GInputStream*)membuf,
                                G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | 
                                G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                G_PRIORITY_DEFAULT,
                                NULL,
                                on_knownhosts_splice_complete,
                                self);

 out:
  if (local_error)
    {
      priv->idle_save_knownhosts_id = 0;
      g_warning ("Failed to write '%s': %s",
                 gs_file_get_path_cached (priv->openssh_knownhosts_path),
                 local_error->message);
      g_error_free (local_error);
    }
}

static gboolean
idle_write_knownhosts (gpointer user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);

  g_file_append_to_async (priv->openssh_knownhosts_path, 0,
                          G_PRIORITY_DEFAULT, NULL,
                          on_knownhosts_opened, self);

  return FALSE;
}

void
hotssh_hostdb_set_entry_host_key_known (HotSshHostDB    *self,
                                        const char      *id,
                                        const char      *keytype,
                                        const char      *key_base64,
                                        const char      *last_ip_address)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GtkTreeIter iter;

  g_return_if_fail (id != NULL);
  g_return_if_fail (keytype != NULL);
  g_return_if_fail (key_base64 != NULL);

  if (!hotssh_hostdb_lookup_by_id (self, id, &iter))
    return;

  gtk_list_store_set (priv->model, &iter,
                      HOTSSH_HOSTDB_COLUMN_HOST_KEY_TYPE, keytype,
                      HOTSSH_HOSTDB_COLUMN_HOST_KEY_BASE64, key_base64,
                      HOTSSH_HOSTDB_COLUMN_HOST_KEY_IP_ADDRESS, last_ip_address,
                      -1);

  g_hash_table_add (priv->add_knownhost_queue, g_strdup (id));

  if (priv->idle_save_knownhosts_id == 0)
    priv->idle_save_knownhosts_id = g_timeout_add_seconds (WRITE_IDLE_SECONDS, idle_write_knownhosts, self);
}

static GFileMonitor *
monitor_file_bind_noerror (HotSshHostDB       *self,
                           GFile              *path,
                           void (*callback) (GFileMonitor *, GFile *, GFile *, GFileMonitorEvent, gpointer user_data))
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GError *local_error = NULL;
  GFileMonitor *ret;

  ret = g_file_monitor (path, 0, NULL, &local_error);
  if (!ret)
    {
      g_error ("Failed to monitor '%s': %s",
               gs_file_get_path_cached (path),
               local_error->message);
    }
  
  g_signal_connect (ret, "changed", G_CALLBACK (callback), self);
  callback (ret, NULL, NULL, 0, self);
  return ret;
}

static void
hotssh_hostdb_init (HotSshHostDB *self)
{
  HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  const char *homedir;
  GError *local_error = NULL;
  gs_free char *knownhosts_path = NULL;
  gs_free char *openssh_path = NULL;
  gs_free char *hotssh_config_dir = NULL;
  gs_free char *hotssh_hostdb_path = NULL;

  priv->model = gtk_list_store_new (HOTSSH_HOSTDB_N_COLUMNS,
                                    G_TYPE_STRING, /* id */
                                    G_TYPE_STRING, /* hostname */
                                    G_TYPE_UINT,   /* port */
                                    G_TYPE_STRING, /* username */
                                    G_TYPE_UINT64, /* last-used */
                                    G_TYPE_BOOLEAN, /* is-known */
                                    G_TYPE_UINT64, /* openssh-knownhost-line */
                                    G_TYPE_STRING, /* last-ip-address */
                                    G_TYPE_STRING, /* host-key-type */
                                    G_TYPE_STRING /* host-key-base64 */
                                    );
  gtk_tree_sortable_set_sort_column_id ((GtkTreeSortable*)priv->model, 
                                        HOTSSH_HOSTDB_COLUMN_LAST_USED,
                                        GTK_SORT_DESCENDING);
  homedir = g_get_home_dir ();
  g_assert (homedir);

  priv->add_knownhost_queue = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     NULL, g_free);

  priv->openssh_knownhosts = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, (GDestroyNotify)g_strfreev);

  openssh_path = g_build_filename (homedir, ".ssh", NULL);
  knownhosts_path = g_build_filename (openssh_path, "known_hosts", NULL);

  priv->openssh_dir = g_file_new_for_path (openssh_path);
  priv->openssh_knownhosts_path = g_file_new_for_path (knownhosts_path);

  hotssh_config_dir = g_build_filename (g_get_user_config_dir (), "hotssh", NULL);
  (void) g_mkdir_with_parents (hotssh_config_dir, 0700);
  hotssh_hostdb_path = g_build_filename (hotssh_config_dir, "hostdb.ini", NULL);
  priv->hotssh_hostdb_path = g_file_new_for_path (hotssh_hostdb_path);

  if (!g_file_query_exists (priv->openssh_dir, NULL))
    {
      if (!g_file_make_directory (priv->openssh_dir, NULL, &local_error))
        {
          g_error ("Failed to make '%s' directory: %s",
                   gs_file_get_path_cached (priv->openssh_dir),
                   local_error->message);
        }
    }
  
  load_hostdb (self);

  priv->knownhosts_monitor = monitor_file_bind_noerror (self, priv->openssh_knownhosts_path,
                                                        on_knownhosts_changed);
}

static void
hotssh_hostdb_dispose (GObject *object)
{
  HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private ((HotSshHostDB*)object);

  g_clear_pointer (&priv->hostdb, g_key_file_unref);
  g_clear_pointer (&priv->openssh_knownhosts_path, g_hash_table_unref);
  g_clear_object (&priv->model);
  g_clear_pointer (&priv->add_knownhost_queue, g_hash_table_unref);

  G_OBJECT_CLASS (hotssh_hostdb_parent_class)->dispose (object);
}

static void
hotssh_hostdb_class_init (HotSshHostDBClass *class)
{
  G_OBJECT_CLASS (class)->dispose = hotssh_hostdb_dispose;
}

HotSshHostDB *
hotssh_hostdb_get_instance (void)
{
  static gsize initialized;
  static HotSshHostDB *instance = NULL;
  
  if (g_once_init_enter (&initialized))
    {
      instance = g_object_new (HOTSSH_TYPE_HOSTDB, NULL);

      g_once_init_leave (&initialized, 1);
    }
  return g_object_ref (instance);
}

GtkTreeModel *
hotssh_hostdb_get_model (HotSshHostDB *self)
{
  HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  return g_object_ref (priv->model);
}
