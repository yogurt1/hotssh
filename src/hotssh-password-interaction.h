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

G_BEGIN_DECLS

#define HOTSSH_TYPE_PASSWORD_INTERACTION         (hotssh_password_interaction_get_type ())
#define HOTSSH_PASSWORD_INTERACTION(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), HOTSSH_TYPE_PASSWORD_INTERACTION, HotSshPasswordInteraction))
#define HOTSSH_PASSWORD_INTERACTION_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), HOTSSH_TYPE_PASSWORD_INTERACTION, HotSshPasswordInteractionClass))
#define HOTSSH_IS_PASSWORD_INTERACTION(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), HOTSSH_TYPE_PASSWORD_INTERACTION))
#define HOTSSH_IS_PASSWORD_INTERACTION_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), HOTSSH_TYPE_PASSWORD_INTERACTION))
#define HOTSSH_PASSWORD_INTERACTION_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), HOTSSH_TYPE_PASSWORD_INTERACTION, HotSshPasswordInteractionClass))

typedef struct _HotSshPasswordInteraction        HotSshPasswordInteraction;
typedef struct _HotSshPasswordInteractionClass   HotSshPasswordInteractionClass;

GType                            hotssh_password_interaction_get_type    (void) G_GNUC_CONST;

HotSshPasswordInteraction *      hotssh_password_interaction_new         (GtkEntry *entry);

G_END_DECLS
