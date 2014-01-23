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

#define HOTSSH_TYPE_APP (hotssh_app_get_type ())
#define HOTSSH_APP(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), HOTSSH_TYPE_APP, HotSshApp))


typedef struct _HotSshApp       HotSshApp;
typedef struct _HotSshAppClass  HotSshAppClass;

GType           hotssh_app_get_type    (void);
HotSshApp     *hotssh_app_new         (void);
