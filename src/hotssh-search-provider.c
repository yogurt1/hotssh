/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Red Hat, Inc.
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

#include <string.h>

#include "hotssh-search-provider.h"
#include "hotssh-search-glue.h"

struct _HotSshSearchProvider
{
  GObject parent;
};

struct _HotSshSearchProviderClass
{
  GObjectClass parent_class;
};

typedef struct _HotSshSearchProviderPrivate HotSshSearchProviderPrivate;

struct _HotSshSearchProviderPrivate
{
  HotSshApp *app;
  guint owner_id;
  HotSshSearchShellSearchProvider2 *skeleton;
};

G_DEFINE_TYPE_WITH_PRIVATE(HotSshSearchProvider, hotssh_search_provider, G_TYPE_OBJECT)

static char **
get_results (HotSshSearchProvider *search_provider,
             char                **terms)
{
  char **term;
  gboolean match = TRUE;

  for (term = terms; *term; term++)
    {
      if (strstr ("foo.example.com", *term) == NULL)
        match = FALSE;
    }

  if (match)
    {
      const char * const results[] = { "foo.example.com", NULL };
      return g_strdupv ((char **)results);
    }
  else
    {
      const char * const results[] = { NULL };
      return g_strdupv ((char **)results);
    }
}

static gboolean
handle_get_initial_result_set (HotSshSearchShellSearchProvider2 *skeleton,
                               GDBusMethodInvocation            *invocation,
                               char                            **terms,
                               HotSshSearchProvider             *search_provider)
{
  char **results = get_results (search_provider, terms);
  hot_ssh_search_shell_search_provider2_complete_get_initial_result_set (skeleton, invocation, (const char * const *)results);
  g_strfreev (results);

  return TRUE;
}

static gboolean
handle_get_subsearch_result_set (HotSshSearchShellSearchProvider2 *skeleton,
                                 GDBusMethodInvocation            *invocation,
                                 char                            **previous_results,
                                 char                            **terms,
                                 HotSshSearchProvider             *search_provider)
{
  char **results = get_results (search_provider, terms);
  hot_ssh_search_shell_search_provider2_complete_get_subsearch_result_set (skeleton, invocation, (const char * const *)results);
  g_strfreev (results);

  return TRUE;
}

static gboolean
handle_get_result_metas (HotSshSearchShellSearchProvider2 *skeleton,
                         GDBusMethodInvocation            *invocation,
                         char                            **identifiers,
                         HotSshSearchProvider              *search_provider)
{
  GVariantBuilder *b1;
  GVariant *metas;
  char **p;

  b1 = g_variant_builder_new (G_VARIANT_TYPE ("aa{sv}"));
  for (p = identifiers; *p; p++)
    {
      const char *id = *p;
      if (g_strcmp0 (id, "foo.example.com") == 0)
        {
          GVariantBuilder *b2 = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
          GIcon *icon;
          GVariant *serialized_icon;
          gchar *string_icon;

          g_variant_builder_add (b2, "{sv}", "id", g_variant_new_string (id));
          g_variant_builder_add (b2, "{sv}", "name", g_variant_new_string ("foo.example.com"));

          icon = g_themed_icon_new ("gnome-terminal");

          serialized_icon = g_icon_serialize (icon);
          g_variant_builder_add (b2, "{sv}", "icon", serialized_icon);
          g_variant_unref (serialized_icon);
          string_icon = g_icon_to_string (icon);
          g_variant_builder_add (b2, "{sv}", "gicon", g_variant_new_string (string_icon));
          g_free (string_icon);

          g_object_unref (icon);

          g_variant_builder_add_value (b1, g_variant_builder_end (b2));
        }

    }

  metas = g_variant_builder_end (b1);

  hot_ssh_search_shell_search_provider2_complete_get_result_metas (skeleton, invocation, metas);
  return TRUE;
}

static gboolean
handle_activate_result (HotSshSearchShellSearchProvider2 *skeleton,
                        GDBusMethodInvocation            *invocation,
                        char                             *identifier,
                        char                            **terms,
                        guint                             timestamp,
                        HotSshSearchProvider             *search_provider)
{
  if (g_strcmp0 (identifier, "foo.example.com") == 0)
    {
      g_print ("Open up the Foo server!\n");
    }

  hot_ssh_search_shell_search_provider2_complete_activate_result (skeleton, invocation);
  return TRUE;
}

static gboolean
handle_launch_search (HotSshSearchShellSearchProvider2 *skeleton,
                      GDBusMethodInvocation            *invocation,
                      char                            **terms,
                      guint                             timestamp,
                      HotSshSearchProvider              *search_provider)
{
  return FALSE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar *name,
                 gpointer user_data)
{

  HotSshSearchProvider *search_provider = user_data;
  HotSshSearchProviderPrivate *priv = hotssh_search_provider_get_instance_private (search_provider);

  priv->skeleton = hot_ssh_search_shell_search_provider2_skeleton_new ();
  g_signal_connect (priv->skeleton,
                    "handle-get-initial-result-set",
                    G_CALLBACK (handle_get_initial_result_set), search_provider);
  g_signal_connect (priv->skeleton,
                    "handle-get-subsearch-result-set",
                    G_CALLBACK (handle_get_subsearch_result_set), search_provider);
  g_signal_connect (priv->skeleton,
                    "handle-get-result-metas",
                    G_CALLBACK (handle_get_result_metas), search_provider);
  g_signal_connect (priv->skeleton,
                    "handle-activate-result",
                    G_CALLBACK (handle_activate_result), search_provider);
  g_signal_connect (priv->skeleton,
                    "handle-launch-search",
                    G_CALLBACK (handle_launch_search), search_provider);

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (priv->skeleton),
                                    connection,
                                    "/org/gnome/hotssh/SearchProvider",
                                    NULL);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar *name,
                  gpointer user_data)
{
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar *name,
              gpointer user_data)
{
}

static void
hotssh_search_provider_init (HotSshSearchProvider *search_provider)
{
  HotSshSearchProviderPrivate *priv = hotssh_search_provider_get_instance_private (search_provider);

  priv->owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                   "org.gnome.HotSsh.SearchProvider",
                                   G_BUS_NAME_OWNER_FLAGS_NONE,
                                   on_bus_acquired,
                                   on_name_acquired,
                                   on_name_lost,
                                   search_provider,
                                   NULL);
}

static void
hotssh_search_provider_dispose (GObject *object)
{
  HotSshSearchProvider *search_provider = HOTSSH_SEARCH_PROVIDER (object);
  HotSshSearchProviderPrivate *priv = hotssh_search_provider_get_instance_private (search_provider);

  if (priv->owner_id)
    {
      g_bus_unown_name (priv->owner_id);
      priv->owner_id = 0;
    }

  G_OBJECT_CLASS (hotssh_search_provider_parent_class)->dispose (object);
}

static void
hotssh_search_provider_class_init (HotSshSearchProviderClass *class)
{
  G_OBJECT_CLASS (class)->dispose = hotssh_search_provider_dispose;
}

HotSshSearchProvider *
hotssh_search_provider_new (HotSshApp *app)
{
  HotSshSearchProvider *search_provider;
  HotSshSearchProviderPrivate *priv;

  search_provider = g_object_new (HOTSSH_TYPE_SEARCH_PROVIDER, NULL);
  priv = hotssh_search_provider_get_instance_private (search_provider);

  priv->app = app;

  return search_provider;
}
