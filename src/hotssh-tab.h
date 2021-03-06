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

#include <vte/vte.h>

#define HOTSSH_TYPE_TAB (hotssh_tab_get_type ())
#define HOTSSH_TAB(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), HOTSSH_TYPE_TAB, HotSshTab))

typedef struct _HotSshTab         HotSshTab;
typedef struct _HotSshTabClass    HotSshTabClass;


GType                   hotssh_tab_get_type     (void);
HotSshTab              *hotssh_tab_new          (void);
HotSshTab              *hotssh_tab_new_channel  (HotSshTab *source);

void                    hotssh_tab_disconnect  (HotSshTab *source);

const char *            hotssh_tab_get_hostname (HotSshTab *self);

gboolean                hotssh_tab_is_connected (HotSshTab *self);

VteTerminal            *hotssh_tab_get_terminal (HotSshTab *self);
