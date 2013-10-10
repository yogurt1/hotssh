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

#include "gssh-channel-private.h"
#include <glib-unix.h>
#include <fcntl.h>

#include "libgsystem.h"

G_DEFINE_TYPE(GSshChannel, gssh_channel, G_TYPE_IO_STREAM);

static GInputStream *
gssh_channel_get_input_stream (GIOStream *io_stream)
{
  GSshChannel *self = GSSH_CHANNEL (io_stream);

  if (self->input_stream == NULL)
    self->input_stream = _gssh_channel_input_stream_new (self);

  return (GInputStream*)self->input_stream;
}

static GOutputStream *
gssh_channel_get_output_stream (GIOStream *io_stream)
{
  GSshChannel *self = GSSH_CHANNEL (io_stream);

  if (self->output_stream == NULL)
    self->output_stream = _gssh_channel_output_stream_new (self);

  return (GOutputStream*)self->output_stream;
}

static gboolean
gssh_channel_close (GIOStream     *stream,
                      GCancellable  *cancellable,
                      GError       **error)
{
  GSshChannel *self = (GSshChannel*)stream;
  
  if (self->connection)
    g_hash_table_remove (self->connection->channels, self);

  return G_IO_STREAM_CLASS (gssh_channel_parent_class)->close_fn (stream, cancellable, error);
}

static void
gssh_channel_close_async (GIOStream           *stream,
                            int                  io_priority,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GSshChannel *self = (GSshChannel*)stream;
  
  if (self->connection)
    g_hash_table_remove (self->connection->channels, self);

  G_IO_STREAM_CLASS (gssh_channel_parent_class)->close_async (stream, io_priority,
                                                                cancellable, callback,
                                                                user_data);
}

static gboolean
gssh_channel_close_finish (GIOStream     *stream,
                             GAsyncResult  *result,
                             GError       **error)
{
  return G_IO_STREAM_CLASS (gssh_channel_parent_class)->close_finish (stream, result, error);
}

static void
gssh_channel_init (GSshChannel *self)
{
}

static void
gssh_channel_dispose (GObject *object)
{
  GSshChannel *self = (GSshChannel*)object;

  g_clear_object (&self->input_stream);
  g_clear_object (&self->output_stream);

  G_OBJECT_CLASS (gssh_channel_parent_class)->dispose (object);
}

static void
gssh_channel_class_init (GSshChannelClass *class)
{
  G_OBJECT_CLASS (class)->dispose = gssh_channel_dispose;
  G_IO_STREAM_CLASS (class)->get_input_stream = gssh_channel_get_input_stream;
  G_IO_STREAM_CLASS (class)->get_output_stream = gssh_channel_get_output_stream;
  G_IO_STREAM_CLASS (class)->close_fn = gssh_channel_close;
  G_IO_STREAM_CLASS (class)->close_async = gssh_channel_close_async;
  G_IO_STREAM_CLASS (class)->close_finish = gssh_channel_close_finish;
}

GSshChannel *
_gssh_channel_new (GSshConnection *connection,
                     LIBSSH2_CHANNEL *libsshchannel)
{
  GSshChannel *self = (GSshChannel*)g_object_new (GSSH_TYPE_CHANNEL, NULL);
  /* We don't hold a ref; if the connection goes away, it will ensure
   * this pointer is zeroed.
   */
  self->connection = connection; 
  self->libsshchannel = libsshchannel; 
  return self;
}
