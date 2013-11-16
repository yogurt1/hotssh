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

#include "hotssh-password-interaction.h"

struct _HotSshPasswordInteraction
{
  GTlsInteraction parent_instance;

  GtkEntry *entry;
};

struct _HotSshPasswordInteractionClass
{
  GTlsInteractionClass parent_class;
};

#include <string.h>

G_DEFINE_TYPE (HotSshPasswordInteraction, hotssh_password_interaction, G_TYPE_TLS_INTERACTION);

static GTlsInteractionResult
hotssh_password_interaction_ask_password (GTlsInteraction    *interaction,
                                        GTlsPassword       *password,
                                        GCancellable       *cancellable,
                                        GError            **error)
{
  HotSshPasswordInteraction *self = (HotSshPasswordInteraction*)interaction;
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return G_TLS_INTERACTION_FAILED;

  g_tls_password_set_value (password, (guint8*)gtk_entry_get_text (self->entry), -1);
  return G_TLS_INTERACTION_HANDLED;
}

static void
hotssh_password_interaction_init (HotSshPasswordInteraction *interaction)
{
}

static void
hotssh_password_interaction_class_init (HotSshPasswordInteractionClass *klass)
{
  GTlsInteractionClass *interaction_class = G_TLS_INTERACTION_CLASS (klass);
  interaction_class->ask_password = hotssh_password_interaction_ask_password;
}

HotSshPasswordInteraction *
hotssh_password_interaction_new (GtkEntry *entry)
{
  HotSshPasswordInteraction *self = g_object_new (HOTSSH_TYPE_PASSWORD_INTERACTION, NULL);
  self->entry = entry;
  return self;
}
