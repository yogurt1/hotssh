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
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "hotssh-app.h"

#define HOTSSH_TYPE_SEARCH_PROVIDER (hotssh_search_provider_get_type ())
#define HOTSSH_SEARCH_PROVIDER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), HOTSSH_TYPE_SEARCH_PROVIDER, HotSshSearchProvider))

typedef struct _HotSshSearchProvider          HotSshSearchProvider;
typedef struct _HotSshSearchProviderClass     HotSshSearchProviderClass;

GType                   hotssh_search_provider_get_type     (void);
HotSshSearchProvider   *hotssh_search_provider_new          (HotSshApp *app);
