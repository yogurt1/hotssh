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
#include "gssh.h"

#include "libgsystem.h"

#include <libssh2.h>
#include <vte/vte.h>
#include <stdio.h>

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
  HOTSSH_TAB_PAGE_INTERSTITAL,
  HOTSSH_TAB_PAGE_AUTH,
  HOTSSH_TAB_PAGE_TERMINAL
} HotSshTabPage;

struct _HotSshTabPrivate
{
  GtkWidget *terminal;

  /* Bound via template */
  GtkWidget *host_entry;
  GtkWidget *username_entry;
  GtkWidget *connect_button;
  GtkWidget *connection_text_container;
  GtkWidget *connection_text;
  GtkWidget *password_container;
  GtkWidget *password_entry;
  GtkWidget *password_submit;
  GtkWidget *connect_cancel_button;
  GtkWidget *auth_cancel_button;
  GtkWidget *hostkey_container;
  GtkWidget *hostkey_fingerprint_label;
  GtkWidget *approve_hostkey_button;
  GtkWidget *terminal_box;

  /* State */
  HotSshTabPage active_page;

  GSocketConnectable *address;
  GSshConnection *connection;
  GSshChannel *channel;

  gboolean need_pty_size_request;
  gboolean sent_pty_size_request;
  gboolean have_outstanding_write;
  gboolean have_outstanding_auth;
  GQueue write_queue;

  GCancellable *cancellable;
};

G_DEFINE_TYPE_WITH_PRIVATE(HotSshTab, hotssh_tab, GTK_TYPE_NOTEBOOK);

static void
set_status (HotSshTab     *self,
	    const char       *text)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  gtk_label_set_text ((GtkLabel*)priv->connection_text, text);
}

static void
reset_focus_state (HotSshTab   *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  if (gtk_entry_get_text ((GtkEntry*)priv->host_entry)[0] == '\0')
    gtk_widget_grab_focus (priv->host_entry);
  else
    gtk_widget_grab_focus (priv->username_entry);
}

static void
state_reset_for_new_connection (HotSshTab                *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  g_debug ("reset state");
  g_clear_object (&priv->address);
  g_clear_object (&priv->connection);
  g_clear_object (&priv->cancellable);
  vte_terminal_reset ((VteTerminal*)priv->terminal, TRUE, TRUE);
  gtk_entry_set_text ((GtkEntry*)priv->password_entry, "");
  reset_focus_state (self);
  gtk_label_set_text ((GtkLabel*)priv->connection_text, "");
  gtk_widget_show (priv->connection_text_container);
  gtk_widget_hide (priv->hostkey_container);
  gtk_widget_set_sensitive (priv->password_container, TRUE);
  g_debug ("reset state done");
}

static void
page_transition (HotSshTab        *self,
		 HotSshTabPage     new_page)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  if (new_page == priv->active_page)
    return;

  g_debug ("PAGE: %d => %d", priv->active_page, new_page);
  g_assert (new_page >= HOTSSH_TAB_PAGE_NEW_CONNECTION &&
	    new_page <= HOTSSH_TAB_PAGE_TERMINAL);
  priv->active_page = new_page;

  if (priv->active_page == HOTSSH_TAB_PAGE_NEW_CONNECTION)
    state_reset_for_new_connection (self);

  gtk_notebook_set_current_page ((GtkNotebook*)self, (guint)new_page);
  
  if (priv->active_page == HOTSSH_TAB_PAGE_TERMINAL)
    gtk_widget_grab_focus ((GtkWidget*)priv->terminal);
}

static void
page_transition_take_error (HotSshTab               *self,
			    GError                     *error)
{
  set_status (self, error->message);
  g_error_free (error);
}

static void
on_istream_read_complete (GObject           *src,
			  GAsyncResult      *res,
			  gpointer           user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GBytes *result = NULL;
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

  g_debug ("connection state: %u", new_state);

  switch (new_state)
    {
    case GSSH_CONNECTION_STATE_DISCONNECTED:
      page_transition (self, HOTSSH_TAB_PAGE_NEW_CONNECTION);
      break;
    case GSSH_CONNECTION_STATE_CONNECTING:
    case GSSH_CONNECTION_STATE_HANDSHAKING:
    case GSSH_CONNECTION_STATE_PREAUTH:
      page_transition (self, HOTSSH_TAB_PAGE_INTERSTITAL);
      break;
    case GSSH_CONNECTION_STATE_AUTHENTICATION_REQUIRED:
      page_transition (self, HOTSSH_TAB_PAGE_AUTH);
      iterate_authentication_modes (self);
      break;
    case GSSH_CONNECTION_STATE_ERROR:
      gtk_widget_hide (priv->hostkey_container);
      gtk_widget_show (priv->connection_text_container);
      page_transition (self, HOTSSH_TAB_PAGE_INTERSTITAL);
      break;
    case GSSH_CONNECTION_STATE_CONNECTED:
      gssh_connection_open_shell_async (priv->connection, priv->cancellable,
					  on_open_shell_complete, self);
      break;
    }
}

static void
on_password_auth_complete (GObject                *src,
			   GAsyncResult           *res,
			   gpointer                user_data)
{
  HotSshTab *self = HOTSSH_TAB (user_data);
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GError *local_error = NULL;

  priv->have_outstanding_auth = FALSE;

  if (!gssh_connection_auth_password_finish ((GSshConnection*)src, res, &local_error))
    goto out;

  g_debug ("password auth complete");

 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
iterate_authentication_modes (HotSshTab          *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  const char *const *authschemes =
    gssh_connection_get_authentication_mechanisms (priv->connection);
  const char *const*iter;
  GError *local_error = NULL;

  if (priv->have_outstanding_auth)
    return;

  for (iter = authschemes; iter && *iter; iter++)
    {
      const char *authscheme = *iter;
      if (strcmp (authscheme, "password") == 0)
	{
	  const char *password = gtk_entry_get_text ((GtkEntry*)priv->password_entry);
	  if (password && password[0])
	    {
	      gssh_connection_auth_password_async (priv->connection, password,
						     priv->cancellable,
						     on_password_auth_complete, self);
	      priv->have_outstanding_auth = TRUE;
	      break;
	    }
	}
    }

  g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
	       "No more authentication mechanisms available");
  page_transition_take_error (self, local_error);
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
  GBytes *hostkey_sha1_binary;
  GString *buf;
  guint i;
  const guint8 *binbuf;
  gsize len;
  gs_free char *hostkey_sha1_text = NULL;

  if (!gssh_connection_handshake_finish ((GSshConnection*)object, result, error))
    goto out;

  g_debug ("handshake complete");

  hostkey_sha1_binary = gssh_connection_preauth_get_fingerprint_sha1 (priv->connection);
  binbuf = g_bytes_get_data (hostkey_sha1_binary, &len);
  buf = g_string_new ("");
  for (i = 0; i < len; i++)
    {
      g_string_append_printf (buf, "%02X", binbuf[i]);
      if (i < len - 1)
	g_string_append_c (buf, ':');
    }
  hostkey_sha1_text = g_string_free (buf, FALSE);

  gtk_label_set_text ((GtkLabel*)priv->hostkey_fingerprint_label,
		      hostkey_sha1_text);
  gtk_widget_hide (priv->connection_text_container);
  gtk_widget_show (priv->hostkey_container);
  page_transition (self, HOTSSH_TAB_PAGE_INTERSTITAL);

 out:
  if (local_error)
    page_transition_take_error (self, local_error);
}

static void
on_connect (GtkButton     *button,
	    HotSshTab  *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);
  GError *local_error = NULL;
  GError **error = &local_error;
  const char *hostname;
  const char *username;

  page_transition (self, HOTSSH_TAB_PAGE_INTERSTITAL);
  gtk_notebook_set_current_page ((GtkNotebook*)self, 1);

  hostname = gtk_entry_get_text (GTK_ENTRY (priv->host_entry));
  username = gtk_entry_get_text (GTK_ENTRY (priv->username_entry));

  g_clear_object (&priv->cancellable);
  priv->cancellable = g_cancellable_new ();

  g_clear_object (&priv->address);
  priv->address = g_network_address_parse (hostname, 22, error);
  if (!priv->address)
    {
      page_transition_take_error (self, local_error);
      return;
    }

  g_clear_object (&priv->connection);
  priv->connection = gssh_connection_new (priv->address, username); 
  g_signal_connect_object (priv->connection, "notify::state",
			   G_CALLBACK (on_connection_state_notify),
			   self, 0);
  g_debug ("connected, beginning handshake");
  gssh_connection_handshake_async (priv->connection, priv->cancellable,
				   on_connection_handshake, self);
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

  iterate_authentication_modes (self);
}

static void
on_connect_cancel (GtkButton     *button,
		   gpointer       user_data)
{
  HotSshTab *self = user_data;
  page_transition (self, HOTSSH_TAB_PAGE_NEW_CONNECTION);
}

static void
on_approve_hostkey_clicked (GtkButton     *button,
			    gpointer       user_data)
{
  HotSshTab *self = user_data;
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  gssh_connection_preauth_continue (priv->connection);
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
hotssh_tab_grab_focus (GtkWidget *widget)
{
  reset_focus_state ((HotSshTab*)widget);
}

static void
hotssh_tab_init (HotSshTab *self)
{
  HotSshTabPrivate *priv = hotssh_tab_get_instance_private (self);

  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (priv->connect_button, "clicked", G_CALLBACK (on_connect), self);
  g_signal_connect (priv->connect_cancel_button, "clicked", G_CALLBACK (on_connect_cancel), self);
  g_signal_connect (priv->auth_cancel_button, "clicked", G_CALLBACK (on_connect_cancel), self);
  g_signal_connect (priv->approve_hostkey_button, "clicked", G_CALLBACK (on_approve_hostkey_clicked), self);
  g_signal_connect_swapped (priv->password_entry, "activate", G_CALLBACK (submit_password), self);
  g_signal_connect_swapped (priv->password_submit, "clicked", G_CALLBACK (submit_password), self);

  priv->terminal = vte_terminal_new ();
  vte_terminal_set_audible_bell ((VteTerminal*)priv->terminal, FALSE);  /* Audible bell is a terrible idea */
  g_signal_connect ((GObject*)priv->terminal, "size-allocate", G_CALLBACK (on_terminal_size_allocate), self);
  g_signal_connect ((GObject*)priv->terminal, "commit", G_CALLBACK (on_terminal_commit), self);
  gtk_box_pack_start ((GtkBox*)priv->terminal_box, (GtkWidget*)priv->terminal, TRUE, TRUE, 0);
  gtk_widget_show_all (priv->terminal_box);

  g_queue_init (&priv->write_queue);
}

static void
hotssh_tab_dispose (GObject *object)
{
  HotSshTab *self = HOTSSH_TAB (object);

  page_transition (self, HOTSSH_TAB_PAGE_NEW_CONNECTION);

  G_OBJECT_CLASS (hotssh_tab_parent_class)->dispose (object);
}

static void
hotssh_tab_class_init (HotSshTabClass *class)
{
  G_OBJECT_CLASS (class)->dispose = hotssh_tab_dispose;

  gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS (class),
                                               "/org/gnome/hotssh/tab.ui");

  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, host_entry);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, username_entry);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, connect_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, connection_text_container);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, connection_text);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, password_container);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, password_entry);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, password_submit);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, connect_cancel_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, auth_cancel_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, hostkey_container);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, hostkey_fingerprint_label);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, approve_hostkey_button);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshTab, terminal_box);

  GTK_WIDGET_CLASS (class)->grab_focus = hotssh_tab_grab_focus;
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
  priv->connection = g_object_ref (source_priv->connection);
  on_connection_state_notify (priv->connection, NULL, tab);

  return tab;
}

void
hotssh_tab_disconnect  (HotSshTab *self)
{
  page_transition (self, HOTSSH_TAB_PAGE_NEW_CONNECTION);
  gtk_notebook_set_current_page ((GtkNotebook*)self, 0);
}
