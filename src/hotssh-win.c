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

#include "hotssh-win.h"
#include "hotssh-tab.h"

#include "libgsystem.h"

#include <glib/gi18n.h>

static void hotssh_win_append_tab (HotSshWindow   *self, gboolean new_channel);
static void new_tab_activated (GSimpleAction    *action,
                               GVariant         *parameter,
                               gpointer          user_data);
static void new_channel_activated (GSimpleAction    *action,
                                   GVariant         *parameter,
                                   gpointer          user_data);
static void disconnect_activated (GSimpleAction    *action,
                                  GVariant         *parameter,
                                  gpointer          user_data);
static void copy_activated (GSimpleAction    *action,
                            GVariant         *parameter,
                            gpointer          user_data);
static void paste_activated (GSimpleAction    *action,
                             GVariant         *parameter,
                             gpointer          user_data);

static GActionEntry win_entries[] = {
  { "new-tab", new_tab_activated, NULL, NULL, NULL },
  { "new-channel", new_channel_activated, NULL, NULL, NULL },
  { "disconnect", disconnect_activated, NULL, NULL, NULL },
  { "copy", copy_activated, NULL, NULL, NULL },
  { "paste", paste_activated, NULL, NULL, NULL }
};

struct _HotSshWindow
{
  GtkApplicationWindow parent;
};

struct _HotSshWindowClass
{
  GtkApplicationWindowClass parent_class;
};

typedef struct _HotSshWindowPrivate HotSshWindowPrivate;

struct _HotSshWindowPrivate
{
  GtkWidget *terminal;
  GSettings *settings;

  /* Bound via template */
  GtkWidget *main_notebook;
  GtkWidget *new_connection;
  GtkWidget *gears;
};

G_DEFINE_TYPE_WITH_PRIVATE(HotSshWindow, hotssh_window, GTK_TYPE_APPLICATION_WINDOW);

static guint
find_tab_index (HotSshWindow    *self,
		HotSshTab       *tab)
{
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);
  gint n_pages, i;

  n_pages = gtk_notebook_get_n_pages ((GtkNotebook*)priv->main_notebook);
  for (i = 0; i < n_pages; i++)
    {
      GtkWidget *widget = gtk_notebook_get_nth_page ((GtkNotebook*)priv->main_notebook, i);
      if (widget == (GtkWidget*)tab)
	return (guint)i;
    }
  g_assert_not_reached ();
}

static void
set_close_button_visibility (HotSshWindow *self,
                             gboolean      visible)
{
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);
  guint i;
  guint n;

  n = gtk_notebook_get_n_pages ((GtkNotebook*)priv->main_notebook);

  for (i = 0; i < n; i++)
    {
      GtkWidget *tab = gtk_notebook_get_nth_page ((GtkNotebook*)priv->main_notebook, i);
      GtkWidget *label = gtk_notebook_get_tab_label ((GtkNotebook*)priv->main_notebook, tab);
      GtkWidget *close_button = g_object_get_data ((GObject*)label, "close-button");

      if (visible)
        gtk_widget_show (close_button);
      else
        gtk_widget_hide (close_button);
    }
}

static void
on_tab_close_button_clicked (GtkButton    *button,
			     gpointer      user_data)
{
  HotSshTab *tab = user_data;
  HotSshWindow *win = (HotSshWindow*)g_object_get_data ((GObject*)tab, "window");
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (win);
  guint index;

  index = find_tab_index (win, tab);
  gtk_notebook_remove_page ((GtkNotebook*)priv->main_notebook, index);
  if (gtk_notebook_get_n_pages ((GtkNotebook*)priv->main_notebook) <= 1)
    set_close_button_visibility (win, FALSE);
}

static GtkWidget *
create_tab_label (HotSshWindow       *self,
		  HotSshTab          *tab)
{
  GtkContainer *label_box;
  GtkLabel *label;
  GtkButton *close_button;
  GtkImage *close_image;

  label_box = (GtkContainer*)gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  label = (GtkLabel*)gtk_label_new ("");
  gtk_label_set_single_line_mode (label, TRUE);
  gtk_misc_set_alignment ((GtkMisc*)label, 0.0, 0.5);
  gtk_misc_set_padding ((GtkMisc*)label, 0, 0);
  gtk_box_pack_start ((GtkBox*)label_box, (GtkWidget*)label, FALSE, FALSE, 0);

  close_button = (GtkButton*)gtk_button_new ();
  gtk_widget_set_name ((GtkWidget*)close_button, "hotssh-tab-close-button");
  gtk_button_set_focus_on_click (close_button, FALSE);
  gtk_button_set_relief (close_button, GTK_RELIEF_NONE);

  close_image = (GtkImage*)gtk_image_new_from_icon_name ("window-close-symbolic",
							 GTK_ICON_SIZE_MENU);
  gtk_widget_set_tooltip_text ((GtkWidget*)close_button, _("Close tab"));
  g_signal_connect (close_button, "clicked",
		    G_CALLBACK (on_tab_close_button_clicked), tab);
  gtk_container_add ((GtkContainer*)close_button, (GtkWidget*)close_image);

  gtk_box_pack_start ((GtkBox*)label_box, (GtkWidget*)close_button, FALSE, FALSE, 0);

  gtk_widget_show_all ((GtkWidget*)label_box);
  g_object_set_data ((GObject*)label_box, "label-text", label);
  g_object_set_data ((GObject*)label_box, "close-button", close_button);
  return (GtkWidget*)label_box;
}

static void
on_tab_hostname_changed (HotSshTab           *tab,
                         GParamSpec          *pspec,
                         HotSshWindow        *self)
{
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);
  GtkWidget *label_box = gtk_notebook_get_tab_label ((GtkNotebook*)priv->main_notebook, (GtkWidget*)tab);
  GtkLabel *real_label = GTK_LABEL (g_object_get_data ((GObject*)label_box, "label-text"));
  const char *hostname = hotssh_tab_get_hostname (tab);
  gtk_label_set_text (real_label, hostname ? hostname : _("Disconnected"));
}

static void
hotssh_win_append_tab (HotSshWindow   *self, gboolean new_channel)
{
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);
  GtkWidget *label;
  HotSshTab *tab;
  int idx;
  guint n_pages;
  gboolean is_first_tab;
  gboolean was_single_tab;

  if (new_channel)
    {
      guint i = gtk_notebook_get_current_page ((GtkNotebook*)priv->main_notebook);
      HotSshTab *current_tab = (HotSshTab*)gtk_notebook_get_nth_page ((GtkNotebook*)priv->main_notebook, i);
      tab = hotssh_tab_new_channel (current_tab);
    }
  else
    {
      tab = hotssh_tab_new ();
    }

  n_pages = gtk_notebook_get_n_pages ((GtkNotebook*)priv->main_notebook);
  is_first_tab = n_pages == 0;
  was_single_tab = n_pages == 1;

  g_object_set_data ((GObject*)tab, "window", self);
  label = create_tab_label (self, tab);
  g_signal_connect ((GObject*)tab, "notify::hostname", G_CALLBACK (on_tab_hostname_changed), self);
  idx = gtk_notebook_append_page ((GtkNotebook*)priv->main_notebook,
                                  (GtkWidget*)tab,
                                  (GtkWidget*)label);
  if (was_single_tab)
    set_close_button_visibility (self, TRUE);
  else if (is_first_tab)
    set_close_button_visibility (self, FALSE);

  on_tab_hostname_changed (tab, NULL, self);
  gtk_widget_show_all ((GtkWidget*)tab);
  gtk_notebook_set_current_page ((GtkNotebook*)priv->main_notebook, idx);
  gtk_widget_grab_focus ((GtkWidget*)tab);
}

static void
new_tab_activated (GSimpleAction    *action,
		   GVariant         *parameter,
		   gpointer          user_data)
{
  HotSshWindow *self = user_data;

  hotssh_win_append_tab (self, FALSE);
}

static void
disconnect_activated (GSimpleAction    *action,
                      GVariant         *parameter,
                      gpointer          user_data)
{
  HotSshWindow *self = user_data;
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);
  guint i = gtk_notebook_get_current_page ((GtkNotebook*)priv->main_notebook);
  HotSshTab *current_tab = (HotSshTab*)gtk_notebook_get_nth_page ((GtkNotebook*)priv->main_notebook, i);

  hotssh_tab_disconnect (current_tab);
}

static void
new_channel_activated (GSimpleAction    *action,
                       GVariant         *parameter,
                       gpointer          user_data)
{
  HotSshWindow *self = user_data;

  hotssh_win_append_tab (self, TRUE);
}

static void
copy_activated (GSimpleAction    *action,
                GVariant         *parameter,
                gpointer          user_data)
{
  HotSshWindow *self = user_data;
  GtkWidget *focus = gtk_window_get_focus ((GtkWindow*)self);
  
  if (!focus)
    return;

  if (GTK_IS_EDITABLE (focus))
    gtk_editable_paste_clipboard ((GtkEditable*) focus);
  else if (VTE_IS_TERMINAL (focus))
    vte_terminal_copy_clipboard ((VteTerminal*) focus);
}

static void
paste_activated (GSimpleAction    *action,
                 GVariant         *parameter,
                 gpointer          user_data)
{
  HotSshWindow *self = user_data;
  GtkWidget *focus = gtk_window_get_focus ((GtkWindow*)self);

  if (!focus)
    return;

  if (GTK_IS_EDITABLE (focus))
    gtk_editable_paste_clipboard ((GtkEditable*) focus);
  else if (VTE_IS_TERMINAL (focus))
    vte_terminal_paste_clipboard ((VteTerminal*) focus);
}

static void
hotssh_window_init (HotSshWindow *self)
{
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);
  GtkBuilder *builder;
  GMenuModel *menu;

  gtk_widget_init_template (GTK_WIDGET (self));
  priv->settings = g_settings_new ("org.gnome.hotssh");

  g_action_map_add_action_entries ((GActionMap*) self, win_entries,
				   G_N_ELEMENTS (win_entries), self);

  builder = gtk_builder_new_from_resource ("/org/gnome/hotssh/gears-menu.ui");
  menu = G_MENU_MODEL (gtk_builder_get_object (builder, "menu"));
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (priv->gears), menu);
  g_object_unref (builder);

  gtk_actionable_set_action_name ((GtkActionable*)priv->new_connection, "win.new-tab");

  hotssh_win_append_tab (self, FALSE);
}

static void
hotssh_window_dispose (GObject *object)
{
  HotSshWindow *self = HOTSSH_WINDOW (object);
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);

  g_clear_object (&priv->settings);

  G_OBJECT_CLASS (hotssh_window_parent_class)->dispose (object);
}

static void
hotssh_window_class_init (HotSshWindowClass *class)
{
  G_OBJECT_CLASS (class)->dispose = hotssh_window_dispose;

  gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS (class),
                                               "/org/gnome/hotssh/window.ui");

  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshWindow, main_notebook);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshWindow, new_connection);
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshWindow, gears);
}

HotSshWindow *
hotssh_window_new (HotSshApp *app)
{
  return g_object_new (HOTSSH_TYPE_WINDOW, "application", app, NULL);
}

GList *
hotssh_window_get_tabs (HotSshWindow *self)
{
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);
  GList *tabs = NULL;
  gint n_pages, i;

  n_pages = gtk_notebook_get_n_pages ((GtkNotebook*)priv->main_notebook);
  for (i = 0; i < n_pages; i++)
    tabs = g_list_prepend (tabs, gtk_notebook_get_nth_page ((GtkNotebook*)priv->main_notebook, i));

  return g_list_reverse (tabs);
}

void
hotssh_window_activate_tab (HotSshWindow *self,
                            HotSshTab    *tab)
{
  HotSshWindowPrivate *priv = hotssh_window_get_instance_private (self);
  guint index;

  index = find_tab_index (self, tab);
  gtk_notebook_set_current_page ((GtkNotebook*)priv->main_notebook, index);
}
