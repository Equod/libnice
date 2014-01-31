/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2010, 2013 Collabora Ltd.
 *  Contact: Youness Alaoui
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Youness Alaoui, Collabora Ltd.
 *   Philip Withnall, Collabora Ltd.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

/**
 * SECTION:nice_output_stream
 * @short_description: #GOutputStream implementation for libnice
 * @see_also: #NiceAgent
 * @include: outputstream.h
 * @stability: Stable
 *
 * #NiceOutputStream is a #GOutputStream wrapper for a single reliable stream
 * and component of a #NiceAgent. Given an existing reliable #NiceAgent, plus
 * the IDs of an existing stream and component in the agent, it will provide a
 * streaming output interface for writing to the given component.
 *
 * A single #NiceOutputStream can only be used with a single agent, stream and
 * component triple, and will be closed as soon as that stream is removed from
 * the agent (e.g. if nice_agent_remove_stream() is called from another thread).
 * If g_output_stream_close() is called on a #NiceOutputStream, the output
 * stream will be marked as closed, but the underlying #NiceAgent stream will
 * not be removed. Use nice_agent_remove_stream() to do that.
 *
 * The output stream can only be used once the
 * #NiceAgent::reliable-transport-writable signal has been received for the
 * stream/component pair. Any calls to g_output_stream_write() before then will
 * return %G_IO_ERROR_BROKEN_PIPE.
 *
 * Since: 0.1.5
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>

#include "outputstream.h"
#include "agent-priv.h"

static void nice_output_stream_init_pollable (
    GPollableOutputStreamInterface *iface);
static void streams_removed_cb (NiceAgent *agent, guint *stream_ids,
    gpointer user_data);

G_DEFINE_TYPE_WITH_CODE (NiceOutputStream,
                         nice_output_stream, G_TYPE_OUTPUT_STREAM,
                         G_IMPLEMENT_INTERFACE (G_TYPE_POLLABLE_OUTPUT_STREAM,
                                                nice_output_stream_init_pollable));

enum
{
  PROP_AGENT = 1,
  PROP_STREAM_ID,
  PROP_COMPONENT_ID,
};

struct _NiceOutputStreamPrivate
{
  GWeakRef/*<NiceAgent>*/ agent_ref;
  guint stream_id;
  guint component_id;
};

static void nice_output_stream_dispose (GObject *object);
static void nice_output_stream_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec);
static void nice_output_stream_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec);

static gssize nice_output_stream_write (GOutputStream *stream,
    const void *buffer, gsize count, GCancellable *cancellable, GError **error);

static gboolean nice_output_stream_is_writable (GPollableOutputStream *stream);
static gssize nice_output_stream_write_nonblocking (
    GPollableOutputStream *stream, const void *buffer, gsize count,
    GError **error);
static GSource *nice_output_stream_create_source (GPollableOutputStream *stream,
    GCancellable *cancellable);

/* Output Stream */
static void
nice_output_stream_class_init (NiceOutputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);

  g_type_class_add_private (klass, sizeof (NiceOutputStreamPrivate));

  stream_class->write_fn = nice_output_stream_write;

  gobject_class->set_property = nice_output_stream_set_property;
  gobject_class->get_property = nice_output_stream_get_property;
  gobject_class->dispose = nice_output_stream_dispose;

  /**
   * NiceOutputStream:agent:
   *
   * The #NiceAgent to wrap with an output stream. This must be an existing
   * reliable agent.
   *
   * A reference is not held on the #NiceAgent. If the agent is destroyed before
   * the #NiceOutputStream, %G_IO_ERROR_CLOSED will be returned for all
   * subsequent operations on the stream.
   *
   * Since: 0.1.5
   */
  g_object_class_install_property (gobject_class, PROP_AGENT,
      g_param_spec_object ("agent",
          "NiceAgent",
          "The underlying NiceAgent",
          NICE_TYPE_AGENT,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * NiceOutputStream:stream-id:
   *
   * ID of the stream to use in the #NiceOutputStream:agent.
   *
   * Since: 0.1.5
   */
  g_object_class_install_property (gobject_class, PROP_STREAM_ID,
      g_param_spec_uint (
          "stream-id",
          "Agent’s stream ID",
          "The ID of the agent’s stream to wrap.",
          0, G_MAXUINT,
          0,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * NiceOutputStream:component-id:
   *
   * ID of the component to use in the #NiceOutputStream:agent.
   *
   * Since: 0.1.5
   */
  g_object_class_install_property (gobject_class, PROP_COMPONENT_ID,
      g_param_spec_uint (
          "component-id",
          "Agent’s component ID",
          "The ID of the agent’s component to wrap.",
          0, G_MAXUINT,
          0,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
nice_output_stream_dispose (GObject *object)
{
  NiceOutputStream *self = NICE_OUTPUT_STREAM (object);
  NiceAgent *agent;

  agent = g_weak_ref_get (&self->priv->agent_ref);
  if (agent != NULL) {
    g_signal_handlers_disconnect_by_func (agent, streams_removed_cb, self);
    g_object_unref (agent);
  }

  g_weak_ref_clear (&self->priv->agent_ref);

  G_OBJECT_CLASS (nice_output_stream_parent_class)->dispose (object);
}

static void
nice_output_stream_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  NiceOutputStream *self = NICE_OUTPUT_STREAM (object);

  switch (prop_id) {
    case PROP_AGENT:
      g_value_take_object (value, g_weak_ref_get (&self->priv->agent_ref));
      break;
    case PROP_STREAM_ID:
      g_value_set_uint (value, self->priv->stream_id);
      break;
    case PROP_COMPONENT_ID:
      g_value_set_uint (value, self->priv->component_id);
      break;
     default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
nice_output_stream_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  NiceOutputStream *self = NICE_OUTPUT_STREAM (object);

  switch (prop_id) {
    case PROP_AGENT: {
      /* Construct only. */
      NiceAgent *agent = g_value_dup_object (value);
      g_weak_ref_set (&self->priv->agent_ref, agent);

      /* agent may be NULL if the stream is being constructed by
       * nice_io_stream_get_output_stream() after the NiceIOStream’s agent has
       * already been finalised. */
      if (agent != NULL) {
        g_signal_connect (agent, "streams-removed",
            (GCallback) streams_removed_cb, self);
        g_object_unref (agent);
      }

      break;
    }
    case PROP_STREAM_ID:
      /* Construct only. */
      self->priv->stream_id = g_value_get_uint (value);
      break;
    case PROP_COMPONENT_ID:
      /* Construct only. */
      self->priv->component_id = g_value_get_uint (value);
      break;
     default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
nice_output_stream_init (NiceOutputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream, NICE_TYPE_OUTPUT_STREAM,
      NiceOutputStreamPrivate);

  g_weak_ref_init (&stream->priv->agent_ref, NULL);
}

static void
nice_output_stream_init_pollable (GPollableOutputStreamInterface *iface)
{
  iface->is_writable = nice_output_stream_is_writable;
  iface->write_nonblocking = nice_output_stream_write_nonblocking;
  iface->create_source = nice_output_stream_create_source;
}

/**
 * nice_output_stream_new:
 * @agent: A #NiceAgent
 * @stream_id: The ID of the agent’s stream to wrap
 * @component_id: The ID of the agent’s component to wrap
 *
 * Create a new #NiceOutputStream wrapping the given stream/component from
 * @agent, which must be a reliable #NiceAgent.
 *
 * The constructed #NiceOutputStream will not hold a reference to @agent. If
 * @agent is destroyed before the output stream, %G_IO_ERROR_CLOSED will be
 * returned for all subsequent operations on the stream.
 *
 * Returns: The new #NiceOutputStream object
 *
 * Since: 0.1.5
 */
NiceOutputStream *
nice_output_stream_new (NiceAgent *agent, guint stream_id, guint component_id)
{
  g_return_val_if_fail (NICE_IS_AGENT (agent), NULL);
  g_return_val_if_fail (stream_id >= 1, NULL);
  g_return_val_if_fail (component_id >= 1, NULL);

  return g_object_new (NICE_TYPE_OUTPUT_STREAM,
      "agent", agent,
      "stream-id", stream_id,
      "component-id", component_id,
      NULL);
}

typedef struct {
  volatile gint ref_count;

  GCond cond;
  GMutex mutex;
  GError *error;

  gboolean writable;
} WriteData;

static void
write_data_unref (WriteData *write_data)
{
  if (g_atomic_int_dec_and_test (&write_data->ref_count)) {
    g_cond_clear (&write_data->cond);
    g_mutex_clear (&write_data->mutex);
    g_clear_error (&write_data->error);
    g_slice_free (WriteData, write_data);
  }
}

static void
write_cancelled_cb (GCancellable *cancellable, gpointer user_data)
{
  WriteData *write_data = user_data;

  g_mutex_lock (&write_data->mutex);
  g_cancellable_set_error_if_cancelled (cancellable, &write_data->error);
  g_cond_broadcast (&write_data->cond);
  g_mutex_unlock (&write_data->mutex);
}

static void
reliable_transport_writeable_cb (NiceAgent *agent, guint stream_id,
    guint component_id, gpointer user_data)
{
  WriteData *write_data = user_data;

  g_mutex_lock (&write_data->mutex);
  write_data->writable = TRUE;
  g_cond_broadcast (&write_data->cond);
  g_mutex_unlock (&write_data->mutex);
}

static gssize
nice_output_stream_write (GOutputStream *stream, const void *buffer, gsize count,
    GCancellable *cancellable, GError **error)
{
  NiceOutputStream *self = NICE_OUTPUT_STREAM (stream);
  gssize len = -1;
  gint n_sent_messages;
  GError *child_error = NULL;
  NiceAgent *agent = NULL;  /* owned */
  gulong cancel_id = 0, writeable_id;
  WriteData *write_data;

  /* Closed streams are not writeable. */
  if (g_output_stream_is_closed (stream)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
        "Stream is closed.");
    return -1;
  }

  /* Has the agent disappeared? */
  agent = g_weak_ref_get (&self->priv->agent_ref);
  if (agent == NULL) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
        "Stream is closed due to the NiceAgent being finalised.");
    return -1;
  }

  if (count == 0) {
    g_object_unref (agent);
    return 0;
  }

  /* FIXME: nice_agent_send_full() is non-blocking, which is a bit unexpected
   * since nice_agent_recv() is blocking. Currently this uses a fairly dodgy
   * GCond solution; would be much better for nice_agent_send() to block
   * properly in the main loop. */
  len = 0;
  write_data = g_slice_new0 (WriteData);
  g_atomic_int_set (&write_data->ref_count, 3);
  write_data->error = NULL;

  g_mutex_init (&write_data->mutex);
  g_cond_init (&write_data->cond);

  if (cancellable != NULL) {
    cancel_id = g_cancellable_connect (cancellable,
        (GCallback) write_cancelled_cb, write_data,
        (GDestroyNotify) write_data_unref);
  }

  g_mutex_lock (&write_data->mutex);

  writeable_id = g_signal_connect_data (G_OBJECT (agent),
      "reliable-transport-writable",
      (GCallback) reliable_transport_writeable_cb, write_data,
      (GClosureNotify) write_data_unref, 0);


  do {
    GOutputVector local_buf = { (const guint8 *) buffer + len, count - len };
    NiceOutputMessage local_message = {&local_buf, 1};

    /* Have to unlock while calling into the agent because
     * it will take the agent lock which will cause a deadlock if one of
     * the callbacks is called.
     */
    write_data->writable = FALSE;
    g_mutex_unlock (&write_data->mutex);

    n_sent_messages = nice_agent_send_messages_nonblocking (agent,
        self->priv->stream_id, self->priv->component_id, &local_message, 1,
        cancellable, &child_error);

    g_mutex_lock (&write_data->mutex);

    if (n_sent_messages == -1 &&
        g_error_matches (child_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
      /* EWOULDBLOCK. */
      g_clear_error (&child_error);
      if (!write_data->writable && !write_data->error)
        g_cond_wait (&write_data->cond, &write_data->mutex);
    } else if (n_sent_messages > 0) {
      /* Success. */
      len = count;
    } else {
      /* Other error. */
      len = n_sent_messages;
      break;
    }
  } while ((gsize) len < count);

  g_signal_handler_disconnect (G_OBJECT (agent), writeable_id);
  g_mutex_unlock (&write_data->mutex);

  if (cancellable != NULL) {
    g_cancellable_disconnect (cancellable, cancel_id);
    /* If we were cancelled, but we have no other errors can couldn't write
     * anything, return the cancellation error. If we could write
     * something partial, there is no error.
     */
    if (write_data->error && !child_error && len == 0) {
      g_propagate_error (&child_error, write_data->error);
      len = -1;
    }
  }

  write_data_unref (write_data);

  g_assert ((child_error != NULL) == (len == -1));
  if (child_error)
    g_propagate_error (error, child_error);

  g_object_unref (agent);
  g_assert (len != 0);

  return len;
}

static gboolean
nice_output_stream_is_writable (GPollableOutputStream *stream)
{
  NiceOutputStreamPrivate *priv = NICE_OUTPUT_STREAM (stream)->priv;
  Component *component = NULL;
  Stream *_stream = NULL;
  gboolean retval = FALSE;
  GSList *i;
  NiceAgent *agent;  /* owned */

  /* Closed streams are not writeable. */
  if (g_output_stream_is_closed (G_OUTPUT_STREAM (stream)))
    return FALSE;

  /* Has the agent disappeared? */
  agent = g_weak_ref_get (&priv->agent_ref);
  if (agent == NULL)
    return FALSE;

  agent_lock ();

  if (!agent_find_component (agent, priv->stream_id, priv->component_id,
          &_stream, &component)) {
    g_warning ("Could not find component %u in stream %u", priv->component_id,
        priv->stream_id);
    goto done;
  }

  /* If it’s a reliable agent, see if there’s any space in the pseudo-TCP output
   * buffer. */
  if (agent->reliable && component->tcp != NULL &&
      pseudo_tcp_socket_can_send (component->tcp)) {
    retval = TRUE;
    goto done;
  }

  /* Check whether any of the component’s FDs are pollable. */
  for (i = component->socket_sources; i != NULL; i = i->next) {
    SocketSource *socket_source = i->data;
    NiceSocket *socket = socket_source->socket;

    if (g_socket_condition_check (socket->fileno, G_IO_OUT) != 0) {
      retval = TRUE;
      break;
    }
  }

done:
  agent_unlock ();

  g_object_unref (agent);

  return retval;
}

static gssize
nice_output_stream_write_nonblocking (GPollableOutputStream *stream,
    const void *buffer, gsize count, GError **error)
{
  NiceOutputStreamPrivate *priv = NICE_OUTPUT_STREAM (stream)->priv;
  NiceAgent *agent;  /* owned */
  GOutputVector local_buf = { buffer, count };
  NiceOutputMessage local_message = { &local_buf, 1 };
  gint n_sent_messages;

  /* Closed streams are not writeable. */
  if (g_output_stream_is_closed (G_OUTPUT_STREAM (stream))) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
        "Stream is closed.");
    return -1;
  }

  /* Has the agent disappeared? */
  agent = g_weak_ref_get (&priv->agent_ref);
  if (agent == NULL) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
        "Stream is closed due to the NiceAgent being finalised.");
    return -1;
  }

  if (count == 0)
    return 0;

  /* This is equivalent to the default GPollableOutputStream implementation. */
  if (!g_pollable_output_stream_is_writable (stream)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
        g_strerror (EAGAIN));
    return -1;
  }

  n_sent_messages = nice_agent_send_messages_nonblocking (agent,
      priv->stream_id, priv->component_id, &local_message, 1, NULL, error);

  g_object_unref (agent);

  return (n_sent_messages == 1) ? (gssize) count : n_sent_messages;
}

static GSource *
nice_output_stream_create_source (GPollableOutputStream *stream,
    GCancellable *cancellable)
{
  NiceOutputStreamPrivate *priv = NICE_OUTPUT_STREAM (stream)->priv;
  GSource *component_source = NULL;
  Component *component = NULL;
  Stream *_stream = NULL;
  NiceAgent *agent;  /* owned */

  component_source = g_pollable_source_new (G_OBJECT (stream));

  if (cancellable) {
    GSource *cancellable_source = g_cancellable_source_new (cancellable);

    g_source_set_dummy_callback (cancellable_source);
    g_source_add_child_source (component_source, cancellable_source);
    g_source_unref (cancellable_source);
  }

  /* Closed streams cannot have sources. */
  if (g_output_stream_is_closed (G_OUTPUT_STREAM (stream)))
    return component_source;

  /* Has the agent disappeared? */
  agent = g_weak_ref_get (&priv->agent_ref);
  if (agent == NULL)
    return component_source;

  agent_lock ();

  /* Grab the socket for this component. */
  if (!agent_find_component (agent, priv->stream_id, priv->component_id,
          &_stream, &component)) {
    g_warning ("Could not find component %u in stream %u", priv->component_id,
        priv->stream_id);
    goto done;
  }

   if (component->tcp_writable_cancellable) {
    GSource *cancellable_source =
        g_cancellable_source_new (component->tcp_writable_cancellable);

    g_source_set_dummy_callback (cancellable_source);
    g_source_add_child_source (component_source, cancellable_source);
    g_source_unref (cancellable_source);
  }

done:
  agent_unlock ();

  g_object_unref (agent);

  return component_source;
}

static void
streams_removed_cb (NiceAgent *agent, guint *stream_ids, gpointer user_data)
{
  NiceOutputStream *self = NICE_OUTPUT_STREAM (user_data);
  guint i;

  for (i = 0; stream_ids[i] != 0; i++) {
    if (stream_ids[i] == self->priv->stream_id) {
      /* The socket has been closed. */
      g_output_stream_close (G_OUTPUT_STREAM (self), NULL, NULL);
      break;
    }
  }
}
