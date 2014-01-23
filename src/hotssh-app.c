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

#include "hotssh-app.h"
#include "hotssh-search-provider.h"
#include "hotssh-win.h"
#include "hotssh-prefs.h"

#include "libgsystem.h"

#include <glib/gi18n.h>

struct _HotSshApp
{
  GtkApplication parent;
  HotSshSearchProvider *search_provider;
};

struct _HotSshAppClass
{
  GtkApplicationClass parent_class;
};

G_DEFINE_TYPE(HotSshApp, hotssh_app, GTK_TYPE_APPLICATION);

static void
hotssh_app_init (HotSshApp *app)
{
}

static void
preferences_activated (GSimpleAction *action,
                       GVariant      *parameter,
                       gpointer       app)
{
  HotSshPrefs *prefs;
  GtkWindow *win;

  win = gtk_application_get_active_window (GTK_APPLICATION (app));
  prefs = hotssh_prefs_new (HOTSSH_WINDOW (win));
  gtk_window_present (GTK_WINDOW (prefs));
}

static void
about_activated (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       app)
{
  const gchar *authors[] = {
    "Colin Walters <walters@verbum.org>",
    NULL
  };
  GtkWindow *parent;

  parent = gtk_application_get_active_window (GTK_APPLICATION (app));

  gtk_show_about_dialog (parent,
                         "authors", authors,
                         "comments", _("SSH client"),
                         "copyright", "Copyright \xc2\xa9 2013 Colin Walters",
                         "license-type", GTK_LICENSE_LGPL_2_1,
                         "logo-icon-name", "hotssh",
                         "program-name", _("HotSSH"),
                         "translator-credits", _("translator-credits"),
                         "version", PACKAGE_VERSION,
                         "website", "https://wiki.gnome.org/Apps/HotSSH",
                         "website-label", _("HotSSH Website"),
                         NULL);
}

static void
quit_activated (GSimpleAction *action,
                GVariant      *parameter,
                gpointer       app)
{
  g_application_quit (G_APPLICATION (app));
}

static GActionEntry app_entries[] =
{
  { "preferences", preferences_activated, NULL, NULL, NULL },
  { "about", about_activated, NULL, NULL, NULL },
  { "quit", quit_activated, NULL, NULL, NULL }
};

static void
hotssh_app_startup (GApplication *app)
{
  HotSshApp *hotssh_app = HOTSSH_APP (app);
  GtkBuilder *builder;
  GMenuModel *app_menu;
  

  G_APPLICATION_CLASS (hotssh_app_parent_class)->startup (app);

  hotssh_app->search_provider = hotssh_search_provider_new (hotssh_app);

  g_action_map_add_action_entries (G_ACTION_MAP (app),
                                   app_entries, G_N_ELEMENTS (app_entries),
                                   app);

  builder = gtk_builder_new_from_resource ("/org/gnome/hotssh/app-menu.ui");
  app_menu = G_MENU_MODEL (gtk_builder_get_object (builder, "appmenu"));
  gtk_application_set_app_menu (GTK_APPLICATION (app), app_menu);
  g_object_unref (builder);

  gtk_application_add_accelerator ((GtkApplication*)app, "<Control><Shift>T", "win.new-tab", NULL);
  {
    guint i = 0;
    for (i = 1; i <= 9; i++)
      {
        gs_free char *key = g_strdup_printf ("<Alt>%u", i);
        gtk_application_add_accelerator ((GtkApplication*)app, key, "win.switch-tab",
                                         g_variant_new_uint32 (i-1));
      }
    gtk_application_add_accelerator ((GtkApplication*)app, "<Alt>0", "win.switch-tab",
                                     g_variant_new_uint32 (9));
  }
}

static void
hotssh_app_activate (GApplication *app)
{
  HotSshWindow *win;

  win = hotssh_window_new (HOTSSH_APP (app));
  gtk_window_present (GTK_WINDOW (win));
}

static int
hotssh_app_command_line (GApplication              *app,
			 GApplicationCommandLine   *cmdline)
{
  HotSshWindow *win;
  gchar **args = NULL;
  gs_free gchar **argv = NULL;
  gint i;
  GError *error = NULL;
  gint argc;
  gboolean no_default_window = FALSE;
  char *host = NULL;
  char *username = NULL;
  gboolean help = FALSE;
  GOptionContext *context;
  GOptionEntry entries[] = {
    { "host", 'h', 0, G_OPTION_ARG_STRING, &host, NULL, NULL },
    { "username", 'u', 0, G_OPTION_ARG_STRING, &username, NULL, NULL },
    { "help", '?', 0, G_OPTION_ARG_NONE, &help, NULL, NULL },
    { "no-default-window", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &no_default_window, NULL, NULL },
    { NULL } 
  };

  args = g_application_command_line_get_arguments (cmdline, &argc);
  argv = g_new (gchar*, argc + 1);
  for (i = 0; i <= argc; i++)
    argv[i] = args[i];
  
  context = g_option_context_new (NULL);
  g_option_context_set_help_enabled (context, FALSE);
  g_option_context_add_main_entries (context, entries, NULL);
  
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_application_command_line_printerr (cmdline, "%s\n", error->message);
      g_error_free (error);
      g_application_command_line_set_exit_status (cmdline, 1);
    }
  else if (help)
    {
      gs_free gchar *text = g_option_context_get_help (context, FALSE, NULL);
      g_application_command_line_print (cmdline, "%s",  text);
    }

  g_strfreev (args);
  g_option_context_free (context);

  if (no_default_window)
    {
      /* By calling hold() and release() we trigger the inactivity timeout,
       * so that we stick around until a D-Bus method call can be made.
       */
      g_application_hold (app);
      g_application_release (app);
    }
  else
    {
      win = hotssh_window_new (HOTSSH_APP (app));
      gtk_widget_show_all ((GtkWidget*)win);
      gtk_window_present (GTK_WINDOW (win));
    }

  return 0;
}

static void
hotssh_app_class_init (HotSshAppClass *class)
{
  G_APPLICATION_CLASS (class)->startup = hotssh_app_startup;
  G_APPLICATION_CLASS (class)->activate = hotssh_app_activate;
  G_APPLICATION_CLASS (class)->command_line = hotssh_app_command_line;
}

HotSshApp *
hotssh_app_new (void)
{
  return g_object_new (HOTSSH_TYPE_APP,
                       "application-id", "org.gnome.hotssh",
                       "flags", G_APPLICATION_HANDLES_COMMAND_LINE,
                       "inactivity-timeout", 120000, /* 2 minutes */
                       NULL);
}
