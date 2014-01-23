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

#include <gtk/gtk.h>

#include "hotssh-app.h"
#include "hotssh-win.h"
#include "hotssh-prefs.h"

struct _HotSshPrefs
{
  GtkDialog parent;
};

struct _HotSshPrefsClass
{
  GtkDialogClass parent_class;
};

typedef struct _HotSshPrefsPrivate HotSshPrefsPrivate;

struct _HotSshPrefsPrivate
{
  GSettings *settings;
  GtkWidget *match_system_terminal_style;
};

G_DEFINE_TYPE_WITH_PRIVATE(HotSshPrefs, hotssh_prefs, GTK_TYPE_DIALOG)

static void
preferences_closed (GtkWidget *button)
{
  gtk_widget_destroy (gtk_widget_get_toplevel (button));
}

static void
hotssh_prefs_init (HotSshPrefs *prefs)
{
  HotSshPrefsPrivate *priv;

  priv = hotssh_prefs_get_instance_private (prefs);
  gtk_widget_init_template (GTK_WIDGET (prefs));
  priv->settings = g_settings_new ("org.gnome.hotssh");

  g_settings_bind (priv->settings, "match-system-terminal-style",
                   priv->match_system_terminal_style, "active",
                   G_SETTINGS_BIND_DEFAULT);
}

static void
hotssh_prefs_dispose (GObject *object)
{
  HotSshPrefsPrivate *priv;

  priv = hotssh_prefs_get_instance_private (HOTSSH_PREFS (object));
  g_clear_object (&priv->settings);

  G_OBJECT_CLASS (hotssh_prefs_parent_class)->dispose (object);
}

static void
hotssh_prefs_class_init (HotSshPrefsClass *class)
{
  G_OBJECT_CLASS (class)->dispose = hotssh_prefs_dispose;

  gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS (class),
                                               "/org/gnome/hotssh/prefs.ui");
  gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (class), HotSshPrefs, match_system_terminal_style);

  gtk_widget_class_bind_template_callback (GTK_WIDGET_CLASS (class), preferences_closed);
}

HotSshPrefs *
hotssh_prefs_new (HotSshWindow *win)
{
  return g_object_new (HOTSSH_TYPE_PREFS, "transient-for", win, NULL);
}
