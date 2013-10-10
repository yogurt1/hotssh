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

#pragma once

#include "gssh-connection-private.h"
#include "gssh-channel.h"
#include "gssh-channel-input-stream.h"
#include "gssh-channel-output-stream.h"

struct _GSshChannel
{
  GIOStream parent;

  GSshConnection *connection;
  gboolean have_pty;
  LIBSSH2_CHANNEL *libsshchannel;

  GTask *pty_size_task;
  guint pty_width;
  guint pty_height;
  
  GSshChannelInputStream *input_stream;
  GSshChannelOutputStream *output_stream;
};

struct _GSshChannelClass
{
  GIOStreamClass parent_class;
};

GSshChannel       *_gssh_channel_new (GSshConnection  *connection,
                                      gboolean         have_pty,
                                      LIBSSH2_CHANNEL *libsshchannel);


void _gssh_channel_iteration (GSshChannel    *self);
