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

#pragma once

#include <gtk/gtk.h>

#define HOTSSH_TYPE_HOSTDB (hotssh_hostdb_get_type ())
#define HOTSSH_HOSTDB(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), HOTSSH_TYPE_HOSTDB, HotSshHostDB))

typedef struct _HotSshHostDB          HotSshHostDB;
typedef struct _HotSshHostDBClass     HotSshHostDBClass;

enum {
  HOTSSH_HOSTDB_COLUMN_ID,
  HOTSSH_HOSTDB_COLUMN_HOSTNAME,
  HOTSSH_HOSTDB_COLUMN_PORT,
  HOTSSH_HOSTDB_COLUMN_USERNAME,
  HOTSSH_HOSTDB_COLUMN_LAST_USED,
  HOTSSH_HOSTDB_COLUMN_IS_KNOWN,
  HOTSSH_HOSTDB_COLUMN_OPENSSH_KNOWNHOST_LINE,
  HOTSSH_HOSTDB_COLUMN_HOST_KEY_IP_ADDRESS,
  HOTSSH_HOSTDB_COLUMN_HOST_KEY_TYPE,
  HOTSSH_HOSTDB_COLUMN_HOST_KEY_BASE64
};
#define HOTSSH_HOSTDB_N_COLUMNS (HOTSSH_HOSTDB_COLUMN_HOST_KEY_BASE64+1)

GType                   hotssh_hostdb_get_type     (void);
HotSshHostDB           *hotssh_hostdb_get_instance (void);

GtkTreeModel           *hotssh_hostdb_get_model    (HotSshHostDB *self);

void                    hotssh_hostdb_add_entry    (HotSshHostDB    *self,
                                                    const char      *username,
                                                    GNetworkAddress *address,
                                                    char            **out_id);

gboolean               hotssh_hostdb_lookup_by_id (HotSshHostDB   *self,
                                                   const char     *id,
                                                   GtkTreeIter    *out_iter);

void                   hotssh_hostdb_set_entry_basic (HotSshHostDB    *self,
                                                      const char      *id,
                                                      const char      *username,
                                                      GNetworkAddress *address);

void                   hotssh_hostdb_update_last_used    (HotSshHostDB    *self,
                                                          const char      *id);

void                   hotssh_hostdb_set_entry_host_key_known (HotSshHostDB    *self,
                                                               const char      *id,
                                                               const char      *keytype,
                                                               const char      *key_base64,
                                                               const char      *last_ip_address);
