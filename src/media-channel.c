/*
 * gabble-media-channel.c - Source for GabbleMediaChannel
 * Copyright (C) 2006 Collabora Ltd.
 * Copyright (C) 2006 Nokia Corporation
 *   @author Ole Andre Vadla Ravnaas <ole.andre.ravnaas@collabora.co.uk>
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
#include "media-channel.h"


#include <dbus/dbus-glib.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/errors.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/channel-iface.h>
#include <telepathy-glib/svc-channel.h>
#include <telepathy-glib/svc-properties-interface.h>

#define DEBUG_FLAG GABBLE_DEBUG_MEDIA

#include "connection.h"
#include "debug.h"
#include "media-factory.h"
#include "media-session.h"
#include "media-session.h"
#include "media-stream.h"
#include "presence-cache.h"
#include "presence.h"

static void call_state_iface_init (gpointer, gpointer);
static void channel_iface_init (gpointer, gpointer);
static void hold_iface_init (gpointer, gpointer);
static void media_signalling_iface_init (gpointer, gpointer);
static void streamed_media_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE (GabbleMediaChannel, gabble_media_channel,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL, channel_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_CALL_STATE,
      call_state_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_GROUP,
      tp_group_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_HOLD,
      hold_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_MEDIA_SIGNALLING,
      media_signalling_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_TYPE_STREAMED_MEDIA,
      streamed_media_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_PROPERTIES_INTERFACE,
      tp_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_CHANNEL_IFACE, NULL));

static const gchar *gabble_media_channel_interfaces[] = {
    /* FIXME: our implementation of CallState is a stub, so it doesn't
    appear in GetInterfaces' output to avoid confusing clients
    TP_IFACE_CHANNEL_INTERFACE_CALL_STATE,
    */
    TP_IFACE_CHANNEL_INTERFACE_GROUP,
    TP_IFACE_CHANNEL_INTERFACE_HOLD,
    TP_IFACE_CHANNEL_INTERFACE_MEDIA_SIGNALLING,
    TP_IFACE_PROPERTIES_INTERFACE,
    NULL
};

/* properties */
enum
{
  PROP_OBJECT_PATH = 1,
  PROP_CHANNEL_TYPE,
  PROP_HANDLE_TYPE,
  PROP_HANDLE,
  PROP_CONNECTION,
  PROP_CREATOR,
  PROP_FACTORY,
  PROP_INTERFACES,
  /* TP properties (see also below) */
  PROP_NAT_TRAVERSAL,
  PROP_STUN_SERVER,
  PROP_STUN_PORT,
  PROP_GTALK_P2P_RELAY_TOKEN,
  LAST_PROPERTY
};

/* TP properties */
enum
{
  CHAN_PROP_NAT_TRAVERSAL = 0,
  CHAN_PROP_STUN_SERVER,
  CHAN_PROP_STUN_PORT,
  CHAN_PROP_GTALK_P2P_RELAY_TOKEN,
  NUM_CHAN_PROPS,
  INVALID_CHAN_PROP
};

const TpPropertySignature channel_property_signatures[NUM_CHAN_PROPS] = {
      { "nat-traversal",          G_TYPE_STRING },
      { "stun-server",            G_TYPE_STRING },
      { "stun-port",              G_TYPE_UINT   },
      { "gtalk-p2p-relay-token",  G_TYPE_STRING }
};

struct _GabbleMediaChannelPrivate
{
  GabbleConnection *conn;
  gchar *object_path;
  TpHandle creator;

  GabbleMediaFactory *factory;
  GabbleMediaSession *session;
  GPtrArray *streams;

  guint next_stream_id;

  TpLocalHoldState hold_state;
  TpLocalHoldStateReason hold_state_reason;

  gboolean closed:1;
  gboolean dispose_has_run:1;
};

#define GABBLE_MEDIA_CHANNEL_GET_PRIVATE(obj) (obj->priv)

static void
gabble_media_channel_init (GabbleMediaChannel *self)
{
  GabbleMediaChannelPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_MEDIA_CHANNEL, GabbleMediaChannelPrivate);

  self->priv = priv;

  priv->next_stream_id = 1;

  /* initialize properties mixin */
  tp_properties_mixin_init (G_OBJECT (self), G_STRUCT_OFFSET (
        GabbleMediaChannel, properties));
}

static GObject *
gabble_media_channel_constructor (GType type, guint n_props,
                                  GObjectConstructParam *props)
{
  GObject *obj;
  GabbleMediaChannelPrivate *priv;
  TpBaseConnection *conn;
  DBusGConnection *bus;
  TpIntSet *set;
  TpHandleRepoIface *contact_handles;

  obj = G_OBJECT_CLASS (gabble_media_channel_parent_class)->
           constructor (type, n_props, props);

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (GABBLE_MEDIA_CHANNEL (obj));
  conn = (TpBaseConnection *) priv->conn;
  contact_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  /* register object on the bus */
  bus = tp_get_bus ();
  dbus_g_connection_register_g_object (bus, priv->object_path, obj);

  tp_group_mixin_init (obj, G_STRUCT_OFFSET (GabbleMediaChannel, group),
      contact_handles, conn->self_handle);

  /* automatically add creator to channel */
  set = tp_intset_new ();
  tp_intset_add (set, priv->creator);

  tp_group_mixin_change_members (obj, "", set, NULL, NULL, NULL, 0, 0);

  tp_intset_destroy (set);

  /* Allow member adding; also, we implement the 0.17.6 properties correctly */
  tp_group_mixin_change_flags (obj,
      TP_CHANNEL_GROUP_FLAG_CAN_ADD | TP_CHANNEL_GROUP_FLAG_PROPERTIES, 0);

  return obj;
}

static void session_state_changed_cb (GabbleMediaSession *session,
    GParamSpec *arg1, GabbleMediaChannel *channel);
static void session_stream_added_cb (GabbleMediaSession *session,
    GabbleMediaStream  *stream, GabbleMediaChannel *chan);
static void session_terminated_cb (GabbleMediaSession *session,
    guint terminator, guint reason, gpointer user_data);

/**
 * create_session
 *
 * Creates a GabbleMediaSession object for given peer.
 *
 * If sid is set to NULL a unique sid is generated and
 * the "initiator" property of the newly created
 * GabbleMediaSession is set to our own handle.
 */
static GabbleMediaSession *
create_session (GabbleMediaChannel *channel,
                TpHandle peer,
                const gchar *peer_resource,
                const gchar *sid,
                GError **error)
{
  GabbleMediaChannelPrivate *priv;
  GabbleMediaSession *session;
  gchar *object_path;
  JingleInitiator initiator;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (channel));

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (channel);

  g_assert (priv->session == NULL);

  object_path = g_strdup_printf ("%s/MediaSession%u", priv->object_path, peer);

  if (sid == NULL)
    {
      /* We are the initiator */
      GabblePresence *presence;
#ifdef ENABLE_DEBUG
      TpBaseConnection *conn = (TpBaseConnection *) priv->conn;
      TpHandleRepoIface *contact_handles = tp_base_connection_get_handles (
          conn, TP_HANDLE_TYPE_CONTACT);
#endif

      initiator = INITIATOR_LOCAL;

      presence = gabble_presence_cache_get (priv->conn->presence_cache, peer);

      if (presence == NULL)
        {
          DEBUG ("failed to add contact %d (%s) to media channel: "
              "no presence available", peer,
              tp_handle_inspect (contact_handles, peer));
          goto NO_CAPS;
        }

      if ((_gabble_media_channel_caps_to_typeflags (presence->caps) &
            (TP_CHANNEL_MEDIA_CAPABILITY_AUDIO |
             TP_CHANNEL_MEDIA_CAPABILITY_VIDEO)) == 0)
        {
          DEBUG ("failed to add contact %d (%s) to media channel: "
              "caps %x aren't sufficient", peer,
              tp_handle_inspect (contact_handles, peer),
              presence->caps);
          goto NO_CAPS;
        }

      sid = _gabble_media_factory_allocate_sid (priv->factory, channel);
    }
  else
    {
      initiator = INITIATOR_REMOTE;
      _gabble_media_factory_register_sid (priv->factory, sid, channel);
    }

  session = g_object_new (GABBLE_TYPE_MEDIA_SESSION,
                          "connection", priv->conn,
                          "media-channel", channel,
                          "object-path", object_path,
                          "session-id", sid,
                          "initiator", initiator,
                          "peer", peer,
                          "peer-resource", peer_resource,
                          NULL);

  g_signal_connect (session, "notify::state",
                    (GCallback) session_state_changed_cb, channel);
  g_signal_connect (session, "stream-added",
                    (GCallback) session_stream_added_cb, channel);
  g_signal_connect (session, "terminated",
                    (GCallback) session_terminated_cb, channel);

  priv->session = session;

  priv->streams = g_ptr_array_sized_new (1);

  tp_svc_channel_interface_media_signalling_emit_new_session_handler (
      channel, object_path, "rtp");

  g_free (object_path);

  return session;

NO_CAPS:
  g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
      "handle %u has no media capabilities", peer);
  return NULL;
}

gboolean
_gabble_media_channel_dispatch_session_action (GabbleMediaChannel *chan,
                                               TpHandle peer,
                                               const gchar *peer_resource,
                                               const gchar *sid,
                                               LmMessage *message,
                                               LmMessageNode *session_node,
                                               const gchar *action,
                                               GError **error)
{
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);
  GabbleMediaSession *session = priv->session;
  gboolean session_is_new = FALSE;

  /* If this assertion fails, create_session() would think we're the
   * initiator. However, GabbleMediaFactory checks this, so it can't fail */
  g_return_val_if_fail (sid != NULL, FALSE);

  if (session == NULL)
    {
      TpGroupMixin *mixin = TP_GROUP_MIXIN (chan);
      TpIntSet *set;

      session = create_session (chan, peer, peer_resource, sid, NULL);
      g_assert (session != NULL);
      session_is_new = TRUE;

      /* make us local pending */
      set = tp_intset_new ();
      tp_intset_add (set, mixin->self_handle);

      tp_group_mixin_change_members ((GObject *) chan,
          "", NULL, NULL, set, NULL, peer, 0);

      tp_intset_destroy (set);

      /* and update flags accordingly */
      tp_group_mixin_change_flags ((GObject *) chan,
          TP_CHANNEL_GROUP_FLAG_CAN_ADD | TP_CHANNEL_GROUP_FLAG_CAN_REMOVE,
          0);
    }

  g_object_ref (session);

  if (_gabble_media_session_handle_action (session, message, session_node,
        action, error))
    {
      g_object_unref (session);
      return TRUE;
    }
  else
    {
      if (session_is_new)
        _gabble_media_session_terminate (session, INITIATOR_LOCAL,
            TP_CHANNEL_GROUP_CHANGE_REASON_ERROR);

      g_object_unref (session);
      return FALSE;
    }
}

static void
gabble_media_channel_get_property (GObject    *object,
                                   guint       property_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (object);
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);
  const gchar *param_name;
  guint tp_property_id;

  switch (property_id) {
    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;
    case PROP_CHANNEL_TYPE:
      g_value_set_static_string (value, TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA);
      break;
    case PROP_HANDLE_TYPE:
      g_value_set_uint (value, TP_HANDLE_TYPE_NONE);
      break;
    case PROP_HANDLE:
      g_value_set_uint (value, 0);
      break;
    case PROP_CONNECTION:
      g_value_set_object (value, priv->conn);
      break;
    case PROP_CREATOR:
      g_value_set_uint (value, priv->creator);
      break;
    case PROP_FACTORY:
      g_value_set_object (value, priv->factory);
      break;
    case PROP_INTERFACES:
      g_value_set_boxed (value, gabble_media_channel_interfaces);
      break;
    default:
      param_name = g_param_spec_get_name (pspec);

      if (tp_properties_mixin_has_property (object, param_name,
            &tp_property_id))
        {
          GValue *tp_property_value =
            chan->properties.properties[tp_property_id].value;

          if (tp_property_value)
            {
              g_value_copy (tp_property_value, value);
              return;
            }
        }

      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gabble_media_channel_set_property (GObject     *object,
                                   guint        property_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (object);
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);
  const gchar *param_name;
  guint tp_property_id;

  switch (property_id) {
    case PROP_OBJECT_PATH:
      g_free (priv->object_path);
      priv->object_path = g_value_dup_string (value);
      break;
    case PROP_HANDLE_TYPE:
    case PROP_HANDLE:
    case PROP_CHANNEL_TYPE:
      /* these properties are writable in the interface, but not actually
       * meaningfully changable on this channel, so we do nothing */
      break;
    case PROP_CONNECTION:
      priv->conn = g_value_get_object (value);
      break;
    case PROP_CREATOR:
      priv->creator = g_value_get_uint (value);
      break;
    case PROP_FACTORY:
      priv->factory = g_value_get_object (value);
      break;
    default:
      param_name = g_param_spec_get_name (pspec);

      if (tp_properties_mixin_has_property (object, param_name,
            &tp_property_id))
        {
          tp_properties_mixin_change_value (object, tp_property_id, value,
                                                NULL);
          tp_properties_mixin_change_flags (object, tp_property_id,
                                                TP_PROPERTY_FLAG_READ,
                                                0, NULL);

          return;
        }

      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void gabble_media_channel_dispose (GObject *object);
static void gabble_media_channel_finalize (GObject *object);
static gboolean gabble_media_channel_remove_member (GObject *obj,
    TpHandle handle, const gchar *message, GError **error);

static void
gabble_media_channel_class_init (GabbleMediaChannelClass *gabble_media_channel_class)
{
  static TpDBusPropertiesMixinPropImpl channel_props[] = {
      { "TargetHandleType", "handle-type", NULL },
      { "TargetHandle", "handle", NULL },
      { "ChannelType", "channel-type", NULL },
      { "Interfaces", "interfaces", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TP_IFACE_CHANNEL,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        channel_props,
      },
      { NULL }
  };
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_media_channel_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gabble_media_channel_class,
      sizeof (GabbleMediaChannelPrivate));

  object_class->constructor = gabble_media_channel_constructor;

  object_class->get_property = gabble_media_channel_get_property;
  object_class->set_property = gabble_media_channel_set_property;

  object_class->dispose = gabble_media_channel_dispose;
  object_class->finalize = gabble_media_channel_finalize;

  g_object_class_override_property (object_class, PROP_OBJECT_PATH,
      "object-path");
  g_object_class_override_property (object_class, PROP_CHANNEL_TYPE,
      "channel-type");
  g_object_class_override_property (object_class, PROP_HANDLE_TYPE,
      "handle-type");
  g_object_class_override_property (object_class, PROP_HANDLE, "handle");

  param_spec = g_param_spec_object ("connection", "GabbleConnection object",
      "Gabble connection object that owns this media channel object.",
      GABBLE_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_uint ("creator", "Channel creator",
      "The TpHandle representing the contact who created the channel.",
      0, G_MAXUINT32, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CREATOR, param_spec);

  param_spec = g_param_spec_object ("factory", "GabbleMediaFactory object",
      "The factory that created this object.",
      GABBLE_TYPE_MEDIA_FACTORY,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_FACTORY, param_spec);

  param_spec = g_param_spec_boxed ("interfaces", "Extra D-Bus interfaces",
      "Additional Channel.Interface.* interfaces",
      G_TYPE_STRV,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INTERFACES, param_spec);

  param_spec = g_param_spec_string ("nat-traversal", "NAT traversal",
      "NAT traversal mechanism.",
      "gtalk-p2p",
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_NAT_TRAVERSAL,
      param_spec);

  param_spec = g_param_spec_string ("stun-server", "STUN server",
      "IP or address of STUN server.",
      NULL,
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_STUN_SERVER, param_spec);

  param_spec = g_param_spec_uint ("stun-port", "STUN port",
      "UDP port of STUN server.",
      0, G_MAXUINT16, 0,
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_STUN_PORT, param_spec);

  param_spec = g_param_spec_string ("gtalk-p2p-relay-token",
      "GTalk P2P Relay Token",
      "Magic token to authenticate with the Google Talk relay server.",
      NULL,
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_GTALK_P2P_RELAY_TOKEN,
      param_spec);

  tp_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMediaChannelClass, properties_class),
      channel_property_signatures, NUM_CHAN_PROPS, NULL);

  gabble_media_channel_class->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMediaChannelClass, dbus_props_class));

  tp_group_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMediaChannelClass, group_class),
      _gabble_media_channel_add_member,
      gabble_media_channel_remove_member);
  tp_group_mixin_init_dbus_properties (object_class);
}

void
gabble_media_channel_dispose (GObject *object)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (object);
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  /** In this we set the state to ENDED, then the callback unrefs
   * the session
   */

  if (!priv->closed)
    gabble_media_channel_close (self);

  g_assert (priv->closed);
  g_assert (priv->session == NULL);
  g_assert (priv->streams == NULL);

  if (G_OBJECT_CLASS (gabble_media_channel_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_media_channel_parent_class)->dispose (object);
}

void
gabble_media_channel_finalize (GObject *object)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (object);
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);

  g_free (priv->object_path);

  tp_group_mixin_finalize (object);
  tp_properties_mixin_finalize (object);

  G_OBJECT_CLASS (gabble_media_channel_parent_class)->finalize (object);
}


/**
 * gabble_media_channel_close
 *
 * Implements D-Bus method Close
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_media_channel_close_async (TpSvcChannel *iface,
                                  DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);

  gabble_media_channel_close (self);
  tp_svc_channel_return_from_close (context);
}

void
gabble_media_channel_close (GabbleMediaChannel *self)
{
  GabbleMediaChannelPrivate *priv;

  DEBUG ("called on %p", self);

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);

  if (priv->closed)
    {
      return;
    }

  priv->closed = TRUE;

  if (priv->session)
    {
      _gabble_media_session_terminate (priv->session, INITIATOR_LOCAL,
          TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
    }

  tp_svc_channel_emit_closed (self);
}


/**
 * gabble_media_channel_get_channel_type
 *
 * Implements D-Bus method GetChannelType
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_media_channel_get_channel_type (TpSvcChannel *iface,
                                       DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_channel_type (context,
      TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA);
}


/**
 * gabble_media_channel_get_handle
 *
 * Implements D-Bus method GetHandle
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_media_channel_get_handle (TpSvcChannel *iface,
                                 DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_handle (context, 0, 0);
}


/**
 * gabble_media_channel_get_interfaces
 *
 * Implements D-Bus method GetInterfaces
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_media_channel_get_interfaces (TpSvcChannel *iface,
                                     DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_interfaces (context,
      gabble_media_channel_interfaces);
}


/**
 * gabble_media_channel_get_session_handlers
 *
 * Implements D-Bus method GetSessionHandlers
 * on interface org.freedesktop.Telepathy.Channel.Interface.MediaSignalling
 */
static void
gabble_media_channel_get_session_handlers (TpSvcChannelInterfaceMediaSignalling *iface,
                                           DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GPtrArray *ret;
  GType info_type = TP_STRUCT_TYPE_MEDIA_SESSION_HANDLER_INFO;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);

  if (priv->session)
    {
      GValue handler = { 0, };
      TpHandle member;
      gchar *path;

      g_value_init (&handler, info_type);
      g_value_take_boxed (&handler,
          dbus_g_type_specialized_construct (info_type));

      g_object_get (priv->session,
                    "peer", &member,
                    "object-path", &path,
                    NULL);

      dbus_g_type_struct_set (&handler,
          0, path,
          1, "rtp",
          G_MAXUINT);

      g_free (path);

      ret = g_ptr_array_sized_new (1);
      g_ptr_array_add (ret, g_value_get_boxed (&handler));
    }
  else
    {
      ret = g_ptr_array_sized_new (0);
    }

  tp_svc_channel_interface_media_signalling_return_from_get_session_handlers (
      context, ret);
  g_ptr_array_free (ret, TRUE);
}


static GPtrArray *
make_stream_list (GabbleMediaChannel *self,
                  GPtrArray *streams)
{
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);
  GPtrArray *ret;
  guint i;
  GType info_type = TP_STRUCT_TYPE_MEDIA_STREAM_INFO;

  ret = g_ptr_array_sized_new (streams->len);

  for (i = 0; i < streams->len; i++)
    {
      GabbleMediaStream *stream = g_ptr_array_index (streams, i);
      GValue entry = { 0, };
      guint id;
      TpHandle peer;
      TpMediaStreamType type;
      TpMediaStreamState connection_state;
      CombinedStreamDirection combined_direction;

      g_object_get (stream,
          "id", &id,
          "media-type", &type,
          "connection-state", &connection_state,
          "combined-direction", &combined_direction,
          NULL);

      g_object_get (priv->session, "peer", &peer, NULL);

      g_value_init (&entry, info_type);
      g_value_take_boxed (&entry,
          dbus_g_type_specialized_construct (info_type));

      dbus_g_type_struct_set (&entry,
          0, id,
          1, peer,
          2, type,
          3, connection_state,
          4, COMBINED_DIRECTION_GET_DIRECTION (combined_direction),
          5, COMBINED_DIRECTION_GET_PENDING_SEND (combined_direction),
          G_MAXUINT);

      g_ptr_array_add (ret, g_value_get_boxed (&entry));
    }

  return ret;
}

/**
 * gabble_media_channel_list_streams
 *
 * Implements D-Bus method ListStreams
 * on interface org.freedesktop.Telepathy.Channel.Type.StreamedMedia
 */
static void
gabble_media_channel_list_streams (TpSvcChannelTypeStreamedMedia *iface,
                                   DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GPtrArray *ret;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);

  /* no session yet? return an empty array */
  if (priv->session == NULL)
    {
      ret = g_ptr_array_new ();
    }
  else
    {
      ret = make_stream_list (self, priv->streams);
    }

  tp_svc_channel_type_streamed_media_return_from_list_streams (context, ret);
  g_ptr_array_free (ret, TRUE);
}


static GabbleMediaStream *
_find_stream_by_id (GabbleMediaChannel *chan, guint stream_id)
{
  GabbleMediaChannelPrivate *priv;
  guint i;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (chan));

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);

  for (i = 0; i < priv->streams->len; i++)
    {
      GabbleMediaStream *stream = g_ptr_array_index (priv->streams, i);
      guint id;

      g_object_get (stream, "id", &id, NULL);
      if (id == stream_id)
        return stream;
    }

  return NULL;
}

/**
 * gabble_media_channel_remove_streams
 *
 * Implements DBus method RemoveStreams
 * on interface org.freedesktop.Telepathy.Channel.Type.StreamedMedia
 */
static void
gabble_media_channel_remove_streams (TpSvcChannelTypeStreamedMedia *iface,
                                     const GArray * streams,
                                     DBusGMethodInvocation *context)
{
  GabbleMediaChannel *obj = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GPtrArray *stream_objs;
  GError *error = NULL;
  guint i;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (obj));

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (obj);

  stream_objs = g_ptr_array_sized_new (streams->len);

  /* check that all stream ids are valid and at the same time build an array
   * of stream objects so we don't have to look them up again after verifying
   * all stream identifiers. */
  for (i = 0; i < streams->len; i++)
    {
      guint id = g_array_index (streams, guint, i);
      GabbleMediaStream *stream;
      guint j;

      stream = _find_stream_by_id (obj, id);
      if (stream == NULL)
        {
          g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
              "given stream id %u does not exist", id);
          goto OUT;
        }

      /* make sure we don't allow the client to repeatedly remove the same
      stream */
      for (j = 0; j < stream_objs->len; j++)
        {
          GabbleMediaStream *tmp = g_ptr_array_index (stream_objs, j);

          if (tmp == stream)
            {
              stream = NULL;
              break;
            }
        }

      if (stream != NULL)
        g_ptr_array_add (stream_objs, stream);
    }

  /* groovy, it's all good dude, let's remove them */
  if (stream_objs->len > 0)
    _gabble_media_session_remove_streams (priv->session, (GabbleMediaStream **)
        stream_objs->pdata, stream_objs->len);

OUT:
  g_ptr_array_free (stream_objs, TRUE);

  if (error)
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
    }
  else
    {
      tp_svc_channel_type_streamed_media_return_from_remove_streams (context);
    }
}


/**
 * gabble_media_channel_request_stream_direction
 *
 * Implements D-Bus method RequestStreamDirection
 * on interface org.freedesktop.Telepathy.Channel.Type.StreamedMedia
 */
static void
gabble_media_channel_request_stream_direction (TpSvcChannelTypeStreamedMedia *iface,
                                               guint stream_id,
                                               guint stream_direction,
                                               DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GabbleMediaStream *stream;
  GError *error = NULL;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);

  if (stream_direction > TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL)
    {
      g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "given stream direction %u is not valid", stream_direction);
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  stream = _find_stream_by_id (self, stream_id);
  if (stream == NULL)
    {
      g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "given stream id %u does not exist", stream_id);
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  /* streams with no session? I think not... */
  g_assert (priv->session != NULL);

  if (_gabble_media_session_request_stream_direction (priv->session, stream,
        stream_direction, &error))
    {
      tp_svc_channel_type_streamed_media_return_from_request_stream_direction (
          context);
    }
  else
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
    }
}


/**
 * gabble_media_channel_request_streams
 *
 * Implements D-Bus method RequestStreams
 * on interface org.freedesktop.Telepathy.Channel.Type.StreamedMedia
 */
static void
gabble_media_channel_request_streams (TpSvcChannelTypeStreamedMedia *iface,
                                      guint contact_handle,
                                      const GArray *types,
                                      DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  TpBaseConnection *conn;
  GPtrArray *streams;
  GError *error = NULL;
  GPtrArray *ret;
  TpHandleRepoIface *contact_handles;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  /* FIXME: disallow this if we've put the other guy on hold? */

  priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);
  conn = (TpBaseConnection *) priv->conn;
  contact_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  if (!tp_handle_is_valid (contact_handles, contact_handle, &error))
    goto error;

  if (priv->session == NULL)
    {
      if (create_session (self, contact_handle, NULL, NULL, &error)
          == NULL)
        {
          dbus_g_method_return_error (context, error);
          g_error_free (error);
          return;
        }
    }
  else
    {
      TpHandle peer;

      g_object_get (priv->session,
          "peer", &peer,
          NULL);

      if (peer != contact_handle)
        {
          g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "cannot add streams for %u: this channel's peer is %u",
              contact_handle, peer);
          goto error;
        }
    }

  g_assert (priv->session != NULL);

  if (!_gabble_media_session_request_streams (priv->session, types, &streams,
        &error))
    goto error;

  ret = make_stream_list (self, streams);

  g_ptr_array_free (streams, TRUE);

  tp_svc_channel_type_streamed_media_return_from_request_streams (context, ret);
  g_ptr_array_free (ret, TRUE);
  return;

error:
  dbus_g_method_return_error (context, error);
  g_error_free (error);
}


gboolean
_gabble_media_channel_add_member (GObject *obj,
                                  TpHandle handle,
                                  const gchar *message,
                                  GError **error)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (obj);
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);
  TpGroupMixin *mixin = TP_GROUP_MIXIN (obj);

  /* did we create this channel? */
  if (priv->creator == mixin->self_handle)
    {
      TpIntSet *set;

      /* yes: invite the peer */

      if (priv->session == NULL)
        {
          /* create a new session */
          if (create_session (chan, handle, NULL, NULL, error) == NULL)
            return FALSE;
        }
      else
        {
          TpHandle peer;

          g_object_get (priv->session,
              "peer", &peer,
              NULL);

          if (peer != handle)
            {
              g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
                  "handle %u cannot be added: this channel's peer is %u",
                  handle, peer);
              return FALSE;
            }
        }

      /* make the peer remote pending */
      set = tp_intset_new ();
      tp_intset_add (set, handle);

      tp_group_mixin_change_members (obj, "", NULL, NULL, NULL, set, 0, 0);

      tp_intset_destroy (set);

      /* and update flags accordingly */
      tp_group_mixin_change_flags (obj,
          TP_CHANNEL_GROUP_FLAG_CAN_REMOVE | TP_CHANNEL_GROUP_FLAG_CAN_RESCIND,
          TP_CHANNEL_GROUP_FLAG_CAN_ADD);

      return TRUE;
    }
  else
    {
      /* no: has a session been created, is the handle being added ours,
       *     and are we in local pending? */

      if (priv->session &&
          handle == mixin->self_handle &&
          tp_handle_set_is_member (mixin->local_pending, handle))
        {
          /* yes: accept the request */

          TpIntSet *set;

          /* make us a member */
          set = tp_intset_new ();
          tp_intset_add (set, handle);

          tp_group_mixin_change_members (obj,
              "", set, NULL, NULL, NULL, 0, 0);

          tp_intset_destroy (set);

          /* update flags */
          tp_group_mixin_change_flags (obj,
              0, TP_CHANNEL_GROUP_FLAG_CAN_ADD);

          /* signal acceptance */
          _gabble_media_session_accept (priv->session);

          return TRUE;
        }
    }

  g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
      "handle %u cannot be added in the current state", handle);
  return FALSE;
}

static gboolean
gabble_media_channel_remove_member (GObject *obj,
                                    TpHandle handle,
                                    const gchar *message,
                                    GError **error)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (obj);
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);
  TpGroupMixin *mixin = TP_GROUP_MIXIN (obj);
  TpIntSet *set;

  if (priv->session == NULL)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "handle %u cannot be removed in the current state", handle);

      return FALSE;
    }

  if (priv->creator != mixin->self_handle &&
      handle != mixin->self_handle)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_PERMISSION_DENIED,
          "handle %u cannot be removed because you are not the creator of the"
          " channel", handle);

      return FALSE;
    }

  _gabble_media_session_terminate (priv->session, INITIATOR_LOCAL,
      TP_CHANNEL_GROUP_CHANGE_REASON_NONE);

  /* remove the member */
  set = tp_intset_new ();
  tp_intset_add (set, handle);

  tp_group_mixin_change_members (obj, "", NULL, set, NULL, NULL, 0, 0);

  tp_intset_destroy (set);

  /* and update flags accordingly */
  tp_group_mixin_change_flags (obj, TP_CHANNEL_GROUP_FLAG_CAN_ADD,
      TP_CHANNEL_GROUP_FLAG_CAN_REMOVE | TP_CHANNEL_GROUP_FLAG_CAN_RESCIND);

  return TRUE;
}

static void
session_terminated_cb (GabbleMediaSession *session,
                       guint terminator,
                       guint reason,
                       gpointer user_data)
{
  GabbleMediaChannel *channel = (GabbleMediaChannel *) user_data;
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (channel);
  TpGroupMixin *mixin = TP_GROUP_MIXIN (channel);
  gchar *sid;
  JingleSessionState state;
  TpHandle peer;
  TpIntSet *set;

  g_object_get (session,
                "state", &state,
                "peer", &peer,
                NULL);

  set = tp_intset_new ();

  /* remove us and the peer from the member list */
  tp_intset_add (set, mixin->self_handle);
  tp_intset_add (set, peer);

  tp_group_mixin_change_members ((GObject *) channel,
      "", NULL, set, NULL, NULL, terminator, reason);

  /* update flags accordingly -- allow adding, deny removal */
  tp_group_mixin_change_flags ((GObject *) channel,
      TP_CHANNEL_GROUP_FLAG_CAN_ADD,
      TP_CHANNEL_GROUP_FLAG_CAN_REMOVE);

  /* free the session ID */
  g_object_get (priv->session, "session-id", &sid, NULL);
  _gabble_media_factory_free_sid (priv->factory, sid);
  g_free (sid);

  /* unref streams */
  if (priv->streams != NULL)
    {
      GPtrArray *tmp = priv->streams;

      /* move priv->streams aside so that the stream_close_cb
       * doesn't double unref */
      priv->streams = NULL;
      g_ptr_array_foreach (tmp, (GFunc) g_object_unref, NULL);
      g_ptr_array_free (tmp, TRUE);
    }

  /* remove the session */
  g_object_unref (priv->session);
  priv->session = NULL;

  /* close the channel */
  gabble_media_channel_close (channel);
}


static void
session_state_changed_cb (GabbleMediaSession *session,
                          GParamSpec *arg1,
                          GabbleMediaChannel *channel)
{
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (channel);
  TpGroupMixin *mixin = TP_GROUP_MIXIN (channel);
  JingleSessionState state;
  TpHandle peer;
  TpIntSet *set;

  g_object_get (session,
                "state", &state,
                "peer", &peer,
                NULL);

  set = tp_intset_new ();

  tp_intset_add (set, peer);

  if (state >= JS_STATE_PENDING_INITIATE_SENT &&
      state < JS_STATE_ACTIVE &&
      !tp_handle_set_is_member (mixin->members, peer))
    {
      /* The first time we send anything to the other user, they materialise
       * in remote-pending if necessary */

      tp_group_mixin_change_members ((GObject *) channel,
          "", NULL, NULL, NULL, set, 0, 0);

      tp_group_mixin_change_flags ((GObject *) channel,
          TP_CHANNEL_GROUP_FLAG_CAN_REMOVE | TP_CHANNEL_GROUP_FLAG_CAN_RESCIND,
          TP_CHANNEL_GROUP_FLAG_CAN_ADD);
    }

  if (state == JS_STATE_ACTIVE &&
      priv->creator == mixin->self_handle)
    {
      /* add the peer to the member list */
      tp_group_mixin_change_members ((GObject *) channel,
          "", set, NULL, NULL, NULL, 0, 0);

      /* update flags accordingly -- allow removal, deny adding and
       * rescinding */
      tp_group_mixin_change_flags ((GObject *) channel,
          TP_CHANNEL_GROUP_FLAG_CAN_REMOVE,
          TP_CHANNEL_GROUP_FLAG_CAN_ADD | TP_CHANNEL_GROUP_FLAG_CAN_RESCIND);
    }

  tp_intset_destroy (set);
}


static void
inform_peer_of_unhold (GabbleMediaChannel *self)
{
  /* FIXME: when we upgrade to current Jingle, signal to the peer that
   * we've taken them off hold, via a session-info message;
   * ignore success or failure, since there's nothing we could really
   * do differently, and the message is only advisory.
   *
   * For now, we don't signal the unhold in the XMPP stream */
  DEBUG ("TODO: tell peer we've taken them off hold");
}


static void
inform_peer_of_hold (GabbleMediaChannel *self)
{
  /* FIXME: when we upgrade to current Jingle, signal to the peer that
   * we're putting them on hold, via a session-info message;
   * ignore success or failure, since there's nothing we could really
   * do differently, and the message is only advisory.
   *
   * For now, we don't signal the hold in the XMPP stream */
  DEBUG ("TODO: tell peer we're putting them on hold");
}


static void
stream_hold_state_changed (GabbleMediaStream *stream G_GNUC_UNUSED,
                           GParamSpec *unused G_GNUC_UNUSED,
                           gpointer data)
{
  GabbleMediaChannel *self = data;
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);
  gboolean all_held = TRUE, any_held = FALSE;
  guint i;

  for (i = 0; i < priv->streams->len; i++)
    {
      gboolean its_hold;

      g_object_get (g_ptr_array_index (priv->streams, i),
          "local-hold", &its_hold,
          NULL);

      DEBUG ("Stream at index %u has local-hold=%u", i, (guint) its_hold);

      all_held = all_held && its_hold;
      any_held = any_held || its_hold;
    }

  DEBUG ("all_held=%u, any_held=%u", (guint) all_held, (guint) any_held);

  if (all_held)
    {
      /* Move to state HELD */

      if (priv->hold_state == TP_LOCAL_HOLD_STATE_HELD)
        {
          /* nothing changed */
          return;
        }
      else if (priv->hold_state == TP_LOCAL_HOLD_STATE_PENDING_UNHOLD)
        {
          /* This can happen if the user asks us to hold, then changes their
           * mind. We make no particular guarantees about stream states when
           * in PENDING_UNHOLD state, so keep claiming to be in that state */
          return;
        }
      else if (priv->hold_state == TP_LOCAL_HOLD_STATE_PENDING_HOLD)
        {
          /* We wanted to hold, and indeed we have. Yay! Keep whatever
           * reason code we used for going to PENDING_HOLD */
          priv->hold_state = TP_LOCAL_HOLD_STATE_HELD;
        }
      else
        {
          /* We were previously UNHELD. So why have we gone on hold now? */
          DEBUG ("Unexpectedly entered HELD state!");
          priv->hold_state = TP_LOCAL_HOLD_STATE_HELD;
          priv->hold_state_reason = TP_LOCAL_HOLD_STATE_REASON_NONE;
        }
    }
  else if (any_held)
    {
      if (priv->hold_state == TP_LOCAL_HOLD_STATE_UNHELD)
        {
          /* The streaming client has spontaneously changed its stream
           * state. Why? We just don't know */
          DEBUG ("Unexpectedly entered PENDING_UNHOLD state!");
          priv->hold_state = TP_LOCAL_HOLD_STATE_PENDING_UNHOLD;
          priv->hold_state_reason = TP_LOCAL_HOLD_STATE_REASON_NONE;
        }
      else if (priv->hold_state == TP_LOCAL_HOLD_STATE_HELD)
        {
          /* Likewise */
          DEBUG ("Unexpectedly entered PENDING_HOLD state!");
          priv->hold_state = TP_LOCAL_HOLD_STATE_PENDING_HOLD;
          priv->hold_state_reason = TP_LOCAL_HOLD_STATE_REASON_NONE;
        }
      else
        {
          /* nothing particularly interesting - we're trying to change hold
           * state already, so nothing to signal */
          return;
        }

      /* Tell the peer what's happened */
      inform_peer_of_unhold (self);
    }
  else
    {
      /* Move to state UNHELD */

      if (priv->hold_state == TP_LOCAL_HOLD_STATE_UNHELD)
        {
          /* nothing changed */
          return;
        }
      else if (priv->hold_state == TP_LOCAL_HOLD_STATE_PENDING_HOLD)
        {
          /* This can happen if the user asks us to unhold, then changes their
           * mind. We make no particular guarantees about stream states when
           * in PENDING_HOLD state, so keep claiming to be in that state */
          return;
        }
      else if (priv->hold_state == TP_LOCAL_HOLD_STATE_PENDING_UNHOLD)
        {
          /* We wanted to hold, and indeed we have. Yay! Keep whatever
           * reason code we used for going to PENDING_UNHOLD */
          priv->hold_state = TP_LOCAL_HOLD_STATE_UNHELD;
        }
      else
        {
          /* We were previously HELD. So why have we gone off hold now? */
          DEBUG ("Unexpectedly entered UNHELD state!");
          priv->hold_state = TP_LOCAL_HOLD_STATE_UNHELD;
          priv->hold_state_reason = TP_LOCAL_HOLD_STATE_REASON_NONE;
        }
    }

  tp_svc_channel_interface_hold_emit_hold_state_changed (self,
      priv->hold_state, priv->hold_state_reason);
}


static void
stream_unhold_failed (GabbleMediaStream *stream,
                      gpointer data)
{
  GabbleMediaChannel *self = data;
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);
  guint i;

  DEBUG ("%p: %p", self, stream);

  /* Unholding failed - let's roll back to Hold state */
  priv->hold_state = TP_LOCAL_HOLD_STATE_PENDING_HOLD;
  priv->hold_state_reason = TP_LOCAL_HOLD_STATE_REASON_RESOURCE_NOT_AVAILABLE;
  tp_svc_channel_interface_hold_emit_hold_state_changed (self,
      priv->hold_state, priv->hold_state_reason);

  /* The stream's state may have changed from unheld to held, so re-poll.
   * It's possible that all streams are now held, in which case we can stop. */
  stream_hold_state_changed (stream, NULL, self);

  if (priv->hold_state == TP_LOCAL_HOLD_STATE_HELD)
    return;

  /* There should be no need to notify the peer, who already thinks they're
   * on hold, so just tell the streaming client what to do. */

  for (i = 0; i < priv->streams->len; i++)
    {
      gabble_media_stream_hold (g_ptr_array_index (priv->streams, i),
          TRUE);
    }
}


static void
stream_close_cb (GabbleMediaStream *stream,
                 GabbleMediaChannel *chan)
{
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);
  guint id;

  g_object_get (stream,
      "id", &id,
      NULL);

  tp_svc_channel_type_streamed_media_emit_stream_removed (chan, id);

  if (priv->streams != NULL)
    {
      g_ptr_array_remove (priv->streams, stream);

      /* A stream closing might cause the "total" hold state to change:
       * if there's one held and one unheld, and the unheld one closes,
       * then our state changes from indeterminate to held. */
      stream_hold_state_changed (stream, NULL, chan);

      g_object_unref (stream);
    }
}

static void
stream_error_cb (GabbleMediaStream *stream,
                 TpMediaStreamError errno,
                 const gchar *message,
                 GabbleMediaChannel *chan)
{
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);
  guint id;

  /* emit signal */
  g_object_get (stream, "id", &id, NULL);
  tp_svc_channel_type_streamed_media_emit_stream_error (chan, id, errno,
      message);

  /* remove stream from session */
  _gabble_media_session_remove_streams (priv->session, &stream, 1);
}

static void
stream_state_changed_cb (GabbleMediaStream *stream,
                         GParamSpec *pspec,
                         GabbleMediaChannel *chan)
{
  guint id;
  TpMediaStreamState connection_state;

  g_object_get (stream,
      "id", &id,
      "connection-state", &connection_state,
      NULL);

  tp_svc_channel_type_streamed_media_emit_stream_state_changed (chan,
      id, connection_state);
}

static void
stream_direction_changed_cb (GabbleMediaStream *stream,
                             GParamSpec *pspec,
                             GabbleMediaChannel *chan)
{
  guint id;
  CombinedStreamDirection combined;
  TpMediaStreamDirection direction;
  TpMediaStreamPendingSend pending_send;

  g_object_get (stream,
      "id", &id,
      "combined-direction", &combined,
      NULL);

  direction = COMBINED_DIRECTION_GET_DIRECTION (combined);
  pending_send = COMBINED_DIRECTION_GET_PENDING_SEND (combined);

  tp_svc_channel_type_streamed_media_emit_stream_direction_changed (
      chan, id, direction, pending_send);
}

static void
session_stream_added_cb (GabbleMediaSession *session,
                         GabbleMediaStream  *stream,
                         GabbleMediaChannel *chan)
{
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);

  guint id, handle, type;

  /* keep track of the stream */
  g_object_ref (stream);
  g_ptr_array_add (priv->streams, stream);

  g_signal_connect (stream, "close",
                    (GCallback) stream_close_cb, chan);
  g_signal_connect (stream, "error",
                    (GCallback) stream_error_cb, chan);
  g_signal_connect (stream, "unhold-failed",
                    (GCallback) stream_unhold_failed, chan);
  g_signal_connect (stream, "notify::connection-state",
                    (GCallback) stream_state_changed_cb, chan);
  g_signal_connect (stream, "notify::combined-direction",
                    (GCallback) stream_direction_changed_cb, chan);
  g_signal_connect (stream, "notify::local-hold",
                    (GCallback) stream_hold_state_changed, chan);

  /* emit StreamAdded */
  g_object_get (session, "peer", &handle, NULL);
  g_object_get (stream, "id", &id, "media-type", &type, NULL);

  tp_svc_channel_type_streamed_media_emit_stream_added (
      chan, id, handle, type);

  /* A stream being added might cause the "total" hold state to change */
  stream_hold_state_changed (stream, NULL, chan);
}

guint
_gabble_media_channel_get_stream_id (GabbleMediaChannel *chan)
{
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (chan);

  return priv->next_stream_id++;
}

#define GTALK_CAPS \
  ( PRESENCE_CAP_GOOGLE_VOICE )

#define JINGLE_CAPS \
  ( PRESENCE_CAP_JINGLE \
  | PRESENCE_CAP_GOOGLE_TRANSPORT_P2P )

#define JINGLE_AUDIO_CAPS \
  ( PRESENCE_CAP_JINGLE_DESCRIPTION_AUDIO )

#define JINGLE_VIDEO_CAPS \
  ( PRESENCE_CAP_JINGLE_DESCRIPTION_VIDEO )

GabblePresenceCapabilities
_gabble_media_channel_typeflags_to_caps (TpChannelMediaCapabilities flags)
{
  GabblePresenceCapabilities caps = 0;

  /* currently we can only signal any (GTalk or Jingle calls) using
   * the GTalk-P2P transport */
  if (flags & TP_CHANNEL_MEDIA_CAPABILITY_NAT_TRAVERSAL_GTALK_P2P)
    {
      caps |= JINGLE_CAPS;

      if (flags & TP_CHANNEL_MEDIA_CAPABILITY_AUDIO)
        caps |= GTALK_CAPS | JINGLE_AUDIO_CAPS;

      if (flags & TP_CHANNEL_MEDIA_CAPABILITY_VIDEO)
        caps |= JINGLE_VIDEO_CAPS;
    }

  return caps;
}

TpChannelMediaCapabilities
_gabble_media_channel_caps_to_typeflags (GabblePresenceCapabilities caps)
{
  TpChannelMediaCapabilities typeflags = 0;

  /* this is intentionally asymmetric to the previous function - we don't
   * require the other end to advertise the GTalk-P2P transport capability
   * separately because old GTalk clients didn't do that - having Google voice
   * implied Google session and GTalk-P2P */
  if ((caps & GTALK_CAPS) == GTALK_CAPS)
    typeflags |= TP_CHANNEL_MEDIA_CAPABILITY_AUDIO;

  if ((caps & JINGLE_CAPS) == JINGLE_CAPS)
    {
      if ((caps & JINGLE_AUDIO_CAPS) == JINGLE_AUDIO_CAPS)
        typeflags |= TP_CHANNEL_MEDIA_CAPABILITY_AUDIO;

      if ((caps & JINGLE_VIDEO_CAPS) == JINGLE_VIDEO_CAPS)
        typeflags |= TP_CHANNEL_MEDIA_CAPABILITY_VIDEO;
    }

  return typeflags;
}

static void
gabble_media_channel_get_call_states (TpSvcChannelInterfaceCallState *iface,
                                      DBusGMethodInvocation *context)
{
  GHashTable *states;

  /* stub implementation: nobody has any call-state flags */
  states = g_hash_table_new (g_direct_hash, g_direct_equal);
  tp_svc_channel_interface_call_state_return_from_get_call_states (context,
      states);
  g_hash_table_destroy (states);
}

static void
gabble_media_channel_get_hold_state (TpSvcChannelInterfaceHold *iface,
                                     DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = (GabbleMediaChannel *) iface;
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);

  tp_svc_channel_interface_hold_return_from_get_hold_state (context,
      priv->hold_state, priv->hold_state_reason);
}


static void
gabble_media_channel_request_hold (TpSvcChannelInterfaceHold *iface,
                                   gboolean hold,
                                   DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv = GABBLE_MEDIA_CHANNEL_GET_PRIVATE (self);
  guint i;
  TpLocalHoldState old_state = priv->hold_state;

  DEBUG ("%p: RequestHold(%u)", self, !!hold);

  if (hold)
    {
      if (priv->hold_state == TP_LOCAL_HOLD_STATE_HELD)
        {
          DEBUG ("No-op");
          tp_svc_channel_interface_hold_return_from_request_hold (context);
          return;
        }

      inform_peer_of_hold (self);

      priv->hold_state = TP_LOCAL_HOLD_STATE_PENDING_HOLD;
    }
  else
    {
      if (priv->hold_state == TP_LOCAL_HOLD_STATE_UNHELD)
        {
          DEBUG ("No-op");
          tp_svc_channel_interface_hold_return_from_request_hold (context);
          return;
        }

      priv->hold_state = TP_LOCAL_HOLD_STATE_PENDING_UNHOLD;
    }

  if (old_state != priv->hold_state ||
      priv->hold_state_reason != TP_LOCAL_HOLD_STATE_REASON_REQUESTED)
    {
      tp_svc_channel_interface_hold_emit_hold_state_changed (self,
          priv->hold_state, TP_LOCAL_HOLD_STATE_REASON_REQUESTED);
      priv->hold_state_reason = TP_LOCAL_HOLD_STATE_REASON_REQUESTED;
    }

  /* Tell streaming client to release or reacquire resources */

  for (i = 0; i < priv->streams->len; i++)
    {
      gabble_media_stream_hold (g_ptr_array_index (priv->streams, i), hold);
    }

  tp_svc_channel_interface_hold_return_from_request_hold (context);
}


static void
channel_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelClass *klass = (TpSvcChannelClass *) g_iface;

#define IMPLEMENT(x, suffix) tp_svc_channel_implement_##x (\
    klass, gabble_media_channel_##x##suffix)
  IMPLEMENT(close,_async);
  IMPLEMENT(get_channel_type,);
  IMPLEMENT(get_handle,);
  IMPLEMENT(get_interfaces,);
#undef IMPLEMENT
}

static void
streamed_media_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelTypeStreamedMediaClass *klass =
    (TpSvcChannelTypeStreamedMediaClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_type_streamed_media_implement_##x (\
    klass, gabble_media_channel_##x)
  IMPLEMENT(list_streams);
  IMPLEMENT(remove_streams);
  IMPLEMENT(request_stream_direction);
  IMPLEMENT(request_streams);
#undef IMPLEMENT
}

static void
media_signalling_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelInterfaceMediaSignallingClass *klass =
    (TpSvcChannelInterfaceMediaSignallingClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_media_signalling_implement_##x (\
    klass, gabble_media_channel_##x)
  IMPLEMENT(get_session_handlers);
#undef IMPLEMENT
}

static void
call_state_iface_init (gpointer g_iface,
                       gpointer iface_data G_GNUC_UNUSED)
{
  TpSvcChannelInterfaceCallStateClass *klass = g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_call_state_implement_##x (\
    klass, gabble_media_channel_##x)
  IMPLEMENT(get_call_states);
#undef IMPLEMENT
}

static void
hold_iface_init (gpointer g_iface,
                 gpointer iface_data G_GNUC_UNUSED)
{
  TpSvcChannelInterfaceHoldClass *klass = g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_hold_implement_##x (\
    klass, gabble_media_channel_##x)
  IMPLEMENT(get_hold_state);
  IMPLEMENT(request_hold);
#undef IMPLEMENT
}