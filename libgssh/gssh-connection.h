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

#pragma once

#include "gssh-types.h"

#define GSSH_TYPE_CONNECTION (gssh_connection_get_type ())
#define GSSH_CONNECTION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GSSH_TYPE_CONNECTION, GSshConnection))

typedef struct _GSshConnection         GSshConnection;
typedef struct _GSshConnectionClass    GSshConnectionClass;

typedef enum /*< prefix=GSSH_CONNECTION_STATE >*/
  {
    GSSH_CONNECTION_STATE_DISCONNECTED,
    GSSH_CONNECTION_STATE_CONNECTING,
    GSSH_CONNECTION_STATE_HANDSHAKING,
    GSSH_CONNECTION_STATE_PREAUTH,
    GSSH_CONNECTION_STATE_AUTHENTICATION_REQUIRED,
    GSSH_CONNECTION_STATE_CONNECTED,
    GSSH_CONNECTION_STATE_ERROR
  } GSshConnectionState;

GType                   gssh_connection_get_type     (void);

GSshConnection       *gssh_connection_new (GSocketConnectable  *address,
                                               const char          *username);

GSocketClient          *gssh_connection_get_socket_client (GSshConnection *self);

GSshConnectionState   gssh_connection_get_state (GSshConnection        *self);

void                    gssh_connection_reset (GSshConnection      *self);

void                    gssh_connection_handshake_async (GSshConnection    *self,
                                                           GCancellable        *cancellable,
                                                           GAsyncReadyCallback  callback,
                                                           gpointer             user_data);

gboolean                gssh_connection_handshake_finish (GSshConnection    *self,
                                                            GAsyncResult        *result,
                                                            GError             **error);

GBytes *                gssh_connection_preauth_get_fingerprint_sha1 (GSshConnection *self);

void                    gssh_connection_preauth_continue (GSshConnection *self);

const char*const*       gssh_connection_get_authentication_mechanisms (GSshConnection   *self);

void                    gssh_connection_auth_password_async (GSshConnection    *self,
                                                               const char          *password,
                                                               GCancellable        *cancellable,
                                                               GAsyncReadyCallback  callback,
                                                               gpointer             user_data);

gboolean                gssh_connection_auth_password_finish (GSshConnection    *self,
                                                                GAsyncResult        *result,
                                                                GError             **error);

void                    gssh_connection_open_shell_async (GSshConnection         *self,
                                                            GCancellable             *cancellable,
                                                            GAsyncReadyCallback       callback,
                                                            gpointer                  user_data);

GSshChannel *         gssh_connection_open_shell_finish (GSshConnection         *self,
                                                             GAsyncResult             *result,
                                                             GError                  **error);

