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

#include "hotssh-tab.h"
#include "hotssh-hostdb.h"
#include "hotssh-password-interaction.h"
#include "gssh.h"

#include "libgsystem.h"

#include <vte/vte.h>
#include <stdio.h>
#include <string.h>

#include <glib/gi18n.h>

static const GSshConnectionAuthMechanism default_authentication_order[] = {
  GSSH_CONNECTION_AUTH_MECHANISM_PUBLICKEY,
  /*  GSSH_CONNECTION_AUTH_MECHANISM_GSSAPI_MIC,  Seems broken */
  GSSH_CONNECTION_AUTH_MECHANISM_PASSWORD
};

enum {
  PROP_0,
  PROP_HOSTNAME
};

struct _HotSshTab
{
  GtkNotebook parent;
};

struct _HotSshTabClass
{
  GtkNotebookClass parent_class;
};

typedef struct _HotSshTabPrivate HotSshTabPrivate;

typedef enum {
  HOTSSH_TAB_PAGE_NEW_CONNECTION,
  HOTSSH_TAB_PAGE_LIST_CONNECTIONS,
  HOTSSH_TAB_PAGE_CONNECTING,
  HOTSSH_TAB_PAGE_ERROR,
  HOTSSH_TAB_PAGE_HOSTKEY,
  HOTSSH_TAB_PAGE_PASSWORD,
  HOTSSH_TAB_PAGE_TERMINAL
} HotSshTabPage;

struct _HotSshTabPrivate
{
  GSettings *settings;
  GtkWidget *terminal;
  HotSshPasswordInteraction *password_interaction;

  /* Bound via template */
  GtkWidget *host_entry;
  GtkWidget *username_entry;
  GtkWidget *create_and_connect_button;
  GtkWidget *add_new_connection_button;
  GtkWidget *connection_text_container;
  GtkWidget *connection_text;
  GtkWidget *error_text;
  GtkWidget *error_disconnect;
  GtkWidget *password_container;
  GtkWidget *password_entry;
  GtkWidget *password_submit;
  GtkWidget *connect_cancel_button;
  GtkWidget *auth_cancel_button;
  GtkWidget *hostkey_container;
  GtkWidget *hostkey_fingerprint_label;
  GtkWidget *approve_hostkey_button;
  GtkWidget *disapprove_hostkey_button;
  GtkWidget *terminal_box;
  GtkWidget *terminal_vscrollbar;
  GtkWidget *connections_treeview;
  GtkWidget *hostname_column;
  GtkWidget *hostname_renderer;
  GtkWidget *lastused_column;
  GtkWidget *lastused_renderer;
  GtkWidget *known_column;
  GtkWidget *known_renderer;

  /* State */
  HotSshTabPage active_page;
  guint authmechanism_index;

  gboolean indisposed;
  char *connection_id;
  char *hostname;
  char *username;
  GtkEntryCompletion *host_completion;
  GSocketConnectable *address;
  GSshConnection *connection;
  GSshChannel *channel;

  gboolean need_pty_size_request;
  gboolean sent_pty_size_request;
  gboolean awaiting_password_entry;
  gboolean submitted_password;
  gboolean have_outstanding_write;
  gboolean have_outstanding_auth;
  GQueue write_queue;

  GCancellable *cancellable;
};

G_DEFINE_TYPE_WITH_PRIVATE(HotSshTab, hotssh_tab, GTK_TYPE_NOTEBOOK);

static void
on_negotiate_complete (GObject             *src,
                       GAsyncResult        *result,
                       gpointer             user_data);

static void
set_status (HotSshTab     *self,
	    const char       *text)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  g_debug ("status: %s", text);
  gtk_label_set_text ((GtkLabel*)priv->connection_text, text);
}

static void
set_status_printf (HotSshTab  *self,
                   const char *format,
                   ...) G_GNUC_PRINTF (2,3);

static void
set_status_printf (HotSshTab  *self,
                   const char *format,
                   ...)
{
  gs_free char *msg;
  va_list args;

  va_start (args, format);
  msg = g_strdup_vprintf (format, args);
  va_end (args);

  set_status (self, msg);
}

static void
state_reset_for_new_connection (HotSshTab                *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  g_debug ("reset state");
  g_clear_pointer (&priv->connection_id, g_free);
  g_clear_object (&priv->address);
  g_clear_object (&priv->connection);
  g_clear_object (&priv->cancellable);
  if (!priv->indisposed)
    {
      g_object_notify ((GObject*)self, "hostname");
      vte_terminal_reset ((VteTerminal*)priv->terminal, TRUE, TRUE);
      gtk_entry_set_text ((GtkEntry*)priv->password_entry, "");
      gtk_label_set_text ((GtkLabel*)priv->connection_text, "");
      gtk_widget_set_sensitive (priv->password_container, TRUE);
      priv->awaiting_password_entry = priv->submitted_password = FALSE;
    }
  g_debug ("reset state done");
}

static void
page_transition (HotSshTab        *self,
		 HotSshTabPage     new_page)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  if (new_page == priv->active_page)
    return;

  g_debug ("PAGE: %u => %u", priv->active_page, new_page);
  priv->active_page = new_page;

  if (priv->active_page == HOTSSH_TAB_PAGE_NEW_CONNECTION
      || priv->active_page == HOTSSH_TAB_PAGE_LIST_CONNECTIONS
      || priv->active_page == HOTSSH_TAB_PAGE_ERROR)
    state_reset_for_new_connection (self);

  gtk_notebook_set_current_page ((GtkNotebook*)self, new_page);

  if (priv->active_page == HOTSSH_TAB_PAGE_TERMINAL)
    gtk_widget_grab_focus ((GtkWidget*)priv->terminal);
}

static void
page_transition_take_error (HotSshTab               *self,
			    GError                     *error)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  g_debug ("Caught error: %s", error->message);
  page_transition (self, HOTSSH_TAB_PAGE_ERROR);
  gtk_label_set_text ((GtkLabel*)priv->error_text, error->message);
  g_error_free (error);
}

static void
on_istream_read_complete (GObject           *src,
			  GAsyncResult      *res,
			  gpointer           user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  gs_unref_bytes GBytes *result = NULL;
  GError *local_error = NULL;
  const guint8 *buf;
  gsize len;

  result = g_input_stream_read_bytes_finish ((GInputStream*)src, res, &local_error);
  if (!result)
    goto out;

  buf = g_bytes_get_data (result, &len);
  g_debug ("read %u bytes", (guint)len);
  
  vte_terminal_feed ((VteTerminal*)priv->terminal, (char*)buf, len);

  g_input_stream_read_bytes_async ((GInputStream*)src, 8192, G_PRIORITY_DEFAULT,
				   priv->cancellable, on_istream_read_complete, self);

 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
on_open_shell_complete (GObject           *src,
			GAsyncResult      *res,
			gpointer           user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GError *local_error = NULL;
  GInputStream *istream;

  g_debug ("open shell complete");

  priv->channel = gssh_connection_open_shell_finish ((GSshConnection*)src,
						       res, &local_error);
  if (!priv->channel)
    goto out;

  page_transition (self, HOTSSH_TAB_PAGE_TERMINAL);

  istream = g_io_stream_get_input_stream ((GIOStream*)priv->channel);

  g_input_stream_read_bytes_async (istream, 8192, G_PRIORITY_DEFAULT,
				   priv->cancellable, on_istream_read_complete, self);
  
 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
iterate_authentication_modes (HotSshTab          *self);

static void
on_connection_state_notify (GSshConnection   *conn,
			    GParamSpec         *pspec,
			    gpointer            user_data)
{
  HotSshTab *self = HOTSSH_TAB (user_data);
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GSshConnectionState new_state = gssh_connection_get_state (conn);

  switch (new_state)
    {
    case GSSH_CONNECTION_STATE_DISCONNECTED:
    case GSSH_CONNECTION_STATE_CONNECTING:
    case GSSH_CONNECTION_STATE_HANDSHAKING:
    case GSSH_CONNECTION_STATE_PREAUTH:
    case GSSH_CONNECTION_STATE_NEGOTIATE_AUTH:
      break;
    case GSSH_CONNECTION_STATE_AUTHENTICATION_REQUIRED:
      break;
    case GSSH_CONNECTION_STATE_ERROR:
      g_debug ("connection in state ERROR!");
      break;
    case GSSH_CONNECTION_STATE_CONNECTED:
      gssh_connection_open_shell_async (priv->connection, priv->cancellable,
					  on_open_shell_complete, self);
      break;
    }
}

static void
on_auth_complete (GObject                *src,
                  GAsyncResult           *res,
                  gpointer                user_data)
{
  HotSshTab *self = HOTSSH_TAB (user_data);
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GError *local_error = NULL;

  priv->have_outstanding_auth = FALSE;

  if (!gssh_connection_auth_finish ((GSshConnection*)src, res, &local_error))
    goto out;

  set_status (self, _("Authenticated, requesting channel…"));

  g_debug ("auth complete");

 out:
  if (local_error)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        {
          g_debug ("Authentication mechanism '%s' denied",
                   gssh_connection_auth_mechanism_to_string (default_authentication_order[priv->authmechanism_index]));
          g_clear_error (&local_error);
          priv->authmechanism_index++;
          iterate_authentication_modes (self);
        }
      else
        page_transition_take_error (self, local_error);
    }
}

static gboolean
have_mechanism (guint                        *available,
                guint                         n_available,
                GSshConnectionAuthMechanism   mech)
{
  guint i;
  for (i = 0; i < n_available; i++)
    if (available[i] == mech)
      return TRUE;
  return FALSE;
}

static void
iterate_authentication_modes (HotSshTab          *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  guint n_mechanisms;
  guint *available_authmechanisms;
  GError *local_error = NULL;

  gssh_connection_get_authentication_mechanisms (priv->connection,
                                                 &available_authmechanisms,
                                                 &n_mechanisms);

  if (priv->have_outstanding_auth)
    {
      return;
    }

  if (priv->awaiting_password_entry &&
      !priv->submitted_password)
    {
      return;
    }

  while (priv->authmechanism_index < G_N_ELEMENTS (default_authentication_order) &&
         !have_mechanism (available_authmechanisms, n_mechanisms,
                          default_authentication_order[priv->authmechanism_index]))
    {
      priv->authmechanism_index++;
    }

  if (priv->authmechanism_index >= G_N_ELEMENTS (default_authentication_order))
    {
      g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("No more authentication mechanisms available"));
      goto out;
    }
  else
    {
      GSshConnectionAuthMechanism mech =
        default_authentication_order[priv->authmechanism_index];
      gboolean is_password = mech == GSSH_CONNECTION_AUTH_MECHANISM_PASSWORD;
      gs_free char *authmsg =
        g_strdup_printf (_("Requesting authentication via '%s'"),
                         gssh_connection_auth_mechanism_to_string (mech));
      /* Ugly gross hack until we have separate auth pages */
      set_status (self, authmsg);
      gtk_widget_set_sensitive (priv->password_container,
                                is_password);
      if (is_password)
        {
          priv->awaiting_password_entry = TRUE;
          if (!priv->submitted_password)
            {
              page_transition (self, HOTSSH_TAB_PAGE_PASSWORD);
              return;
            }
        }
      else
        {
          set_status (self, authmsg);
        }
        
      gssh_connection_auth_async (priv->connection,
                                  mech,
                                  priv->cancellable,
                                  on_auth_complete, self);
      priv->have_outstanding_auth = TRUE;
    }
  
 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
handle_unknown_hostkey (HotSshTab *self,
                        const char *connected_hostkey_type,
                        const char *connected_hostkey_sha1)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  gtk_label_set_text ((GtkLabel*)priv->hostkey_fingerprint_label,
		      connected_hostkey_sha1);
  page_transition (self, HOTSSH_TAB_PAGE_HOSTKEY);
}

static void
verify_hostkey (HotSshTab              *self,
                const char             *connected_hostkey_type,
                const char             *connected_hostkey_base64,
                const char             *saved_hostkey_type,
                const char             *saved_hostkey_base64)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  gs_free char *errdetails = NULL;

  if (strcmp (connected_hostkey_type, saved_hostkey_type) != 0)
    {
      errdetails = g_strdup_printf (_("The remote host key type has changed; it was previously \"%s\", now \"%s\""),
                                    saved_hostkey_type, connected_hostkey_type);
    }
  else if (strcmp (connected_hostkey_base64, saved_hostkey_base64) != 0)
    {
      errdetails = g_strdup (_("The remote host key has changed"));
    }
    
  if (errdetails)
    {
      GError *local_error = NULL;

      g_set_error (&local_error, G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   _("Error: The host credentials for \"%s\" (%s) do not match the previously "
                     "saved credentials.  This may represent an attempt by a malicious party to "
                     "intercept communication; however, it is also possible that the host key was changed "
                     "by a system administrator.\n\nDetails: %s"),
                   priv->hostname,
                   g_network_address_get_hostname ((GNetworkAddress*)priv->address),
                   errdetails);
      page_transition_take_error (self, local_error);
    }
  else
    {
      g_debug ("Remote host key matches");
      page_transition (self, HOTSSH_TAB_PAGE_CONNECTING);
      set_status (self, _("Negotiating authentication…"));

      gssh_connection_negotiate_async (priv->connection, priv->cancellable,
                                       on_negotiate_complete, self);
    }
}

static void
on_connection_handshake (GObject         *object,
			 GAsyncResult    *result,
			 gpointer         user_data)
{
  HotSshTab *self = HOTSSH_TAB (user_data);
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GError *local_error = NULL;
  GError **error = &local_error;
  gs_free char *saved_hostkey_type = NULL;
  gs_free char *saved_hostkey_base64 = NULL;
  gs_unref_object GtkTreeModel *model = NULL;
  gs_free char *connected_hostkey_type = NULL;
  gs_free char *connected_hostkey_sha1_text = NULL;
  gs_free char *connected_hostkey_base64 = NULL;
  GtkTreeIter iter;

  model = hotssh_hostdb_get_model (hotssh_hostdb_get_instance ());

  if (!gssh_connection_handshake_finish ((GSshConnection*)object, result, error))
    goto out;

  gssh_connection_preauth_get_host_key (priv->connection,
                                        &connected_hostkey_type,
                                        &connected_hostkey_sha1_text,
                                        &connected_hostkey_base64);

  g_debug ("handshake complete");
  g_debug ("remote key type:%s SHA1:%s",
           connected_hostkey_type,
           connected_hostkey_sha1_text);

  g_assert (hotssh_hostdb_lookup_by_id (hotssh_hostdb_get_instance (), priv->connection_id, &iter));

  gtk_tree_model_get (model, &iter,
                      HOTSSH_HOSTDB_COLUMN_HOST_KEY_TYPE,
                      &saved_hostkey_type,
                      HOTSSH_HOSTDB_COLUMN_HOST_KEY_BASE64,
                      &saved_hostkey_base64,
                      -1);

  if (saved_hostkey_type == NULL)
    {
      handle_unknown_hostkey (self,
                              connected_hostkey_type,
                              connected_hostkey_sha1_text);
    }
  else
    {
      verify_hostkey (self,
                      connected_hostkey_type,
                      connected_hostkey_base64,
                      saved_hostkey_type,
                      saved_hostkey_base64);
    }

 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
on_socket_client_event (GSocketClient      *client,
                        GSocketClientEvent  event,
                        GSocketConnectable *connectable,
                        GIOStream          *connection,
                        gpointer            user_data)
{
  HotSshTab *self = HOTSSH_TAB (user_data);
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  switch (event)
    {
    case G_SOCKET_CLIENT_RESOLVING:
      set_status_printf (self, _("Resolving '%s'…"),
                         priv->hostname);
      break;
    case G_SOCKET_CLIENT_CONNECTING:
      {
        GSocketConnection *socketconn = G_SOCKET_CONNECTION (connection);
        gs_unref_object GSocketAddress *remote_address =
          g_socket_connection_get_remote_address (socketconn, NULL);

        g_debug ("socket connecting remote=%p", remote_address);
        if (remote_address && G_IS_INET_SOCKET_ADDRESS (remote_address))
          {
            GInetAddress *inetaddr =
              g_inet_socket_address_get_address ((GInetSocketAddress*)remote_address);
            gs_free char *inet_str = g_inet_address_to_string (inetaddr);
            set_status_printf (self, _("Connecting to '%s'…"),
                               inet_str);
          }
        break;
      }
    default:
      break;
    }
}

static void
on_add_new_connection (GtkButton     *button,
                       HotSshTab  *self)
{
  page_transition (self, HOTSSH_TAB_PAGE_NEW_CONNECTION);
}
                        
static void
on_connect (GtkButton     *button,
	    HotSshTab  *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GError *local_error = NULL;
  GError **error = &local_error;
  GtkTreeIter iter;
  gs_unref_object GNetworkAddress *address = NULL;
  const char *hostname;
  const char *username;

  hostname = gtk_entry_get_text (GTK_ENTRY (priv->host_entry));
  username = gtk_entry_get_text (GTK_ENTRY (priv->username_entry));

  g_clear_object (&priv->cancellable);
  priv->cancellable = g_cancellable_new ();

  g_clear_object (&priv->address);
  address = (GNetworkAddress*)g_network_address_parse (hostname, 22, error);
  if (!address)
    {
      page_transition_take_error (self, local_error);
      return;
    }

  g_clear_pointer (&priv->connection_id, g_free);
  hotssh_hostdb_add_entry (hotssh_hostdb_get_instance (),
                           username,
                           (GNetworkAddress*)address,
                           &priv->connection_id);
  hotssh_hostdb_lookup_by_id (hotssh_hostdb_get_instance (),
                              priv->connection_id, &iter);
  /*
  gtk_tree_selection_select_iter (gtk_tree_view_get_selection ((GtkTreeView*)priv->connections_treeview),
                                  &iter);
  */

  page_transition (self, HOTSSH_TAB_PAGE_LIST_CONNECTIONS);
}

static void
on_connection_row_activated (GtkTreeView       *tree_view,
                             GtkTreePath       *path,
                             GtkTreeViewColumn *column,
                             gpointer           user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GtkTreeIter iter;
  guint port;
  gs_unref_object GSocketConnectable *address = NULL;
  gs_unref_object GtkTreeModel *model = NULL;

  model = hotssh_hostdb_get_model (hotssh_hostdb_get_instance ());

  g_assert (gtk_tree_model_get_iter (model, &iter, path));

  g_clear_pointer (&priv->connection_id, g_free);
  g_clear_pointer (&priv->hostname, g_free);
  g_clear_pointer (&priv->username, g_free);
  gtk_tree_model_get (model, &iter,
                      HOTSSH_HOSTDB_COLUMN_ID,
                      &priv->connection_id,
                      HOTSSH_HOSTDB_COLUMN_HOSTNAME,
                      &priv->hostname,
                      HOTSSH_HOSTDB_COLUMN_PORT,
                      &port,
                      HOTSSH_HOSTDB_COLUMN_USERNAME,
                      &priv->username,
                      -1);

  g_clear_object (&priv->address);
  priv->address = g_network_address_new (priv->hostname, port);

  g_object_notify ((GObject*)self, "hostname");

  page_transition (self, HOTSSH_TAB_PAGE_CONNECTING);
  g_clear_object (&priv->connection);
  priv->connection = gssh_connection_new (priv->address, priv->username); 
  g_signal_connect (gssh_connection_get_socket_client (priv->connection),
                    "event", G_CALLBACK (on_socket_client_event), self);
  gssh_connection_set_interaction (priv->connection, (GTlsInteraction*)priv->password_interaction);
  g_signal_connect_object (priv->connection, "notify::state",
			   G_CALLBACK (on_connection_state_notify),
			   self, 0);
  g_debug ("connected, beginning handshake");
  gssh_connection_handshake_async (priv->connection, priv->cancellable,
				   on_connection_handshake, self);
  
  hotssh_hostdb_update_last_used (hotssh_hostdb_get_instance (),
                                  priv->connection_id);
}

static void
process_write_queue (HotSshTab        *self);

static void
on_ostream_write_complete (GObject           *src,
			   GAsyncResult      *res,
			   gpointer           user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  gssize result;
  GError *local_error = NULL;
  GBytes *buf;
  gsize bufsize;

  priv->have_outstanding_write = FALSE;

  result = g_output_stream_write_bytes_finish ((GOutputStream*)src, res, &local_error);
  if (result < 0)
    goto out;

  buf = g_queue_pop_head (&priv->write_queue);
  g_assert (buf != NULL);
  bufsize = g_bytes_get_size (buf);
  
  if (result == bufsize)
    ;
  else 
    {
      GBytes *subbuf;
      g_assert (result < bufsize);
      subbuf = g_bytes_new_from_bytes (buf, result, bufsize - result);
      g_queue_push_head (&priv->write_queue, subbuf);
    }
  
  process_write_queue (self);

 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
process_write_queue (HotSshTab        *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  if (!priv->have_outstanding_write && priv->write_queue.length > 0)
    {
      GOutputStream *ostream = g_io_stream_get_output_stream ((GIOStream*)priv->channel);
      GBytes *buf = g_queue_peek_head (&priv->write_queue);

      g_output_stream_write_bytes_async (ostream, buf, G_PRIORITY_DEFAULT,
					 priv->cancellable, on_ostream_write_complete, self);
      priv->have_outstanding_write = TRUE;
    }
}

static void
on_terminal_commit (VteTerminal *vteterminal,
		    gchar       *text,
		    guint        size,
		    gpointer     user_data) 
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  g_queue_push_tail (&priv->write_queue, g_bytes_new (text, size));
  process_write_queue (self);
}

static void
submit_password (HotSshTab *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  g_debug ("password submit");
  
  gtk_widget_set_sensitive (priv->password_container, FALSE);

  priv->submitted_password = TRUE;
  iterate_authentication_modes (self);
}

static void
on_connect_cancel (GtkButton     *button,
		   gpointer       user_data)
{
  HotSshTab *self = user_data;
  page_transition (self, HOTSSH_TAB_PAGE_LIST_CONNECTIONS);
}

static void
on_negotiate_complete (GObject             *src,
                       GAsyncResult        *result,
                       gpointer             user_data)
{
  HotSshTab *self = user_data;
  GError *local_error = NULL;

  if (!gssh_connection_negotiate_finish ((GSshConnection*)src, result, &local_error))
    goto out;

  set_status (self, _("Authenticating…"));

  iterate_authentication_modes (self);

 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
on_approve_hostkey_clicked (GtkButton     *button,
			    gpointer       user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  gs_free char *keytype = NULL;
  gs_free char *key_base64 = NULL;

  page_transition (self, HOTSSH_TAB_PAGE_CONNECTING);
  set_status (self, _("Negotiating authentication…"));

  gssh_connection_preauth_get_host_key (priv->connection,
                                        &keytype, NULL, &key_base64);

  hotssh_hostdb_set_entry_host_key_known (hotssh_hostdb_get_instance (),
                                          priv->connection_id,
                                          keytype, key_base64,
                                          NULL);

  gssh_connection_negotiate_async (priv->connection, priv->cancellable,
                                   on_negotiate_complete, self);
}

static void
send_pty_size_request (HotSshTab             *self);

static void
on_pty_size_complete (GObject                    *src,
		      GAsyncResult               *result,
		      gpointer                    user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GError *local_error = NULL;

  g_debug ("pty size request complete");
  priv->sent_pty_size_request = FALSE;

  if (!gssh_channel_request_pty_size_finish (priv->channel, result, &local_error))
    goto out;

  if (priv->need_pty_size_request)
    send_pty_size_request (self);

 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
send_pty_size_request (HotSshTab             *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  guint width = vte_terminal_get_column_count ((VteTerminal*)priv->terminal);
  guint height = vte_terminal_get_row_count ((VteTerminal*)priv->terminal);
  
  priv->need_pty_size_request = FALSE;
  priv->sent_pty_size_request = TRUE;
  gssh_channel_request_pty_size_async (priv->channel, width, height,
				       priv->cancellable, on_pty_size_complete, self);
}

static void
on_terminal_size_allocate (GtkWidget    *widget,
			   GdkRectangle *allocation,
			   gpointer      user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  if (priv->channel)
    {
      priv->need_pty_size_request = TRUE;
      if (!priv->sent_pty_size_request)
	send_pty_size_request (self);
    }
}

static void
hotssh_tab_style_updated (GtkWidget      *widget)
{
  HotSshTab   *self = (HotSshTab*)widget;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GdkRGBA fg, bg;

  GTK_WIDGET_CLASS (hotssh_tab_parent_class)->style_updated (widget);

  /* Hardcode black on white for now; in the future I'd like to do
   * per-host colors.
   */
  fg.red = fg.blue = fg.green = 0;
  fg.alpha = 1;
  bg.red = bg.blue = bg.green = 1;
  bg.alpha = 1;

  vte_terminal_set_color_foreground_rgba ((VteTerminal*)priv->terminal, &fg);
  vte_terminal_set_color_background_rgba ((VteTerminal*)priv->terminal, &bg);
  vte_terminal_set_color_bold_rgba ((VteTerminal*)priv->terminal, &fg);
  vte_terminal_set_color_dim_rgba ((VteTerminal*)priv->terminal, &fg);
}

static gboolean
host_entry_match (GtkEntryCompletion *completion,
                  const char         *key,
                  GtkTreeIter        *iter,
                  gpointer            user_data)
{
  gs_free char *host = NULL;
  GtkTreeModel *model = gtk_entry_completion_get_model (completion);

  if (host == NULL)
    return FALSE;

  gtk_tree_model_get (model, iter, 0, &host, -1);

  return g_str_has_prefix (host, key);
}

static void
on_vte_realize (GtkWidget   *widget,
                HotSshTab   *self)
{
  hotssh_tab_style_updated ((GtkWidget*)self);
}

static void
hotssh_tab_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  HotSshTab *self = (HotSshTab*) (object);
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_HOSTNAME:
      g_value_set_string (value, priv->hostname);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
hotssh_tab_init (HotSshTab *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  priv->settings = g_settings_new ("org.gnome.hotssh");

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_notebook_set_show_tabs ((GtkNotebook*)self, FALSE);

  g_signal_connect (priv->create_and_connect_button, "clicked", G_CALLBACK (on_connect), self);
  g_signal_connect (priv->add_new_connection_button, "clicked", G_CALLBACK (on_add_new_connection), self);
  g_signal_connect (priv->connect_cancel_button, "clicked", G_CALLBACK (on_connect_cancel), self);
  g_signal_connect (priv->error_disconnect, "clicked", G_CALLBACK (on_connect_cancel), self);
  g_signal_connect (priv->auth_cancel_button, "clicked", G_CALLBACK (on_connect_cancel), self);
  g_signal_connect (priv->approve_hostkey_button, "clicked", G_CALLBACK (on_approve_hostkey_clicked), self);
  g_signal_connect (priv->disapprove_hostkey_button, "clicked", G_CALLBACK (on_connect_cancel), self);
  g_signal_connect_swapped (priv->password_entry, "activate", G_CALLBACK (submit_password), self);
  g_signal_connect_swapped (priv->password_submit, "clicked", G_CALLBACK (submit_password), self);
  g_signal_connect (priv->connections_treeview, "row-activated", G_CALLBACK (on_connection_row_activated), self);

  priv->password_interaction = hotssh_password_interaction_new ((GtkEntry*)priv->password_entry);
  
  priv->terminal = vte_terminal_new ();
  g_signal_connect (priv->terminal, "realize", G_CALLBACK (on_vte_realize), self);
  vte_terminal_set_audible_bell ((VteTerminal*)priv->terminal, FALSE);  /* Audible bell is a terrible idea */
  g_signal_connect ((GObject*)priv->terminal, "size-allocate", G_CALLBACK (on_terminal_size_allocate), self);
  g_signal_connect ((GObject*)priv->terminal, "commit", G_CALLBACK (on_terminal_commit), self);
  gtk_box_pack_start ((GtkBox*)priv->terminal_box, priv->terminal, TRUE, TRUE, 0);
  gtk_range_set_adjustment ((GtkRange*)priv->terminal_vscrollbar,
                            gtk_scrollable_get_vadjustment ((GtkScrollable*)priv->terminal));
  gtk_widget_show_all (priv->terminal_box);

  g_queue_init (&priv->write_queue);

  {
    gs_unref_object HotSshHostDB *hostdb = hotssh_hostdb_get_instance ();
    gs_unref_object GtkTreeModel *hostdb_model = hotssh_hostdb_get_model (hostdb);
    priv->host_completion = gtk_entry_completion_new ();
    gtk_entry_completion_set_match_func (priv->host_completion, host_entry_match, self, NULL);
    gtk_entry_completion_set_model (priv->host_completion, hostdb_model);
    gtk_entry_completion_set_text_column (priv->host_completion, 0);
    gtk_entry_completion_set_inline_completion (priv->host_completion, TRUE);
    gtk_entry_set_completion ((GtkEntry*)priv->host_entry, priv->host_completion);
  }
}

static void
hotssh_tab_dispose (GObject *object)
{
  HotSshTab *self = HOTSSH_TAB (object);
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  priv->indisposed = TRUE;

  page_transition (self, HOTSSH_TAB_PAGE_LIST_CONNECTIONS);

  g_clear_object (&priv->host_completion);

  G_OBJECT_CLASS (hotssh_tab_parent_class)->dispose (object);
}

static const char *
seconds_to_time_ago_format (gulong *seconds)
{
  
  if (*seconds < 60)
    return _("Less than 1 minute ago");
  *seconds /= 60;
  if (*seconds < 60)
    return ngettext ("%d minute ago", "%d minutes ago",
                     *seconds);
  *seconds /= 60;
  if (*seconds < 24)
    return ngettext ("%d hour ago", "%d hours ago",
                     *seconds);
  *seconds /= 24;
  return ngettext ("%d day ago", "%d days ago",
                   *seconds);
}

static void
render_last_used (GtkTreeViewColumn *tree_column,
                  GtkCellRenderer *cell,
                  GtkTreeModel *tree_model,
                  GtkTreeIter *iter,
                  gpointer data)
{
  guint64 last_used;
  guint64 current_time;
  gs_free char *formatted_text = NULL;
  const char *text;

  gtk_tree_model_get (tree_model, iter,
                      HOTSSH_HOSTDB_COLUMN_LAST_USED,
                      &last_used,
                      -1);
  if (last_used == 0)
    {
      text = _("Never");
    }
  else
    {
      current_time = g_get_real_time () / G_USEC_PER_SEC;
      if (current_time < last_used)
        text = _("In the future");
      else
        {
          gulong diff = (gulong)(current_time - last_used);
          const char *fmt = seconds_to_time_ago_format (&diff);

          text = formatted_text = g_strdup_printf (fmt, diff);
        }
    }

  g_object_set (cell, "text", text, NULL);
}

static void
render_known (GtkTreeViewColumn *tree_column,
              GtkCellRenderer *cell,
              GtkTreeModel *tree_model,
              GtkTreeIter *iter,
              gpointer data)
{
  gboolean known;
  const char *text;
  gs_free char *keytype;

  gtk_tree_model_get (tree_model, iter,
                      HOTSSH_HOSTDB_COLUMN_IS_KNOWN,
                      &known,
                      HOTSSH_HOSTDB_COLUMN_HOST_KEY_TYPE,
                      &keytype,
                      -1);

  if (!known || !keytype)
    text = _("Unknown");
  else 
    text = keytype;

  g_object_set (cell, "text", text, NULL);
}

static void
hotssh_tab_constructed (GObject *object)
{
  HotSshTab *self = HOTSSH_TAB (object);
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  gs_unref_object GtkTreeModel *model = NULL;

  if (G_OBJECT_CLASS (hotssh_tab_parent_class)->constructed)
    G_OBJECT_CLASS (hotssh_tab_parent_class)->constructed (object);

  page_transition (self, HOTSSH_TAB_PAGE_LIST_CONNECTIONS);

  model = hotssh_hostdb_get_model (hotssh_hostdb_get_instance ());
  gtk_tree_view_set_model ((GtkTreeView*)priv->connections_treeview, model);

  gtk_tree_view_column_add_attribute ((GtkTreeViewColumn*)priv->hostname_column,
                                      (GtkCellRenderer*)priv->hostname_renderer,
                                      "text", 1);
  gtk_tree_view_column_set_cell_data_func ((GtkTreeViewColumn*)priv->lastused_column,
                                           (GtkCellRenderer*)priv->lastused_renderer,
                                           render_last_used,
                                           self, NULL);
  g_object_set (priv->lastused_column,
                "sort-indicator", TRUE,
                "sort-order", GTK_SORT_DESCENDING,
                NULL);
  gtk_tree_view_column_set_cell_data_func ((GtkTreeViewColumn*)priv->known_column,
                                           (GtkCellRenderer*)priv->known_renderer,
                                           render_known,
                                           self, NULL);
}

static void
hotssh_tab_class_init (HotSshTabClass *class)
{
  G_OBJECT_CLASS (class)->get_property = hotssh_tab_get_property;
  G_OBJECT_CLASS (class)->dispose = hotssh_tab_dispose;
  G_OBJECT_CLASS (class)->constructed = hotssh_tab_constructed;

  gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS (class),
                                               "/org/gnome/hotssh/tab.ui");

  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, connections_treeview);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, hostname_column);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, hostname_renderer);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, lastused_column);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, lastused_renderer);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, known_column);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, known_renderer);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, host_entry);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, username_entry);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, create_and_connect_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, add_new_connection_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, connection_text_container);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, connection_text);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, error_text);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, error_disconnect);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, password_container);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, password_entry);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, password_submit);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, connect_cancel_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, auth_cancel_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, hostkey_container);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, hostkey_fingerprint_label);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, approve_hostkey_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, disapprove_hostkey_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, terminal_box);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, terminal_vscrollbar);

  GTK_WIDGET_CLASS (class)->style_updated = hotssh_tab_style_updated;

  g_object_class_install_property (G_OBJECT_CLASS (class),
                                   PROP_HOSTNAME,
                                   g_param_spec_string ("hostname", "Hostname", "",
							NULL,
                                                        G_PARAM_READABLE));
}

HotSshTab *
hotssh_tab_new (void)
{
  return g_object_new (HOTSSH_TYPE_TAB, NULL);
}

HotSshTab *
hotssh_tab_new_channel  (HotSshTab *source)
{
  HotSshTab *tab = hotssh_tab_new ();
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (tab);
  HotSshTabPrivate *source_priv = hotssh_tab_get_instance_private (source);

  state_reset_for_new_connection (tab);
  
  set_status (tab, _("Authenticated, requesting channel…"));
  page_transition (tab, HOTSSH_TAB_PAGE_CONNECTING);

  priv->hostname = g_strdup (source_priv->hostname);
  priv->connection = g_object_ref (source_priv->connection);
  on_connection_state_notify (priv->connection, NULL, tab);

  return tab;
}

void
hotssh_tab_disconnect  (HotSshTab *self)
{
  page_transition (self, HOTSSH_TAB_PAGE_LIST_CONNECTIONS);
}

const char *
hotssh_tab_get_hostname (HotSshTab *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  return priv->hostname;
}

gboolean
hotssh_tab_is_connected (HotSshTab *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  return priv->connection &&
    gssh_connection_get_state (priv->connection) == GSSH_CONNECTION_STATE_CONNECTED;
}

VteTerminal *
hotssh_tab_get_terminal (HotSshTab *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  return (VteTerminal*)priv->terminal;
}
