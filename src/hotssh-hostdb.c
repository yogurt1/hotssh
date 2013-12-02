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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>

#include "hotssh-hostdb.h"
#include "hotssh-win.h"
#include "hotssh-prefs.h"

#include "libgsystem.h"

enum {
  HOTSSH_HOSTDB_COLUMN_HOSTNAME,
  HOTSSH_HOSTDB_COLUMN_LAST_USED,
  HOTSSH_HOSTDB_COLUMN_IS_KNOWN,
  HOTSSH_HOSTDB_COLUMN_USERNAME
};

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
  GKeyFile *extradb;
  GFile *openssh_dir;
  GFile *openssh_knownhosts_path;
  GFile *hotssh_extradb;
  GFileMonitor *knownhosts_monitor;
  GFileMonitor *hotssh_extradb_monitor;

  guint idle_save_extradb_id;
  char *new_extradb_contents;
};

G_DEFINE_TYPE_WITH_PRIVATE(HotSshHostDB, hotssh_hostdb, G_TYPE_OBJECT)

static char *
host_group_key_to_host (const char *group)
{
  char *hostname = NULL;
  const char *host_group_prefix = "host \"";
  const char *lastquote;
  const char *hoststart;
  
  if (!g_str_has_prefix (group, host_group_prefix))
    return NULL;

  hoststart = group + strlen (host_group_prefix);
  lastquote = strchr (hoststart, '"');
  if (!lastquote)
    return NULL;
      
  hostname = g_strndup (hoststart, lastquote - hoststart);
  if (!(hostname && hostname[0]))
    {
      g_free (hostname);
      return NULL;
    }

  return hostname;
}

static gboolean
sync_extradb_to_model (HotSshHostDB    *self,
                       const char      *groupname,
                       const char      *hostname,
                       GtkTreeIter     *iter)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  guint64 last_used;
  gs_free char *username = NULL;
  GError *temp_error = NULL;

  last_used = g_key_file_get_uint64 (priv->extradb, groupname, "last-used", &temp_error);
  if (temp_error)
    {
      g_clear_error (&temp_error);
      return FALSE;
    }
  else
    {
      gtk_list_store_set (priv->model, iter,
                          HOTSSH_HOSTDB_COLUMN_LAST_USED, last_used,
                          -1);
    }

  username = g_key_file_get_string (priv->extradb, groupname, "username", NULL);
  if (username && username[0])
    {
      gtk_list_store_set (priv->model, iter,
                          HOTSSH_HOSTDB_COLUMN_USERNAME, username,
                          -1);
    }
  return TRUE;
}

static void
merge_databases (HotSshHostDB *self)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  gs_strfreev char **extradb_groups = NULL;
  gs_unref_hashtable GHashTable *known_hosts
    = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  char **strviter;
  GtkTreeIter iter;
  
  if (!gtk_tree_model_get_iter_first ((GtkTreeModel*)priv->model, &iter))
    return;
  while (TRUE)
    {
      gs_free char *hostname = NULL;
      gs_free char *section = NULL;
      gs_free char *username = NULL;
      gboolean is_known = FALSE;
      gboolean remove = FALSE;

      gtk_tree_model_get ((GtkTreeModel*)priv->model, &iter,
                          HOTSSH_HOSTDB_COLUMN_HOSTNAME, &hostname,
                          HOTSSH_HOSTDB_COLUMN_IS_KNOWN, &is_known,
                          -1);

      if (is_known)
        g_hash_table_add (known_hosts, g_strdup (hostname));

      section = g_strdup_printf ("host \"%s\"", hostname);
      if (!sync_extradb_to_model (self, section, hostname, &iter))
        {
          if (!is_known)
            remove = TRUE;
        }

      if (remove)
        {
          if (!gtk_list_store_remove (priv->model, &iter))
            break;
        }
      else
        {
          if (!gtk_tree_model_iter_next ((GtkTreeModel*)priv->model, &iter))
            break;
        }
    }

  extradb_groups = g_key_file_get_groups (priv->extradb, NULL);
  for (strviter = extradb_groups; strviter && *strviter; strviter++)
    {
      const char *group = *strviter;
      gs_free char *hostname = NULL;

      hostname = host_group_key_to_host (group);
      if (!hostname)
        continue;
      
      if (!g_hash_table_contains (known_hosts, hostname))
        {
          gtk_list_store_append (priv->model, &iter);
          gtk_list_store_set (priv->model, &iter,
                              HOTSSH_HOSTDB_COLUMN_HOSTNAME, hostname,
                              HOTSSH_HOSTDB_COLUMN_IS_KNOWN, FALSE,
                              -1);
          (void) sync_extradb_to_model (self, group, hostname, &iter);
        }
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
  GtkTreeIter modeliter;
  gsize len;
  char *iter;
  char *eol;
  char *carriage;

  if (!g_file_load_contents (priv->openssh_knownhosts_path, NULL,
                             &contents, &len, NULL,
                             &local_error))
    goto out;

  gtk_list_store_clear (priv->model);
  
  iter = contents;
  while (TRUE)
    {
      gs_strfreev char **parts = NULL;
      char *comma;

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
      gtk_list_store_append (priv->model, &modeliter);
      gtk_list_store_set (priv->model, &modeliter,
                          HOTSSH_HOSTDB_COLUMN_HOSTNAME, parts[0],
                          HOTSSH_HOSTDB_COLUMN_LAST_USED, 0,
                          HOTSSH_HOSTDB_COLUMN_IS_KNOWN, TRUE,
                          -1);

    next:
      if (eol)
        iter = eol + 1;
      else
        break;
    }

  g_debug ("Read %d known hosts", gtk_tree_model_iter_n_children ((GtkTreeModel*)priv->model, NULL));

  if (priv->extradb)
    merge_databases (self);
  
 out:
  if (local_error)
    {
      g_debug ("Failed to read '%s': %s", gs_file_get_path_cached (priv->openssh_knownhosts_path),
               local_error->message);
      g_clear_error (&local_error);
    }
}

static void
on_extradb_changed (GFileMonitor        *monitor,
                    GFile               *src,
                    GFile               *other,
                    GFileMonitorEvent    event,
                    gpointer             user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GError *local_error = NULL;

  g_clear_pointer (&priv->extradb, g_key_file_unref);
  priv->extradb = g_key_file_new ();
  
  if (!g_key_file_load_from_file (priv->extradb, gs_file_get_path_cached (priv->hotssh_extradb), 0, &local_error))
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_clear_error (&local_error);
      else
        goto out;
    }

  if (priv->extradb)
    merge_databases (self);
  
 out:
  if (local_error)
    {
      g_debug ("Failed to read '%s': %s", gs_file_get_path_cached (priv->openssh_knownhosts_path),
               local_error->message);
      g_clear_error (&local_error);
    }
}

static gboolean
hostname_to_iter (HotSshHostDB    *self,
                  const char      *hostname,
                  GtkTreeIter     *iter)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);

  if (!gtk_tree_model_get_iter_first ((GtkTreeModel*)priv->model, iter))
    return FALSE;

  while (TRUE)
    {
      gs_free char *model_hostname = NULL;

      gtk_tree_model_get ((GtkTreeModel*)priv->model, iter,
                          HOTSSH_HOSTDB_COLUMN_HOSTNAME, &model_hostname,
                          -1);

      if (g_ascii_strcasecmp (hostname, model_hostname) == 0)
        return TRUE;

      if (!gtk_tree_model_iter_next ((GtkTreeModel*)priv->model, iter))
        break;
    }

  return FALSE;
}

static void
get_or_create_host_in_db (HotSshHostDB        *self,
                          const char          *hostname,
                          GtkTreeIter         *iter)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);

  if (hostname_to_iter (self, hostname, iter))
    return;

  gtk_list_store_append (priv->model, iter);
  gtk_list_store_set (priv->model, iter,
                      HOTSSH_HOSTDB_COLUMN_HOSTNAME, hostname,
                      HOTSSH_HOSTDB_COLUMN_IS_KNOWN, FALSE,
                      -1);
}

static void
on_replace_extradb_contents_complete (GObject                *src,
                                      GAsyncResult           *result,
                                      gpointer                user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GError *local_error = NULL;

  priv->idle_save_extradb_id = 0;

  if (!g_file_replace_contents_finish ((GFile*)src, result, NULL, &local_error))
    goto out;

 out:
  g_clear_pointer (&priv->new_extradb_contents, g_free);
  if (local_error)
    {
      g_warning ("Failed to save '%s': %s",
                 gs_file_get_path_cached (priv->hotssh_extradb),
                 local_error->message);
      g_clear_error (&local_error);
    }
}

static gboolean
idle_save_extradb (gpointer user_data)
{
  HotSshHostDB *self = user_data;
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  gsize len;

  priv->new_extradb_contents = g_key_file_to_data (priv->extradb, &len, NULL);
  g_assert (priv->new_extradb_contents);

  g_file_replace_contents_async (priv->hotssh_extradb, priv->new_extradb_contents, len, NULL,
                                 FALSE, 0, NULL,
                                 on_replace_extradb_contents_complete, self);
  return FALSE;
}

static void
queue_save_extradb (HotSshHostDB    *self)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  if (priv->idle_save_extradb_id > 0)
    return;
  priv->idle_save_extradb_id = g_timeout_add_seconds (5, idle_save_extradb, self);
}

void
hotssh_hostdb_host_used (HotSshHostDB *self,
                         const char   *hostname)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GtkTreeIter iter;
  gs_free char *groupname = g_strdup_printf ("host \"%s\"", hostname);

  get_or_create_host_in_db (self, hostname, &iter);

  g_key_file_set_uint64 (priv->extradb, groupname, "last-used", g_get_real_time () / G_USEC_PER_SEC);
  queue_save_extradb (self);
}

void
hotssh_hostdb_set_username (HotSshHostDB *self,
                            const char   *hostname,
                            const char   *username)
{
  G_GNUC_UNUSED HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  GtkTreeIter iter;
  gs_free char *groupname = g_strdup_printf ("host \"%s\"", hostname);

  get_or_create_host_in_db (self, hostname, &iter);

  g_key_file_set_string (priv->extradb, groupname, "username", username);

  queue_save_extradb (self);
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
  gs_free char *hotssh_extradb_path = NULL;

  priv->model = gtk_list_store_new (4, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_BOOLEAN,
                                    G_TYPE_STRING);
  homedir = g_get_home_dir ();
  g_assert (homedir);

  openssh_path = g_build_filename (homedir, ".ssh", NULL);
  knownhosts_path = g_build_filename (openssh_path, "known_hosts", NULL);

  priv->openssh_dir = g_file_new_for_path (openssh_path);
  priv->openssh_knownhosts_path = g_file_new_for_path (knownhosts_path);

  hotssh_config_dir = g_build_filename (g_get_user_config_dir (), "hotssh", NULL);
  (void) g_mkdir_with_parents (hotssh_config_dir, 0700);
  hotssh_extradb_path = g_build_filename (hotssh_config_dir, "hostdb.ini", NULL);
  priv->hotssh_extradb = g_file_new_for_path (hotssh_extradb_path);

  if (!g_file_query_exists (priv->openssh_dir, NULL))
    {
      if (!g_file_make_directory (priv->openssh_dir, NULL, &local_error))
        {
          g_error ("Failed to make '%s' directory: %s",
                   gs_file_get_path_cached (priv->openssh_dir),
                   local_error->message);
        }
    }
  
  priv->knownhosts_monitor = monitor_file_bind_noerror (self, priv->openssh_knownhosts_path,
                                                        on_knownhosts_changed);

  priv->hotssh_extradb_monitor = monitor_file_bind_noerror (self, priv->hotssh_extradb,
                                                            on_extradb_changed);
}

static void
hotssh_hostdb_dispose (GObject *object)
{
  HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private ((HotSshHostDB*)object);

  g_clear_pointer (&priv->extradb, g_key_file_unref);
  g_clear_object (&priv->model);

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
