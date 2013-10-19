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

#include "gssh-connection-private.h"
#include "gssh-channel-private.h"
#include "gssh-enum-types.h"

#include "libgsystem.h"

enum {
  PROP_0,
  PROP_ADDRESS,
  PROP_USERNAME,
  PROP_MAINCONTEXT,
  PROP_STATE
};

typedef enum {
  GSSH_CONNECTION_CHANNEL_CREATION_STATE_OPEN_SESSION,
  GSSH_CONNECTION_CHANNEL_CREATION_STATE_REQUEST_PTY,
  GSSH_CONNECTION_CHANNEL_CREATION_STATE_EXEC
} GSshConnectionChannelCreationState;

typedef struct {
  char *exec_command; /* If NULL, then shell */
  GSshConnectionChannelCreationState state;
  ssh_channel libsshchannel;
} GSshConnectionChannelCreationData;

G_DEFINE_TYPE(GSshConnection, gssh_connection, G_TYPE_OBJECT);

void
_gssh_set_error_from_libssh (GError         **error,
                             const char      *prefix,
                             ssh_session      session)
{
  const char *errmsg = ssh_get_error (session);
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", prefix, errmsg);
}

static void
reset_state (GSshConnection               *self)
{
  GHashTableIter hiter;
  gpointer key, value;

  g_assert (self->state == GSSH_CONNECTION_STATE_DISCONNECTED ||
            self->state == GSSH_CONNECTION_STATE_ERROR);
  g_clear_object (&self->handshake_task);
  g_clear_pointer (&self->open_channel_exec_tasks, g_hash_table_unref);
  self->open_channel_exec_tasks = g_hash_table_new_full (NULL, NULL, NULL, g_object_unref);
  g_clear_pointer (&self->channel_tasks, g_hash_table_unref);
  self->channel_tasks = g_hash_table_new_full (NULL, NULL, NULL, g_object_unref);

  if (self->channels)
    {
      g_hash_table_iter_init (&hiter, self->channels);
      while (g_hash_table_iter_next (&hiter, &key, &value))
        {
          GSshChannel *channel = key;
          channel->connection = NULL;
        }
    }
  g_clear_pointer (&self->channels, g_hash_table_unref);
  /* This hash doesn't hold a ref */
  self->channels = g_hash_table_new_full (NULL, NULL, NULL, NULL);

  g_clear_pointer (&self->remote_hostkey_sha1, g_bytes_unref);
  g_clear_error (&self->cached_error);
  g_clear_object (&self->socket);
  if (self->socket_source)
    g_source_destroy (self->socket_source);
  g_clear_pointer (&self->socket_source, g_source_unref);
  g_clear_pointer (&self->authschemes, g_ptr_array_unref);
}

static void
state_transition (GSshConnection                *self,
		  GSshConnectionState            new_state)
{
  if (new_state == self->state)
    return;

  g_debug ("STATE: %d => %d", self->state, new_state);

  self->state = new_state;
  g_object_notify ((GObject*)self, "state");

  if (self->state == GSSH_CONNECTION_STATE_DISCONNECTED ||
      self->state == GSSH_CONNECTION_STATE_ERROR)
    reset_state (self);
}

static void
state_transition_take_error (GSshConnection       *self,
                             GError                 *error)
{
  g_debug ("caught error: %s", error->message);

  if (self->handshake_task)
    {
      g_task_return_error (self->handshake_task, error);
      g_clear_object (&self->handshake_task);
    }
  else
    {
      GHashTableIter hiter;
      gpointer hkey, hvalue;
      gboolean had_active_channel_task = FALSE;

      g_hash_table_iter_init (&hiter, self->channel_tasks);
      while (g_hash_table_iter_next (&hiter, &hkey, &hvalue))
        {
          GTask *channeltask = hkey;
          GError *error_copy = g_error_copy (error);

          had_active_channel_task = TRUE;
          g_task_return_error (channeltask, error_copy);
        }

      /* Keep it around for the next async call */
      if (!had_active_channel_task)
        self->cached_error = error;
      else
        g_error_free (error);
    }
  state_transition (self, GSSH_CONNECTION_STATE_ERROR);
}

static gboolean
on_socket_ready (GSocket *socket,
		 GIOCondition condition,
		 gpointer user_data);

static void
recalculate_socket_state (GSshConnection   *self)
{
  int status = ssh_get_poll_flags (self->session);
  guint block_inbound = (status & SSH_READ_PENDING) > 0;
  guint block_outbound = (status & SSH_WRITE_PENDING) > 0;
  GIOCondition conditions = G_IO_HUP | G_IO_ERR;

  if (self->paused)
    block_inbound = block_outbound = 0;

  if (block_inbound == self->select_inbound &&
      block_outbound == self->select_outbound &&
      self->socket_source != NULL)
    return;

  self->select_inbound = block_inbound;
  if (self->select_inbound)
    conditions |= G_IO_IN;
  self->select_outbound = block_outbound;
  if (self->select_outbound)
    conditions |= G_IO_OUT;
  
  if (self->socket_source)
    { 
      g_source_destroy (self->socket_source);
      g_source_unref (self->socket_source);
    }
  g_printerr ("socket will select on inbound: %d outbound: %d\n",
	      self->select_inbound, self->select_outbound);
  self->socket_source = g_socket_create_source (self->socket,
						conditions,
						self->cancellable);
  g_source_set_callback (self->socket_source, (GSourceFunc)on_socket_ready, self, NULL);
  g_source_attach (self->socket_source, NULL);
}

static void
process_channel_reads (GSshConnection   *self)
{
  GHashTableIter hiter;
  gpointer hkey, hvalue;

  g_hash_table_iter_init (&hiter, self->channels);
  while (g_hash_table_iter_next (&hiter, &hkey, &hvalue))
    {
      GSshChannel *channel = hkey;
      if (channel->input_stream)
        _gssh_channel_input_stream_iteration (channel->input_stream);
    }
}

static void
process_channel_writes (GSshConnection   *self)
{
  GHashTableIter hiter;
  gpointer hkey, hvalue;

  g_hash_table_iter_init (&hiter, self->channels);
  while (g_hash_table_iter_next (&hiter, &hkey, &hvalue))
    {
      GSshChannel *channel = hkey;
      if (channel->output_stream)
        _gssh_channel_output_stream_iteration (channel->output_stream);
    }
}

static void
process_channels (GSshConnection   *self,
                  GIOCondition        condition)
{
  GHashTableIter hiter;
  gpointer hkey, hvalue;
  int rc;
  GError *local_error = NULL;

  /* Channel I/O */
  if ((condition & G_IO_IN) > 0)
    process_channel_reads (self);
  if ((condition & G_IO_OUT) > 0)
    process_channel_writes (self);
  
  /* And new channel requests */
  g_hash_table_iter_init (&hiter, self->open_channel_exec_tasks);
  while (g_hash_table_iter_next (&hiter, &hkey, &hvalue))
    {
      GTask *task = hkey;
      GSshConnectionChannelCreationData *data = g_task_get_task_data (task);

      switch (data->state)
        {
        case GSSH_CONNECTION_CHANNEL_CREATION_STATE_OPEN_SESSION:
          {
            if (!data->libsshchannel)
              data->libsshchannel = ssh_channel_new (self->session);
            g_assert (data->libsshchannel);  /* Should only fail on OOM */
            rc = ssh_channel_open_session (data->libsshchannel);
            if (rc == SSH_OK)
              ;
            else if (rc == SSH_AGAIN)
              {
                break;
              }
            else
              {
                _gssh_set_error_from_libssh (&local_error, "Failed to open session: ", self->session);
                g_task_return_error (task, local_error);
                g_hash_table_iter_remove (&hiter);
                break;
              }
            data->state = GSSH_CONNECTION_CHANNEL_CREATION_STATE_REQUEST_PTY;
            /* Fall through */
          }
        case GSSH_CONNECTION_CHANNEL_CREATION_STATE_REQUEST_PTY:
          {
            rc = ssh_channel_request_pty (data->libsshchannel);
            if (rc == SSH_OK)
              {
                data->state = GSSH_CONNECTION_CHANNEL_CREATION_STATE_EXEC;
              }
            else if (rc == SSH_AGAIN)
              break;
            else
              {
                _gssh_set_error_from_libssh (&local_error, "Failed to open session: ", self->session);
                g_task_return_error (task, local_error);
                g_hash_table_iter_remove (&hiter);
                break;
              }
            /* Fall through */
          } 
        case GSSH_CONNECTION_CHANNEL_CREATION_STATE_EXEC:
          {
            if (data->exec_command == NULL)
              rc = ssh_channel_request_shell (data->libsshchannel);
            else
              rc = ssh_channel_request_exec (data->libsshchannel,
                                             data->exec_command);
            if (rc == SSH_OK)
              {
                GSshChannel *new_channel; 

                g_assert (rc == 0);

                new_channel = _gssh_channel_new (self, TRUE, data->libsshchannel);
                g_hash_table_insert (self->channels, new_channel, new_channel);

                g_task_return_pointer (task, new_channel, g_object_unref);
                g_hash_table_iter_remove (&hiter);
                break;
              }
            else if (rc == SSH_AGAIN)
              break;
            else
              {
                _gssh_set_error_from_libssh (&local_error, "Failed to exec: ", self->session);
                g_task_return_error (task, local_error);
                g_hash_table_iter_remove (&hiter);
                break;
              }
          }
        }
    }
}

static gboolean
set_hostkey_sha1 (GSshConnection           *self,
                  GError                  **error)
{
  gboolean ret = FALSE;
  int rc;
  ssh_key key = NULL;
  char *key_b64;
  guint8 *key_raw;
  gsize key_len;
  guint8 sha1buf[20];
  gsize sha1len = sizeof (sha1buf);
  GChecksum *csum;

  rc = ssh_get_publickey (self->session, &key);
  if (rc != SSH_OK)
    {
      _gssh_set_error_from_libssh (error, "Failed to get public key", self->session);
      goto out;
    }

  rc = ssh_pki_export_pubkey_base64 (key, &key_b64);
  if (rc != SSH_OK)
    {
      _gssh_set_error_from_libssh (error, "Failed to export public key", self->session);
      goto out;
    }

  key_raw = g_base64_decode (key_b64, &key_len);
  g_assert (key_raw);

  csum = g_checksum_new (G_CHECKSUM_SHA1);
  g_checksum_update (csum, key_raw, key_len);
  g_checksum_get_digest (csum, sha1buf, &sha1len);
  g_assert (sha1len == sizeof (sha1buf));

  self->remote_hostkey_sha1 = g_bytes_new (sha1buf, sha1len);

  ret = TRUE;
 out:
  if (key)
    ssh_key_free (key);
  return ret;
}

static void
gssh_connection_iteration_internal (GSshConnection   *self,
                                    GIOCondition        condition)
{
  GError *local_error = NULL;
  GError **error = &local_error;
  int rc;

 repeat:

  switch (self->state)
    {
    case GSSH_CONNECTION_STATE_DISCONNECTED:
      g_assert_not_reached ();
      break;
    case GSSH_CONNECTION_STATE_CONNECTING:
      g_assert_not_reached ();
      break;
    case GSSH_CONNECTION_STATE_HANDSHAKING:
      {
        rc = ssh_connect (self->session);
        if (rc == SSH_OK)
          {
            if (!set_hostkey_sha1 (self, error))
              {
                g_task_return_error (self->handshake_task, local_error);
                g_clear_object (&self->handshake_task);
                return;
              }
          }
        else if (rc == SSH_AGAIN)
          {
            return;
          }
        else
          {
            _gssh_set_error_from_libssh (error, "Failed to handshake SSH2 session", self->session);
            g_task_return_error (self->handshake_task, local_error);
            g_clear_object (&self->handshake_task);
            return;
          }
        
        g_task_return_boolean (self->handshake_task, TRUE);
        g_clear_object (&self->handshake_task);

        state_transition (self, GSSH_CONNECTION_STATE_PREAUTH);
        /* Fall through */
      }
    case GSSH_CONNECTION_STATE_PREAUTH:
      {
        int method;
        if (!self->tried_userauth_none)
          {
            /* Now try the NONE authentication; if it succeeds we jump
             * directly to authenticated.
             */
            rc = ssh_userauth_none (self->session, NULL);
            if (rc == SSH_AUTH_AGAIN)
              {
                return;
              }
            else if (rc == SSH_AUTH_SUCCESS)
              {
                state_transition (self, GSSH_CONNECTION_STATE_CONNECTED);
                goto repeat;
              }
            else if (rc == SSH_AUTH_ERROR)
              {
                _gssh_set_error_from_libssh (error, "NONE authentication failed", self->session);
                g_task_return_error (self->handshake_task, local_error);
                g_clear_object (&self->handshake_task);
                return;
              }
            else
              {
                g_assert (rc == SSH_AUTH_DENIED);
                self->tried_userauth_none = TRUE;
              }
          }

        g_clear_pointer (&self->authschemes, g_ptr_array_unref);
        self->authschemes = g_ptr_array_new ();
        method = ssh_userauth_list (self->session, NULL);

        if (method & SSH_AUTH_METHOD_PASSWORD)
          g_ptr_array_add (self->authschemes, "password");
        if (method & SSH_AUTH_METHOD_GSSAPI_MIC)
          g_ptr_array_add (self->authschemes, "gssapi-mic");
        if (method & SSH_AUTH_METHOD_PUBLICKEY)
          g_ptr_array_add (self->authschemes, "publickey");
        if (method & SSH_AUTH_METHOD_HOSTBASED)
          g_ptr_array_add (self->authschemes, "hostbased");
        if (method & SSH_AUTH_METHOD_INTERACTIVE)
          g_ptr_array_add (self->authschemes, "keyboard-interactive");

        g_ptr_array_add (self->authschemes, NULL);

        if (!self->preauth_continue)
          {
            self->paused = TRUE;
            break;
          }

        state_transition (self, GSSH_CONNECTION_STATE_AUTHENTICATION_REQUIRED);
        /* Fall through */
      }
    case GSSH_CONNECTION_STATE_AUTHENTICATION_REQUIRED:
      {
        int method;

        method = ssh_userauth_list (self->session, NULL);
        /* User should have connected to notify:: state and
         * watch for AUTHENTICATION_REQUIRED, then call
         * gssh_connection_auth_password_async().
         */
        if (self->auth_task != NULL)
          {
            if ((method & SSH_AUTH_METHOD_PASSWORD) && self->password)
              {
                rc = ssh_userauth_password (self->session, NULL, self->password);
                if (rc == SSH_AUTH_AGAIN)
                  {
                    return;
                  }
                else if (rc == SSH_AUTH_ERROR)
                  {
                    _gssh_set_error_from_libssh (error, "Failed to password auth", self->session);
                    g_task_return_error (self->auth_task, local_error);
                    g_clear_object (&self->auth_task);
                  }
                else if (rc == SSH_AUTH_DENIED)
                  {
                    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         "Authentication failed");
                    g_task_return_error (self->auth_task, local_error);
                  }
                else if (rc == SSH_AUTH_PARTIAL)
                  {
                    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                                         "Need to continue authentication");
                    g_task_return_error (self->auth_task, local_error);
                  }
                else
                  {
                    g_task_return_boolean (self->auth_task, TRUE);
                    g_clear_object (&self->auth_task);
                    state_transition (self, GSSH_CONNECTION_STATE_CONNECTED);
                    goto repeat;
                  }
              }
            else
              {
                g_assert_not_reached ();
              }
          }
        break;
      }
    case GSSH_CONNECTION_STATE_CONNECTED:
      process_channels (self, condition);
      break;
    case GSSH_CONNECTION_STATE_ERROR:
      break;
    }
}

static void
gssh_connection_iteration (GSshConnection   *self,
                           GIOCondition        condition)
{
  gssh_connection_iteration_internal (self, condition);
  recalculate_socket_state (self);
}

static void
gssh_connection_iteration_default (GSshConnection   *self)
{
  /* This is a bit of a hack, but eh... we'll just get EAGAIN */
  gssh_connection_iteration (self, G_IO_IN | G_IO_OUT);
}

static gboolean
on_socket_ready (GSocket *socket,
		 GIOCondition condition,
		 gpointer user_data)
{
  GSshConnection *self = user_data;

  if (condition & (G_IO_ERR | G_IO_HUP))
    {
      GError *local_error = NULL;
      g_set_error_literal (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "I/O error");
      state_transition_take_error (self, local_error);
      return FALSE;
    }

  g_debug ("socket ready: state %d", self->state);

  gssh_connection_iteration (self, condition);

  return TRUE;
}

static void
on_socket_client_connected (GObject         *src,
			    GAsyncResult    *result,
			    gpointer         user_data)
{
  GSshConnection *self = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  int fd;

  g_assert (src == (GObject*)self->socket_client);

  g_clear_object (&self->socketconn);
  self->socketconn = g_socket_client_connect_finish (self->socket_client, result, error);
  if (!self->socketconn)
    goto out;

  self->socket = g_socket_connection_get_socket (self->socketconn);

  self->session = ssh_new ();
  ssh_set_log_level (SSH_LOG_FUNCTIONS);
  if (!self->session)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Failed to initialize SSH2 session");
      goto out;
    }
  
  fd = g_socket_get_fd (self->socket);

  ssh_set_blocking (self->session, 0);
  ssh_options_set (self->session, SSH_OPTIONS_FD, &fd);
  ssh_options_set (self->session, SSH_OPTIONS_USER, self->username);

  state_transition (self, GSSH_CONNECTION_STATE_HANDSHAKING);

  gssh_connection_iteration_default (self);

  return;
out:
  if (local_error)
    state_transition_take_error (self, local_error);
}

GSshConnectionState
gssh_connection_get_state (GSshConnection        *self)
{
  return self->state;
}

/**
 * gssh_connection_preauth_get_fingerprint_sha1:
 * @self: Self
 *
 * Returns: (transfer none): 20 bytes for the remote host's SHA1 fingerprint
 */
GBytes *
gssh_connection_preauth_get_fingerprint_sha1 (GSshConnection *self)
{
  return self->remote_hostkey_sha1;
}

/**
 * gssh_connection_preauth_continue:
 * @self: Self
 *
 * Call this function after having verified the host key fingerprint
 * to continue the connection.
 */
void
gssh_connection_preauth_continue (GSshConnection *self)
{
  g_return_if_fail (self->state == GSSH_CONNECTION_STATE_PREAUTH);
  g_return_if_fail (!self->preauth_continue);
  self->preauth_continue = TRUE;
  self->paused = FALSE;
  gssh_connection_iteration_default (self);
}

const char*const*
gssh_connection_get_authentication_mechanisms (GSshConnection   *self)
{
  return (const char *const*)self->authschemes->pdata;
}

void
gssh_connection_auth_password_async (GSshConnection    *self,
                                       const char          *password,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
  g_return_if_fail (self->state == GSSH_CONNECTION_STATE_AUTHENTICATION_REQUIRED);
  g_return_if_fail (self->auth_task == NULL);

  if (self->password)
    g_free (self->password);
  self->password = g_strdup (password);

  self->auth_task = g_task_new (self, cancellable, callback, user_data);
  
  gssh_connection_iteration_default (self);
}

gboolean
gssh_connection_auth_password_finish (GSshConnection    *self,
                                        GAsyncResult        *result,
                                        GError             **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

GSocketClient *
gssh_connection_get_socket_client (GSshConnection *self)
{
  return self->socket_client;
}

void
gssh_connection_reset (GSshConnection      *self)
{
  state_transition (self, GSSH_CONNECTION_STATE_DISCONNECTED);
}

void
gssh_connection_handshake_async (GSshConnection    *self,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  g_return_if_fail (self->state == GSSH_CONNECTION_STATE_DISCONNECTED);

  state_transition (self, GSSH_CONNECTION_STATE_CONNECTING);

  self->handshake_task = g_task_new (self, cancellable, callback, user_data);
  g_socket_client_connect_async (self->socket_client, self->address, cancellable,
				 on_socket_client_connected, self);
}

gboolean
gssh_connection_handshake_finish (GSshConnection    *self,
                                    GAsyncResult        *result,
                                    GError             **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
channel_creation_data_free (gpointer datap)
{
  GSshConnectionChannelCreationData *data = datap;
  g_free (data->exec_command);
  g_free (data);
}

void
gssh_connection_open_shell_async (GSshConnection         *self,
                                    GCancellable             *cancellable,
                                    GAsyncReadyCallback       callback,
                                    gpointer                  user_data)
{
  GTask *task;
  GSshConnectionChannelCreationData *data =
    g_new0 (GSshConnectionChannelCreationData, 1);

  /* Don't do anything to data, we have no exec command */

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, data, channel_creation_data_free);
  g_hash_table_add (self->open_channel_exec_tasks, task);

  gssh_connection_iteration_default (self);
}

GSshChannel *
gssh_connection_open_shell_finish (GSshConnection         *self,
                                     GAsyncResult             *result,
                                     GError                  **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
gssh_connection_init (GSshConnection *self)
{
  self->socket_client = g_socket_client_new ();
  reset_state (self);
}

static void
gssh_connection_dispose (GObject *object)
{
  GSshConnection *self = GSSH_CONNECTION (object);

  state_transition (self, GSSH_CONNECTION_STATE_DISCONNECTED);

  G_OBJECT_CLASS (gssh_connection_parent_class)->dispose (object);
}

static void
gssh_connection_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GSshConnection *self = GSSH_CONNECTION (object);

  switch (prop_id)
    {
    case PROP_ADDRESS:
      g_value_set_object (value, self->address);
      break;

    case PROP_USERNAME:
      g_value_set_string (value, self->username);
      break;

    case PROP_STATE:
      g_value_set_enum (value, self->state);
      break;

    case PROP_MAINCONTEXT:
      g_value_set_boxed (value, self->maincontext);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gssh_connection_set_property (GObject    *object,
                                guint       prop_id,
                                const GValue     *value,
                                GParamSpec *pspec)
{
  GSshConnection *self = GSSH_CONNECTION (object);

  switch (prop_id)
    {
    case PROP_ADDRESS:
      self->address = g_value_dup_object (value);
      break;

    case PROP_USERNAME:
      self->username = g_value_dup_string (value);
      break;

    case PROP_STATE:
      self->state = g_value_get_enum (value);
      break;

    case PROP_MAINCONTEXT:
      self->maincontext = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gssh_connection_class_init (GSshConnectionClass *class)
{
  G_OBJECT_CLASS (class)->dispose = gssh_connection_dispose;
  G_OBJECT_CLASS (class)->get_property = gssh_connection_get_property;
  G_OBJECT_CLASS (class)->set_property = gssh_connection_set_property;

  g_object_class_install_property (G_OBJECT_CLASS (class), PROP_ADDRESS,
				   g_param_spec_object ("address", "", "",
                                                        G_TYPE_SOCKET_CONNECTABLE,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (G_OBJECT_CLASS (class), PROP_USERNAME,
				   g_param_spec_string ("username", "", "",
                                                        NULL,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (G_OBJECT_CLASS (class), PROP_STATE,
				   g_param_spec_enum ("state", "", "",
                                                      GSSH_TYPE_CONNECTION_STATE,
                                                      GSSH_CONNECTION_STATE_DISCONNECTED,
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (G_OBJECT_CLASS (class), PROP_MAINCONTEXT,
				   g_param_spec_boxed ("maincontext", "", "",
                                                      G_TYPE_MAIN_CONTEXT,
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

}

GSshConnection *
gssh_connection_new (GSocketConnectable   *address,
                       const char           *username)
{
  return g_object_new (GSSH_TYPE_CONNECTION,
                       "address", address,
                       "username", username,
                       "maincontext", g_main_context_get_thread_default (),
                       NULL);
}
