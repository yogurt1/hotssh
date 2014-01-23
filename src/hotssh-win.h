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

#include "hotssh-app.h"
#include "hotssh-tab.h"

#define HOTSSH_TYPE_WINDOW (hotssh_window_get_type ())
#define HOTSSH_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), HOTSSH_TYPE_WINDOW, HotSshWindow))
#define HOTSSH_IS_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), HOTSSH_TYPE_WINDOW))

typedef struct _HotSshWindow         HotSshWindow;
typedef struct _HotSshWindowClass    HotSshWindowClass;


GType                   hotssh_window_get_type     (void);
HotSshWindow       *hotssh_window_new          (HotSshApp *app);

GList *hotssh_window_get_tabs     (HotSshWindow *self);
void   hotssh_window_activate_tab (HotSshWindow *self,
                                   HotSshTab    *tab);
