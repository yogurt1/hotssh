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
  GFile *openssh_dir;
  GFile *openssh_knownhosts_path;
  GFile *hotssh_extra;
  GFileMonitor *knownhosts_monitor;
};

G_DEFINE_TYPE_WITH_PRIVATE(HotSshHostDB, hotssh_hostdb, G_TYPE_OBJECT)

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
      gtk_list_store_set (priv->model, &modeliter, 0, parts[0], 1, 0, -1);

    next:
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

static void
hotssh_hostdb_init (HotSshHostDB *self)
{
  HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private (self);
  const char *homedir;
  GError *local_error = NULL;
  gs_free char *knownhosts_path = NULL;
  gs_free char *openssh_path = NULL;

  priv->model = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_UINT64);
  homedir = g_get_home_dir ();
  g_assert (homedir);

  openssh_path = g_build_filename (homedir, ".ssh", NULL);
  knownhosts_path = g_build_filename (openssh_path, "known_hosts", NULL);

  priv->openssh_dir = g_file_new_for_path (openssh_path);
  priv->openssh_knownhosts_path = g_file_new_for_path (knownhosts_path);

  if (!g_file_query_exists (priv->openssh_dir, NULL))
    {
      if (!g_file_make_directory (priv->openssh_dir, NULL, &local_error))
        {
          g_error ("Failed to make '%s' directory: %s",
                   gs_file_get_path_cached (priv->openssh_dir),
                   local_error->message);
        }
    }
  
  priv->knownhosts_monitor = g_file_monitor (priv->openssh_knownhosts_path, 0, NULL,
                                             &local_error);
  if (!priv->knownhosts_monitor)
    {
      g_error ("Failed to monitor '%s': %s",
               gs_file_get_path_cached (priv->openssh_knownhosts_path),
               local_error->message);
    }
  
  g_signal_connect (priv->knownhosts_monitor, "changed",
                    G_CALLBACK (on_knownhosts_changed), self);
  on_knownhosts_changed (priv->knownhosts_monitor, NULL, NULL, 0, self);
}

static void
hotssh_hostdb_dispose (GObject *object)
{
  HotSshHostDBPrivate *priv = hotssh_hostdb_get_instance_private ((HotSshHostDB*)object);

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
