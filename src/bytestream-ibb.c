/*
 * bytestream-ibb.c - Source for GabbleBytestreamIBB
 * Copyright (C) 2007 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "bytestream-ibb.h"

#include <string.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <loudmouth/loudmouth.h>
#include <telepathy-glib/interfaces.h>

#define DEBUG_FLAG GABBLE_DEBUG_BYTESTREAM

#include "base64.h"
#include "bytestream-factory.h"
#include "bytestream-iface.h"
#include "connection.h"
#include "debug.h"
#include "disco.h"
#include "namespaces.h"
#include "util.h"

static void
bytestream_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE (GabbleBytestreamIBB, gabble_bytestream_ibb,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_BYTESTREAM_IFACE,
      bytestream_iface_init));

/* properties */
enum
{
  PROP_CONNECTION = 1,
  PROP_PEER_HANDLE,
  PROP_PEER_HANDLE_TYPE,
  PROP_STREAM_ID,
  PROP_STREAM_INIT_ID,
  PROP_PEER_JID,
  PROP_PEER_RESOURCE,
  PROP_STATE,
  PROP_PROTOCOL,
  PROP_BLOCK_SIZE,
  LAST_PROPERTY
};

#define READ_BUFFER_MAX_SIZE (512 * 1024)

/* the number of not acked stanzas allowed. Once this number reached, we stop
 * sending and wait for acks. */
#define WINDOW_SIZE 10

struct _GabbleBytestreamIBBPrivate
{
  GabbleConnection *conn;
  TpHandle peer_handle;
  gchar *stream_id;
  gchar *stream_init_id;
  gchar *peer_resource;
  GabbleBytestreamState state;
  gchar *peer_jid;
  guint block_size;

  guint16 seq;
  guint16 last_seq_recv;
  LmMessage *close_iq_to_ack;

  /* We can't stop receving IBB data so if user wants to block the bytestream
   * we buffer them until he unblocks it. */
  gboolean read_blocked;
  GString *read_buffer;
  /* list of reffed (LmMessage *) */
  GSList *received_stanzas_not_acked;

  /* (LmMessage *) -> TRUE
   * We don't keep a ref on the LmMessage as we just use this table to track
   * stanzas waiting for reply. The stanza is never used (and so deferenced). */
  GHashTable *sent_stanzas_not_acked;
  GString *write_buffer;
  gboolean write_blocked;

  gboolean dispose_has_run;
};

#define GABBLE_BYTESTREAM_IBB_GET_PRIVATE(obj) ((obj)->priv)

static void
gabble_bytestream_ibb_init (GabbleBytestreamIBB *self)
{
  GabbleBytestreamIBBPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_BYTESTREAM_IBB, GabbleBytestreamIBBPrivate);

  self->priv = priv;

  priv->read_buffer = NULL;
  priv->received_stanzas_not_acked = NULL;

  priv->sent_stanzas_not_acked = g_hash_table_new (g_direct_hash,
      g_direct_equal);
  priv->write_buffer = NULL;
  priv->write_blocked = FALSE;
}

static void
gabble_bytestream_ibb_dispose (GObject *object)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (object);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  tp_handle_unref (contact_repo, priv->peer_handle);

  if (priv->state != GABBLE_BYTESTREAM_STATE_CLOSED)
    {
      gabble_bytestream_iface_close (GABBLE_BYTESTREAM_IFACE (self), NULL);
    }

  if (priv->close_iq_to_ack != NULL)
    {
      _gabble_connection_acknowledge_set_iq (priv->conn, priv->close_iq_to_ack);
      lm_message_unref (priv->close_iq_to_ack);
      priv->close_iq_to_ack = NULL;
    }

  G_OBJECT_CLASS (gabble_bytestream_ibb_parent_class)->dispose (object);
}

static void
gabble_bytestream_ibb_finalize (GObject *object)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (object);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);

  g_free (priv->stream_id);
  g_free (priv->stream_init_id);
  g_free (priv->peer_resource);
  g_free (priv->peer_jid);

  if (priv->read_buffer != NULL)
    g_string_free (priv->read_buffer, TRUE);

  if (priv->write_buffer != NULL)
    g_string_free (priv->write_buffer, TRUE);

  g_hash_table_destroy (priv->sent_stanzas_not_acked);

  G_OBJECT_CLASS (gabble_bytestream_ibb_parent_class)->finalize (object);
}

static void
gabble_bytestream_ibb_get_property (GObject *object,
                                    guint property_id,
                                    GValue *value,
                                    GParamSpec *pspec)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (object);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_CONNECTION:
        g_value_set_object (value, priv->conn);
        break;
      case PROP_PEER_HANDLE:
        g_value_set_uint (value, priv->peer_handle);
        break;
      case PROP_PEER_HANDLE_TYPE:
        g_value_set_uint (value, TP_HANDLE_TYPE_CONTACT);
        break;
      case PROP_STREAM_ID:
        g_value_set_string (value, priv->stream_id);
        break;
      case PROP_STREAM_INIT_ID:
        g_value_set_string (value, priv->stream_init_id);
        break;
      case PROP_PEER_RESOURCE:
        g_value_set_string (value, priv->peer_resource);
        break;
      case PROP_PEER_JID:
        g_value_set_string (value, priv->peer_jid);
        break;
      case PROP_STATE:
        g_value_set_uint (value, priv->state);
        break;
      case PROP_PROTOCOL:
        g_value_set_string (value, NS_IBB);
        break;
      case PROP_BLOCK_SIZE:
        g_value_set_uint (value, priv->block_size);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gabble_bytestream_ibb_set_property (GObject *object,
                                    guint property_id,
                                    const GValue *value,
                                    GParamSpec *pspec)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (object);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_CONNECTION:
        priv->conn = g_value_get_object (value);
        break;
      case PROP_PEER_HANDLE:
        priv->peer_handle = g_value_get_uint (value);
        break;
      case PROP_STREAM_ID:
        g_free (priv->stream_id);
        priv->stream_id = g_value_dup_string (value);
        break;
      case PROP_STREAM_INIT_ID:
        g_free (priv->stream_init_id);
        priv->stream_init_id = g_value_dup_string (value);
        break;
      case PROP_PEER_RESOURCE:
        g_free (priv->peer_resource);
        priv->peer_resource = g_value_dup_string (value);
        break;
      case PROP_STATE:
        if (priv->state != g_value_get_uint (value))
            {
              priv->state = g_value_get_uint (value);
              g_signal_emit_by_name (object, "state-changed", priv->state);
            }
        break;
      case PROP_BLOCK_SIZE:
        priv->block_size = g_value_get_uint (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static GObject *
gabble_bytestream_ibb_constructor (GType type,
                                   guint n_props,
                                   GObjectConstructParam *props)
{
  GObject *obj;
  GabbleBytestreamIBBPrivate *priv;
  TpHandleRepoIface *contact_repo;
  const gchar *jid;

  obj = G_OBJECT_CLASS (gabble_bytestream_ibb_parent_class)->
           constructor (type, n_props, props);

  priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (GABBLE_BYTESTREAM_IBB (obj));

  g_assert (priv->conn != NULL);
  g_assert (priv->peer_handle != 0);
  g_assert (priv->stream_id != NULL);

  contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  tp_handle_ref (contact_repo, priv->peer_handle);

  jid = tp_handle_inspect (contact_repo, priv->peer_handle);

  if (priv->peer_resource != NULL)
    priv->peer_jid = g_strdup_printf ("%s/%s", jid, priv->peer_resource);
  else
    priv->peer_jid = g_strdup (jid);

  return obj;
}

static void
gabble_bytestream_ibb_class_init (
    GabbleBytestreamIBBClass *gabble_bytestream_ibb_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_bytestream_ibb_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gabble_bytestream_ibb_class,
      sizeof (GabbleBytestreamIBBPrivate));

  object_class->dispose = gabble_bytestream_ibb_dispose;
  object_class->finalize = gabble_bytestream_ibb_finalize;

  object_class->get_property = gabble_bytestream_ibb_get_property;
  object_class->set_property = gabble_bytestream_ibb_set_property;
  object_class->constructor = gabble_bytestream_ibb_constructor;

   g_object_class_override_property (object_class, PROP_CONNECTION,
      "connection");
   g_object_class_override_property (object_class, PROP_PEER_HANDLE,
       "peer-handle");
   g_object_class_override_property (object_class, PROP_PEER_HANDLE_TYPE,
       "peer-handle-type");
   g_object_class_override_property (object_class, PROP_STREAM_ID,
       "stream-id");
   g_object_class_override_property (object_class, PROP_PEER_JID,
       "peer-jid");
   g_object_class_override_property (object_class, PROP_STATE,
       "state");
   g_object_class_override_property (object_class, PROP_PROTOCOL,
       "protocol");

  param_spec = g_param_spec_string (
      "peer-resource",
      "Peer resource",
      "the resource used by the remote peer during the SI, if any",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_PEER_RESOURCE,
      param_spec);

  param_spec = g_param_spec_string (
      "stream-init-id",
      "stream init ID",
      "the iq ID of the SI request, if any",
      NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STREAM_INIT_ID,
      param_spec);

  param_spec = g_param_spec_uint (
      "block-size",
      "block size",
      "Maximum data sent using one stanza as described in XEP-0047",
      0, G_MAXUINT32, 4096,
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_BLOCK_SIZE,
      param_spec);
}

static void
change_write_blocked_state (GabbleBytestreamIBB *self,
                            gboolean blocked)
{
  GabbleBytestreamIBBPrivate *priv =
      GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);

  if (priv->write_blocked == blocked)
    return;

  priv->write_blocked = blocked;
  g_signal_emit_by_name (self, "write-blocked", blocked);
}

static void
send_close_stanza (GabbleBytestreamIBB *self)
{
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  LmMessage *msg;

  if (priv->close_iq_to_ack != NULL)
    {
      /* We received a close IQ and just need to ACK it */
      _gabble_connection_acknowledge_set_iq (priv->conn, priv->close_iq_to_ack);
      lm_message_unref (priv->close_iq_to_ack);
      priv->close_iq_to_ack = NULL;
    }

  DEBUG ("send IBB close stanza");

  msg = lm_message_build (priv->peer_jid, LM_MESSAGE_TYPE_IQ,
      '@', "type", "set",
      '(', "close", "",
        '@', "xmlns", NS_IBB,
        '@', "sid", priv->stream_id,
      ')', NULL);

  /* We don't really care about the answer as the bytestream
   * is closed anyway. */
  _gabble_connection_send_with_reply (priv->conn, msg,
      NULL, NULL, NULL, NULL);

  lm_message_unref (msg);
}

static gboolean
send_data (GabbleBytestreamIBB *self, const gchar *str, guint len,
    gboolean *result);

static LmHandlerResult
iq_acked_cb (GabbleConnection *conn,
             LmMessage *sent_msg,
             LmMessage *reply_msg,
             GObject *obj,
             gpointer user_data)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (obj);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);

  g_hash_table_remove (priv->sent_stanzas_not_acked, sent_msg);

  if (priv->write_buffer != NULL)
    {
      guint sent;

      DEBUG ("A stanza has been acked. Try to flush the buffer");

      sent = send_data (self, priv->write_buffer->str, priv->write_buffer->len,
          NULL);
      if (sent == priv->write_buffer->len)
        {
          DEBUG ("buffer has been flushed; unblock write the bytestream");
          g_string_free (priv->write_buffer, TRUE);
          priv->write_buffer = NULL;

          change_write_blocked_state (self, FALSE);

          if (priv->state == GABBLE_BYTESTREAM_STATE_CLOSING)
            {
              DEBUG ("Can close the bystream now the buffer is flushed");
              send_close_stanza (self);
              g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_CLOSED,
                  NULL);
            }
        }
      else
        {
          g_string_erase (priv->write_buffer, 0, sent);

          DEBUG ("buffer has not been completely flushed; %" G_GSIZE_FORMAT
              " bytes left",
              priv->write_buffer->len);
        }
    }

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

static gboolean
send_data (GabbleBytestreamIBB *self,
           const gchar *str,
           guint len,
           gboolean *result)
{
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  guint sent, stanza_count;

  sent = 0;
  stanza_count = 0;
  while (sent < len)
    {
      LmMessage *iq;
      guint send_now, remaining;
      gchar *seq, *encoded;
      GError *error = NULL;
      gboolean ret;
      guint nb_stanzas_waiting;

      remaining = (len - sent);

      nb_stanzas_waiting = g_hash_table_size (priv->sent_stanzas_not_acked);
      if (nb_stanzas_waiting >= WINDOW_SIZE)
        {
          DEBUG ("Window is full (%u). Stop sending stanzas",
              nb_stanzas_waiting);
          break;
        }

      /* We can send stanzas */
      if (remaining > priv->block_size)
        {
          /* We can't send all the remaining data in one stanza */
          send_now = priv->block_size;
        }
      else
        {
          /* Send all the remaining data */
          send_now = remaining;
        }

      encoded = base64_encode (send_now, str + sent, FALSE);
      seq = g_strdup_printf ("%u", priv->seq++);

      iq = lm_message_build (priv->peer_jid, LM_MESSAGE_TYPE_IQ,
          '@', "type", "set",
          '(', "data", encoded,
            '@', "xmlns", NS_IBB,
            '@', "sid", priv->stream_id,
            '@', "seq", seq,
          ')', NULL);

      ret = _gabble_connection_send_with_reply (priv->conn, iq, iq_acked_cb,
          G_OBJECT (self), NULL, &error);

      g_free (encoded);
      g_free (seq);
      lm_message_unref (iq);

      if (!ret)
        {
          DEBUG ("error sending IBB stanza: %s. Close the bytestream",
              error->message);
          g_error_free (error);

          gabble_bytestream_iface_close (GABBLE_BYTESTREAM_IFACE (self), NULL);

          if (result != NULL)
            *result = FALSE;

          return sent;
        }

      g_hash_table_insert (priv->sent_stanzas_not_acked, iq,
          GUINT_TO_POINTER (TRUE));

      DEBUG ("send %d bytes (window size: %u)", send_now,
          nb_stanzas_waiting + 1);

      sent += send_now;
      stanza_count++;
    }

  DEBUG ("sent %d bytes (%d stanzas needed)", sent, stanza_count);

  if (result != NULL)
    *result = TRUE;
  return sent;
}

/*
 * gabble_bytestream_ibb_send
 *
 * Implements gabble_bytestream_iface_send on GabbleBytestreamIface
 */
static gboolean
gabble_bytestream_ibb_send (GabbleBytestreamIface *iface,
                            guint len,
                            const gchar *str)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (iface);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  gboolean result;
  guint sent;

  if (priv->state != GABBLE_BYTESTREAM_STATE_OPEN)
    {
      DEBUG ("can't send data through a not open bytestream (state: %d)",
          priv->state);
      return FALSE;
    }

  if (priv->write_blocked)
    {
      DEBUG ("sending data while the bytestream was blocked");
    }

  if (priv->write_buffer != NULL)
    {
      DEBUG ("Write buffer is not empty. Buffering data");

      g_string_append_len (priv->write_buffer, str, len);
      return TRUE;
    }

  sent = send_data (self, str, len, &result);
  if (sent < len)
    {
      guint remaining;

      DEBUG ("Some data have not been sent. Buffer them and write "
          "block the bytestream");

      remaining = (len - sent);

      if (priv->write_buffer == NULL)
        {
          priv->write_buffer = g_string_new_len (str + sent, remaining);
        }
      else
        {
          g_string_append_len (priv->write_buffer, str + sent, remaining);
        }

      DEBUG ("write buffer size: %" G_GSIZE_FORMAT, priv->write_buffer->len);
      change_write_blocked_state (self, TRUE);
    }

  return result;
}

void
gabble_bytestream_ibb_receive (GabbleBytestreamIBB *self,
                               LmMessage *msg,
                               gboolean is_iq)
{
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  LmMessageNode *data;
  GString *str;
  TpHandle sender;

  /* caller must have checked for this in order to know which bytestream to
   * route this packet to */
  data = lm_message_node_get_child_with_namespace (
    wocky_stanza_get_top_node (msg), "data", NS_IBB);
  g_assert (data != NULL);

  if (priv->state != GABBLE_BYTESTREAM_STATE_OPEN)
    {
      DEBUG ("can't receive data through a not open bytestream (state: %d)",
          priv->state);
      if (is_iq)
        _gabble_connection_send_iq_error (priv->conn, msg,
            XMPP_ERROR_BAD_REQUEST, "IBB bytestream isn't open");
      return;
    }

  /* Private stream using SI - the bytestream factory has already checked
   * the sender in order to dispatch to us */
  sender = priv->peer_handle;

  /* FIXME: check sequence number */

  str = base64_decode (lm_message_node_get_value (data));
  if (str == NULL)
    {
      DEBUG ("base64 decoding failed");
      if (is_iq)
        _gabble_connection_send_iq_error (priv->conn, msg,
            XMPP_ERROR_BAD_REQUEST, "base64 decoding failed");
      return;
    }

  if (priv->read_blocked)
    {
      gsize current_buffer_len = 0;

      DEBUG ("Bytestream is blocked. Buffering data");
      if (priv->read_buffer != NULL)
        current_buffer_len = priv->read_buffer->len;

      if (current_buffer_len + str->len > READ_BUFFER_MAX_SIZE)
        {
          DEBUG ("Buffer is full. Closing the bytestream");

          if (is_iq)
            _gabble_connection_send_iq_error (priv->conn, msg,
                XMPP_ERROR_NOT_ACCEPTABLE, "buffer is full");

          gabble_bytestream_iface_close (GABBLE_BYTESTREAM_IFACE (self), NULL);
          g_string_free (str, TRUE);
          return;
        }

      if (priv->read_buffer == NULL)
        {
          priv->read_buffer = str;
        }
      else
        {
          g_string_append_len (priv->read_buffer, str->str, str->len);
          g_string_free (str, TRUE);
        }

      if (is_iq)
        {
          priv->received_stanzas_not_acked = g_slist_prepend (
              priv->received_stanzas_not_acked, lm_message_ref (msg));
        }

      return;
    }

  g_signal_emit_by_name (G_OBJECT (self), "data-received", sender, str);
  g_string_free (str, TRUE);

  if (is_iq)
    _gabble_connection_acknowledge_set_iq (priv->conn, msg);

  return;
}

/*
 * gabble_bytestream_ibb_accept
 *
 * Implements gabble_bytestream_iface_accept on GabbleBytestreamIface
 */
static void
gabble_bytestream_ibb_accept (GabbleBytestreamIface *iface,
                              GabbleBytestreamAugmentSiAcceptReply func,
                              gpointer user_data)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (iface);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  LmMessage *msg;
  LmMessageNode *si;

  if (priv->state != GABBLE_BYTESTREAM_STATE_LOCAL_PENDING)
    {
      /* The stream was previoulsy or automatically accepted */
      return;
    }

  msg = gabble_bytestream_factory_make_accept_iq (priv->peer_jid,
      priv->stream_init_id, NS_IBB);
  si = lm_message_node_get_child_with_namespace (
      wocky_stanza_get_top_node (msg), "si", NS_SI);
  g_assert (si != NULL);

  if (func != NULL)
    {
      /* let the caller add his profile specific data */
      func (si, user_data);
    }

  if (_gabble_connection_send (priv->conn, msg, NULL))
    {
      DEBUG ("stream %s with %s is now accepted", priv->stream_id,
          priv->peer_jid);
      g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_ACCEPTED, NULL);
    }

  lm_message_unref (msg);
}

static void
gabble_bytestream_ibb_decline (GabbleBytestreamIBB *self,
                               GError *error)
{
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  LmMessage *msg;

  g_return_if_fail (priv->state == GABBLE_BYTESTREAM_STATE_LOCAL_PENDING);

  msg = lm_message_build (priv->peer_jid, LM_MESSAGE_TYPE_IQ,
      '@', "type", "error",
      '@', "id", priv->stream_init_id,
      NULL);

  if (error != NULL && error->domain == GABBLE_XMPP_ERROR)
    {
      gabble_xmpp_error_to_node (error->code,
        wocky_stanza_get_top_node (msg), error->message);
    }
  else
    {
      gabble_xmpp_error_to_node (XMPP_ERROR_FORBIDDEN,
          wocky_stanza_get_top_node (msg),
          "Offer Declined");
    }

  _gabble_connection_send (priv->conn, msg, NULL);

  lm_message_unref (msg);

  g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_CLOSED, NULL);
}

/*
 * gabble_bytestream_ibb_close
 *
 * Implements gabble_bytestream_iface_close on GabbleBytestreamIface
 */
static void
gabble_bytestream_ibb_close (GabbleBytestreamIface *iface,
                             GError *error)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (iface);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  GSList *l;

  if (priv->state == GABBLE_BYTESTREAM_STATE_CLOSED)
     /* bytestream already closed, do nothing */
     return;

  /* Send error for pending IQ's */
  priv->received_stanzas_not_acked = g_slist_reverse (
      priv->received_stanzas_not_acked);

  for (l = priv->received_stanzas_not_acked; l != NULL;
      l = g_slist_next (l))
    {
      LmMessage *iq = (LmMessage *) l->data;

      _gabble_connection_send_iq_error (priv->conn, iq,
          XMPP_ERROR_ITEM_NOT_FOUND, NULL);

      lm_message_unref (iq);
    }

  g_slist_free (priv->received_stanzas_not_acked);
  priv->received_stanzas_not_acked = NULL;

  if (priv->state == GABBLE_BYTESTREAM_STATE_LOCAL_PENDING)
    {
      /* Stream was created using SI so we decline the request */
      gabble_bytestream_ibb_decline (self, error);
    }
  else
    {
      if (priv->write_buffer != NULL)
        {
          DEBUG ("write buffer is not empty. Wait before sending close stanza");
          g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_CLOSING, NULL);
        }
      else
        {
          send_close_stanza (self);
          g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_CLOSED, NULL);
        }
    }
}

static LmHandlerResult
ibb_init_reply_cb (GabbleConnection *conn,
                   LmMessage *sent_msg,
                   LmMessage *reply_msg,
                   GObject *obj,
                   gpointer user_data)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (obj);

  if (lm_message_get_sub_type (reply_msg) == LM_MESSAGE_SUB_TYPE_RESULT)
    {
      /* yeah, stream initiated */
      DEBUG ("IBB stream initiated");
      g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_OPEN, NULL);
    }
  else
    {
      DEBUG ("error during IBB initiation");
      g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_CLOSED, NULL);
    }

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

/*
 * gabble_bytestream_ibb_initiate
 *
 * Implements gabble_bytestream_iface_initiate on GabbleBytestreamIface
 */
static gboolean
gabble_bytestream_ibb_initiate (GabbleBytestreamIface *iface)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (iface);
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);
  LmMessage *msg;
  gchar *block_size;

  if (priv->state != GABBLE_BYTESTREAM_STATE_INITIATING)
    {
      DEBUG ("bytestream is not is the initiating state (state %d",
          priv->state);
      return FALSE;
    }

  block_size = g_strdup_printf ("%u", priv->block_size);
  msg = lm_message_build (priv->peer_jid, LM_MESSAGE_TYPE_IQ,
      '@', "type", "set",
      '(', "open", "",
        '@', "xmlns", NS_IBB,
        '@', "sid", priv->stream_id,
        '@', "block-size", block_size,
      ')', NULL);
  g_free (block_size);

  if (!_gabble_connection_send_with_reply (priv->conn, msg,
      ibb_init_reply_cb, G_OBJECT (self), NULL, NULL))
    {
      DEBUG ("Error when sending IBB init stanza");

      lm_message_unref (msg);
      return FALSE;
    }

  lm_message_unref (msg);

  return TRUE;
}

static void
gabble_bytestream_ibb_block_reading (GabbleBytestreamIface *iface,
                                     gboolean block)
{
  GabbleBytestreamIBB *self = GABBLE_BYTESTREAM_IBB (iface);
  GabbleBytestreamIBBPrivate *priv =
      GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);

  if (priv->read_blocked == block)
    return;

  priv->read_blocked = block;

  DEBUG ("%s the transport bytestream", block ? "block": "unblock");

  if (priv->read_buffer != NULL && !block)
    {
      GSList *l;

      DEBUG ("Bytestream unblocked, flushing the buffer");

      g_signal_emit_by_name (G_OBJECT (self), "data-received",
          priv->peer_handle, priv->read_buffer);

      g_string_free (priv->read_buffer, TRUE);
      priv->read_buffer = NULL;

      /* ack pending stanzas */
      priv->received_stanzas_not_acked = g_slist_reverse (
          priv->received_stanzas_not_acked);

      for (l = priv->received_stanzas_not_acked; l != NULL;
          l = g_slist_next (l))
        {
          LmMessage *iq = (LmMessage *) l->data;

          _gabble_connection_acknowledge_set_iq (priv->conn, iq);

          lm_message_unref (iq);
        }

      g_slist_free (priv->received_stanzas_not_acked);
      priv->received_stanzas_not_acked = NULL;
    }
}

void
gabble_bytestream_ibb_close_received (GabbleBytestreamIBB *self,
                                      LmMessage *iq)
{
  GabbleBytestreamIBBPrivate *priv = GABBLE_BYTESTREAM_IBB_GET_PRIVATE (self);

  DEBUG ("received IBB close stanza. Closing bytestream");

  priv->close_iq_to_ack = lm_message_ref (iq);
  gabble_bytestream_ibb_close (GABBLE_BYTESTREAM_IFACE (self), NULL);
}

static void
bytestream_iface_init (gpointer g_iface,
                       gpointer iface_data)
{
  GabbleBytestreamIfaceClass *klass = (GabbleBytestreamIfaceClass *) g_iface;

  klass->initiate = gabble_bytestream_ibb_initiate;
  klass->send = gabble_bytestream_ibb_send;
  klass->close = gabble_bytestream_ibb_close;
  klass->accept = gabble_bytestream_ibb_accept;
  klass->block_reading = gabble_bytestream_ibb_block_reading;
}
