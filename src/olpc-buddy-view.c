/*
 * olpc-buddy-view.c - Source for GabbleOlpcView
 * Copyright (C) 2008 Collabora Ltd.
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

#include "olpc-buddy-view.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include <loudmouth/loudmouth.h>
#include <telepathy-glib/channel-iface.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/svc-channel.h>
#include <telepathy-glib/svc-generic.h>

#define DEBUG_FLAG GABBLE_DEBUG_OLPC

#include "conn-olpc.h"
#include "debug.h"
#include "extensions/extensions.h"
#include "gabble-signals-marshal.h"
#include "olpc-activity.h"
#include "namespaces.h"
#include "util.h"

#define GABBLE_ARRAY_TYPE_HANDLE (dbus_g_type_get_collection ("GArray", \
    G_TYPE_UINT))

/* signals */
enum
{
  BUDDY_ACTIVITIES_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_OBJECT_PATH = 1,
  /* org.freedesktop.Telepathy.Channel D-Bus properties */
  PROP_CHANNEL_TYPE,
  PROP_INTERFACES,
  PROP_HANDLE,
  PROP_TARGET_ID,
  PROP_HANDLE_TYPE,

  /* org.freedesktop.Telepathy.Channel.FUTURE D-Bus properties */
  PROP_REQUESTED,
  PROP_INITIATOR_HANDLE,
  PROP_INITIATOR_ID,

  PROP_CHANNEL_DESTROYED,
  PROP_CHANNEL_PROPERTIES,

  /* org.laptop.Telepathy.Channel.Type.View D-Bus properties */
  PROP_MAX_SIZE,
  PROP_BUDDIES,
  PROP_ACTIVITIES,

  /* org.laptop.Telepathy.Channel.Type.BuddyView D-Bus properties */
  PROP_VIEW_PROPERTIES,
  PROP_ALIAS,

  PROP_CONNECTION,
  PROP_ID,
  LAST_PROPERTY
};

typedef struct _GabbleOlpcViewPrivate GabbleOlpcViewPrivate;
struct _GabbleOlpcViewPrivate
{
  GabbleConnection *conn;
  char *object_path;
  guint id;
  gboolean closed;
  guint max_size;

  GHashTable *properties;
  gchar *alias;

  TpHandleSet *buddies;
  /* TpHandle => GabbleOlpcActivity * */
  GHashTable *activities;

  /* TpHandle (owned in priv->buddies) => GHashTable * */
  GHashTable *buddy_properties;
  /* TpHandle (owned in priv->buddies) => TpHandleSet of activity rooms */
  GHashTable *buddy_rooms;

  gboolean dispose_has_run;
};

static void channel_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE (
    GabbleOlpcView, gabble_olpc_view, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_SVC_OLPC_CHANNEL_TYPE_BUDDYVIEW,
      NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL, channel_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_EXPORTABLE_CHANNEL, NULL);
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_SVC_CHANNEL_FUTURE, NULL);
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_SVC_OLPC_CHANNEL_INTERFACE_VIEW, NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_CHANNEL_IFACE, NULL);
    );

static const gchar *gabble_olpc_buddy_view_interfaces[] = {
    GABBLE_IFACE_CHANNEL_FUTURE,
    GABBLE_IFACE_OLPC_CHANNEL_INTERFACE_VIEW,
    NULL
};

#define GABBLE_OLPC_VIEW_GET_PRIVATE(obj) \
    ((GabbleOlpcViewPrivate *) obj->priv)


static void
gabble_olpc_view_init (GabbleOlpcView *self)
{
  GabbleOlpcViewPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_OLPC_VIEW, GabbleOlpcViewPrivate);

  self->priv = priv;

  priv->closed = FALSE;

  priv->dispose_has_run = FALSE;
}

static void
gabble_olpc_view_dispose (GObject *object)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (object);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  if (priv->buddies != NULL)
    {
      tp_handle_set_destroy (priv->buddies);
      priv->buddies = NULL;
    }

  if (priv->activities != NULL)
    {
      g_hash_table_destroy (priv->activities);
      priv->activities = NULL;
    }

  if (priv->buddy_properties != NULL)
    {
      g_hash_table_destroy (priv->buddy_properties);
      priv->buddy_properties = NULL;
    }

  if (priv->buddy_rooms != NULL)
    {
      g_hash_table_destroy (priv->buddy_rooms);
      priv->buddy_rooms = NULL;
    }

  priv->dispose_has_run = TRUE;

  if (G_OBJECT_CLASS (gabble_olpc_view_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_olpc_view_parent_class)->dispose (object);
}

static void
gabble_olpc_view_finalize (GObject *object)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (object);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  g_free (priv->object_path);

  G_OBJECT_CLASS (gabble_olpc_view_parent_class)->finalize (object);
}

static void
add_activity_to_array (TpHandle handle,
                       GabbleOlpcActivity *activity,
                       GPtrArray *array)
{
  GValue gvalue = { 0 };

  g_value_init (&gvalue, GABBLE_STRUCT_TYPE_ACTIVITY);
  g_value_take_boxed (&gvalue, dbus_g_type_specialized_construct
      (GABBLE_STRUCT_TYPE_ACTIVITY));
  dbus_g_type_struct_set (&gvalue,
      0, activity->id,
      1, activity->room,
      G_MAXUINT);

  g_ptr_array_add (array, g_value_get_boxed (&gvalue));
}

static GPtrArray *
create_activities_array (GabbleOlpcView *self)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GPtrArray *activities;

  activities = g_ptr_array_new ();

  g_hash_table_foreach (priv->activities, (GHFunc) add_activity_to_array,
      activities);

  return activities;
}

static void
gabble_olpc_view_get_property (GObject *object,
                               guint property_id,
                               GValue *value,
                               GParamSpec *pspec)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (object);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  TpBaseConnection *base_conn = (TpBaseConnection *) priv->conn;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_value_set_string (value, priv->object_path);
        break;
      case PROP_CHANNEL_TYPE:
        g_value_set_static_string (value,
            GABBLE_IFACE_OLPC_CHANNEL_TYPE_BUDDYVIEW);
        break;
      case PROP_HANDLE_TYPE:
        g_value_set_uint (value, TP_HANDLE_TYPE_NONE);
        break;
      case PROP_HANDLE:
        g_value_set_uint (value, 0);
        break;
      case PROP_INITIATOR_HANDLE:
        g_value_set_uint (value, base_conn->self_handle);
        break;
      case PROP_INITIATOR_ID:
          {
            TpHandleRepoIface *repo = tp_base_connection_get_handles (
                base_conn, TP_HANDLE_TYPE_CONTACT);

            g_value_set_string (value,
                tp_handle_inspect (repo, base_conn->self_handle));
          }
        break;
      case PROP_TARGET_ID:
          {
            g_value_set_string (value, NULL);
        }
        break;
      case PROP_REQUESTED:
        g_value_set_boolean (value, TRUE);
        break;
     case PROP_INTERFACES:
        g_value_set_boxed (value, gabble_olpc_buddy_view_interfaces);
        break;
      case PROP_CHANNEL_DESTROYED:
        g_value_set_boolean (value, priv->closed);
        break;
      case PROP_CHANNEL_PROPERTIES:
        g_value_take_boxed (value,
            tp_dbus_properties_mixin_make_properties_hash (object,
                TP_IFACE_CHANNEL, "TargetHandle",
                TP_IFACE_CHANNEL, "TargetHandleType",
                TP_IFACE_CHANNEL, "ChannelType",
                TP_IFACE_CHANNEL, "TargetID",
                GABBLE_IFACE_CHANNEL_FUTURE, "InitiatorHandle",
                GABBLE_IFACE_CHANNEL_FUTURE, "InitiatorID",
                GABBLE_IFACE_CHANNEL_FUTURE, "Requested",
                GABBLE_IFACE_OLPC_CHANNEL_INTERFACE_VIEW, "MaxSize",
                GABBLE_IFACE_OLPC_CHANNEL_INTERFACE_VIEW, "Buddies",
                GABBLE_IFACE_OLPC_CHANNEL_INTERFACE_VIEW, "Activities",
                GABBLE_IFACE_OLPC_CHANNEL_TYPE_BUDDYVIEW, "Properties",
                GABBLE_IFACE_OLPC_CHANNEL_TYPE_BUDDYVIEW, "Alias",
                NULL));
        break;
      case PROP_CONNECTION:
        g_value_set_object (value, priv->conn);
        break;
      case PROP_ID:
        g_value_set_uint (value, priv->id);
        break;
      case PROP_MAX_SIZE:
        g_value_set_uint (value, priv->max_size);
        break;
      case PROP_BUDDIES:
        g_value_take_boxed (value, tp_handle_set_to_array (priv->buddies));
        break;
      case PROP_ACTIVITIES:
        /* TODO: is that leak free ? */
        g_value_take_boxed (value, create_activities_array (self));
        break;
      case PROP_VIEW_PROPERTIES:
        g_value_set_boxed (value, priv->properties);
        break;
      case PROP_ALIAS:
        g_value_set_string (value, priv->alias);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gabble_olpc_view_set_property (GObject *object,
                               guint property_id,
                               const GValue *value,
                               GParamSpec *pspec)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (object);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_free (priv->object_path);
        priv->object_path = g_value_dup_string (value);
        break;
      case PROP_HANDLE:
      case PROP_INITIATOR_HANDLE:
      case PROP_HANDLE_TYPE:
      case PROP_CHANNEL_TYPE:
        /* these properties are writable in the interface, but not actually
         * meaningfully changeable on this channel, so we do nothing */
        break;
      case PROP_CONNECTION:
        priv->conn = g_value_get_object (value);
        break;
      case PROP_ID:
        priv->id = g_value_get_uint (value);
        break;
      case PROP_MAX_SIZE:
        priv->max_size = g_value_get_uint (value);
        break;
      case PROP_VIEW_PROPERTIES:
        priv->properties = g_value_dup_boxed (value);
        break;
      case PROP_ALIAS:
        priv->alias = g_value_dup_string (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static GObject *
gabble_olpc_view_constructor (GType type,
                              guint n_props,
                              GObjectConstructParam *props)
{
  GObject *obj;
  GabbleOlpcViewPrivate *priv;
  DBusGConnection *bus;
  TpBaseConnection *conn;
  TpHandleRepoIface *contact_handles;

  obj = G_OBJECT_CLASS (gabble_olpc_view_parent_class)->
           constructor (type, n_props, props);

  priv = GABBLE_OLPC_VIEW_GET_PRIVATE (GABBLE_OLPC_VIEW (obj));
  conn = (TpBaseConnection *) priv->conn;

  priv->object_path = g_strdup_printf ("%s/OlpcView%u",
      conn->object_path, priv->id);
  bus = tp_get_bus ();
  dbus_g_connection_register_g_object (bus, priv->object_path, obj);

  contact_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  priv->buddies = tp_handle_set_new (contact_handles);

  priv->activities = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, g_object_unref);
  priv->buddy_properties = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, (GDestroyNotify) g_hash_table_unref);
  priv->buddy_rooms = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) tp_handle_set_destroy);

  if (priv->properties == NULL)
    {
      priv->properties = g_hash_table_new (g_direct_hash, g_direct_equal);
    }

  return obj;
}

static void
gabble_olpc_view_class_init (GabbleOlpcViewClass *gabble_olpc_view_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_olpc_view_class);
  GParamSpec *param_spec;
   static TpDBusPropertiesMixinPropImpl channel_props[] = {
      { "TargetHandleType", "handle-type", NULL },
      { "TargetHandle", "handle", NULL },
      { "TargetID", "target-id", NULL },
      { "ChannelType", "channel-type", NULL },
      { "Interfaces", "interfaces", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinPropImpl future_props[] = {
      { "Requested", "requested", NULL },
      { "InitiatorHandle", "initiator-handle", NULL },
      { "InitiatorID", "initiator-id", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinPropImpl view_props[] = {
      { "MaxSize", "max-size", NULL },
      { "Buddies", "buddies", NULL },
      { "Activities", "activities", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinPropImpl buddy_view_props[] = {
      { "Properties", "view-properties", NULL },
      { "Alias", "alias", NULL },
      { NULL }
  };
  /* Add view properties */
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TP_IFACE_CHANNEL,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        channel_props,
      },
      { GABBLE_IFACE_CHANNEL_FUTURE,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        future_props,
      },
      { GABBLE_IFACE_OLPC_CHANNEL_INTERFACE_VIEW,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        view_props,
      },
      { GABBLE_IFACE_OLPC_CHANNEL_TYPE_BUDDYVIEW,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        buddy_view_props,
      },
      { NULL }
  };

  object_class->get_property = gabble_olpc_view_get_property;
  object_class->set_property = gabble_olpc_view_set_property;
  object_class->constructor = gabble_olpc_view_constructor;

  g_type_class_add_private (gabble_olpc_view_class,
      sizeof (GabbleOlpcViewPrivate));

  object_class->dispose = gabble_olpc_view_dispose;
  object_class->finalize = gabble_olpc_view_finalize;

  g_object_class_override_property (object_class, PROP_OBJECT_PATH,
      "object-path");
  g_object_class_override_property (object_class, PROP_CHANNEL_TYPE,
      "channel-type");
  g_object_class_override_property (object_class, PROP_HANDLE_TYPE,
      "handle-type");
  g_object_class_override_property (object_class, PROP_HANDLE, "handle");
  g_object_class_override_property (object_class, PROP_CHANNEL_DESTROYED,
      "channel-destroyed");
  g_object_class_override_property (object_class, PROP_CHANNEL_PROPERTIES,
      "channel-properties");

  param_spec = g_param_spec_boxed ("interfaces", "Extra D-Bus interfaces",
      "Additional Channel.Interface.* interfaces",
      G_TYPE_STRV,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INTERFACES, param_spec);

  param_spec = g_param_spec_string ("target-id", "Peer's bare JID",
      "The string obtained by inspecting the peer handle (never the full JID)",
      NULL,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_TARGET_ID, param_spec);

  param_spec = g_param_spec_boolean ("requested", "Requested?",
      "True if this channel was requested by the local user",
      FALSE,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_REQUESTED, param_spec);

  param_spec = g_param_spec_uint ("initiator-handle", "Initiator's handle",
      "The contact who initiated the channel",
      0, G_MAXUINT32, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INITIATOR_HANDLE,
      param_spec);

  param_spec = g_param_spec_string ("initiator-id", "Initiator's bare JID",
      "The string obtained by inspecting the initiator-handle",
      NULL,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INITIATOR_ID,
      param_spec);

  param_spec = g_param_spec_uint ("max-size", "View's max size",
      "The maximum number of elements that Gadget has to return to the "
      "search request.",
      0, G_MAXUINT32, 0,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_MAX_SIZE,
      param_spec);

  param_spec = g_param_spec_boxed ("buddies", "View's buddies",
      "The contact handles of the buddies who are in this view channel",
      GABBLE_ARRAY_TYPE_HANDLE,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_BUDDIES,
      param_spec);

  param_spec = g_param_spec_boxed ("activities", "View's activities",
      "The activities which are in this view channel",
      GABBLE_ARRAY_TYPE_ACTIVITY_LIST,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_ACTIVITIES,
      param_spec);

  param_spec = g_param_spec_boxed ("view-properties", "View's search properties",
      "The buddy properties Gadget should look for",
      TP_HASH_TYPE_STRING_VARIANT_MAP,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_VIEW_PROPERTIES,
      param_spec);

  param_spec = g_param_spec_string ("alias", "View's search alias",
      "The buddy alias Gadget should look for",
      NULL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_ALIAS,
      param_spec);

   param_spec = g_param_spec_object (
      "connection",
      "GabbleConnection object",
      "Gabble connection object that owns this view object.",
      GABBLE_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_uint (
      "id",
      "query ID",
      "The ID of the query associated with this view",
      0, G_MAXUINT, 0,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_ID, param_spec);

  signals[BUDDY_ACTIVITIES_CHANGED] =
    g_signal_new ("buddy-activities-changed",
        G_OBJECT_CLASS_TYPE (gabble_olpc_view_class),
        G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
        0,
        NULL, NULL,
        gabble_marshal_VOID__UINT,
        G_TYPE_NONE, 1, G_TYPE_UINT);

  gabble_olpc_view_class->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleOlpcViewClass, dbus_props_class));
}

GabbleOlpcView *
gabble_olpc_buddy_view_new (GabbleConnection *conn,
                            const gchar *object_path,
                            guint id,
                            guint max_size,
                            GHashTable *properties,
                            const gchar *alias)

{
  return g_object_new (GABBLE_TYPE_OLPC_VIEW,
      "object-path", object_path,
      "connection", conn,
      "id", id,
      "max-size", max_size,
      "view-properties", properties,
      "alias", alias,
      NULL);
}

static void
free_activities_array (GPtrArray *activities)
{
  guint i;

  for (i = 0; i < activities->len; i++)
    g_boxed_free (GABBLE_STRUCT_TYPE_ACTIVITY, activities->pdata[i]);

  g_ptr_array_free (activities, TRUE);
}

static void
buddy_left_activities_foreach (TpHandleSet *set,
                               TpHandle buddy,
                               GabbleOlpcView *self)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  /* Remove all the activity of this buddy */
  if (!g_hash_table_remove (priv->buddy_rooms, GUINT_TO_POINTER (buddy)))
    return;

  g_signal_emit (G_OBJECT (self), signals[BUDDY_ACTIVITIES_CHANGED],
      0, buddy);
}

static gboolean
do_close (GabbleOlpcView *self,
          GError **error)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  LmMessage *msg;
  gchar *id_str;

  id_str = g_strdup_printf ("%u", priv->id);

  msg = lm_message_build (priv->conn->olpc_gadget_buddy,
      LM_MESSAGE_TYPE_MESSAGE,
      '(', "close", "",
        '@', "xmlns", NS_OLPC_BUDDY,
        '@', "id", id_str,
      ')', NULL);

#if 0
  else if (priv->type == GABBLE_OLPC_VIEW_TYPE_ACTIVITY)
    {
      msg = lm_message_build (priv->conn->olpc_gadget_activity,
          LM_MESSAGE_TYPE_MESSAGE,
          '(', "close", "",
            '@', "xmlns", NS_OLPC_ACTIVITY,
            '@', "id", id_str,
          ')', NULL);
#endif

  g_free (id_str);

  if (!_gabble_connection_send (priv->conn, msg, error))
    {
      lm_message_unref (msg);
      return FALSE;
    }

  lm_message_unref (msg);

  /* Claim that all the buddies left their activities */
  tp_handle_set_foreach (priv->buddies,
      (TpHandleSetMemberFunc) buddy_left_activities_foreach, self);

  priv->closed = TRUE;

  tp_svc_channel_emit_closed (self);

  return TRUE;
}

/**
 * gabble_olpc_buddy_view_close
 *
 * Implements D-Bus method Close
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_olpc_buddy_view_close (TpSvcChannel *iface,
                              DBusGMethodInvocation *context)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (iface);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GError *error = NULL;

  if (priv->closed)
    {
      DEBUG ("Already closed. Doing nothing");
    }
  else
    {
      /* FIXME: set closed */
      if (!do_close (self, &error))
        {
          dbus_g_method_return_error (context, error);
          g_error_free (error);
        }
    }

  tp_svc_channel_return_from_close (context);
}

/**
 * gabble_olpc_buddy_view_get_channel_type
 *
 * Implements D-Bus method GetChannelType
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_olpc_buddy_view_get_channel_type (TpSvcChannel *iface,
                                         DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_channel_type (context,
      GABBLE_IFACE_OLPC_CHANNEL_TYPE_BUDDYVIEW);
}

/**
 * gabble_olpc_buddy_view_get_handle
 *
 * Implements D-Bus method GetHandle
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_olpc_buddy_view_get_handle (TpSvcChannel *iface,
                                   DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_handle (context, TP_HANDLE_TYPE_NONE, 0);
}

/**
 * gabble_olpc_buddy_view_get_interfaces
 *
 * Implements D-Bus method GetInterfaces
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_olpc_buddy_view_get_interfaces (TpSvcChannel *iface,
                                       DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_interfaces (context,
      gabble_olpc_buddy_view_interfaces);
}

/* If room is not zero, these buddies are associated with the activity
 * of this room. They'll leave the view if the activity is removed.
 */
void
gabble_olpc_view_add_buddies (GabbleOlpcView *self,
                              GArray *buddies,
                              GPtrArray *buddies_properties,
                              TpHandle room)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  guint i;
  GArray *empty;
  TpHandleRepoIface *room_repo;
  GArray *buddies_changed;

  g_assert (buddies->len == buddies_properties->len);
  if (buddies->len == 0)
    return;

  room_repo = tp_base_connection_get_handles ((TpBaseConnection *) priv->conn,
      TP_HANDLE_TYPE_ROOM);

  empty = g_array_new (FALSE, FALSE, sizeof (TpHandle));
  buddies_changed = g_array_new (FALSE, FALSE, sizeof (TpHandle));

  /* store properties */
  for (i = 0; i < buddies->len; i++)
    {
      TpHandle handle;
      GHashTable *properties;

      handle = g_array_index (buddies, TpHandle, i);
      properties = g_ptr_array_index (buddies_properties, i);

      tp_handle_set_add (priv->buddies, handle);
      g_hash_table_insert (priv->buddy_properties, GUINT_TO_POINTER (handle),
          properties);
      g_hash_table_ref (properties);

      if (room != 0)
        {
          /* buddies are in an activity */
          TpHandleSet *set;

          set = g_hash_table_lookup (priv->buddy_rooms, GUINT_TO_POINTER (
                handle));

          if (set == NULL)
            {
              set = tp_handle_set_new (room_repo);

              g_hash_table_insert (priv->buddy_rooms, GUINT_TO_POINTER (
                    handle), set);
            }

          if (!tp_handle_set_is_member (set, room))
            {
              tp_handle_set_add (set, room);

              /* We fire BuddyInfo.ActivitiesChanged signal after
               * View.BuddiesChanged so client knows where these buddies
               * come from */
              g_array_append_val (buddies_changed, handle);
            }
        }
    }

  gabble_svc_olpc_channel_interface_view_emit_buddies_changed (self, buddies,
      empty);

  for (i = 0; i < buddies_changed->len; i++)
    {
      TpHandle handle;

      handle = g_array_index (buddies_changed, TpHandle, i);
      g_signal_emit (G_OBJECT (self),
          signals[BUDDY_ACTIVITIES_CHANGED], 0, handle);
    }

  g_array_free (buddies_changed, TRUE);
  g_array_free (empty, TRUE);
}

static void
remove_buddy_foreach (TpHandleSet *buddies,
                      TpHandle handle,
                      GabbleOlpcView *self)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  tp_handle_set_remove (priv->buddies, handle);
  g_hash_table_remove (priv->buddy_properties, GUINT_TO_POINTER (handle));
  g_hash_table_remove (priv->buddy_rooms, GUINT_TO_POINTER (handle));
}

void
gabble_olpc_view_remove_buddies (GabbleOlpcView *self,
                                 TpHandleSet *buddies)
{
  GArray *removed, *empty;

  if (tp_handle_set_size (buddies) == 0)
    return;

  tp_handle_set_foreach (buddies,
      (TpHandleSetMemberFunc) remove_buddy_foreach, self);

  empty = g_array_new (FALSE, FALSE, sizeof (TpHandle));
  removed = tp_handle_set_to_array (buddies);

  gabble_svc_olpc_channel_interface_view_emit_buddies_changed (self, empty,
      removed);

  g_array_free (empty, TRUE);
  g_array_free (removed, TRUE);
}

gboolean
gabble_olpc_view_set_buddy_properties (GabbleOlpcView *self,
                                       TpHandle buddy,
                                       GHashTable *properties)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  if (!tp_handle_set_is_member (priv->buddies, buddy))
    {
      DEBUG ("buddy %d is not member of this view", buddy);
      return FALSE;
    }

  tp_handle_set_add (priv->buddies, buddy);
  g_hash_table_insert (priv->buddy_properties, GUINT_TO_POINTER (buddy),
      properties);
  g_hash_table_ref (properties);

  return TRUE;
}

GHashTable *
gabble_olpc_view_get_buddy_properties (GabbleOlpcView *self,
                                       TpHandle buddy)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  return g_hash_table_lookup (priv->buddy_properties,
      GUINT_TO_POINTER (buddy));
}

void
gabble_olpc_view_add_activities (GabbleOlpcView *self,
                                 GHashTable *activities)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GPtrArray *added, *empty;

  if (g_hash_table_size (activities) == 0)
    return;

  tp_g_hash_table_update (priv->activities, activities, NULL, g_object_ref);

  added = g_ptr_array_new ();
  g_hash_table_foreach (activities, (GHFunc) add_activity_to_array, added);

  empty = g_ptr_array_new ();

  gabble_svc_olpc_channel_interface_view_emit_activities_changed (self, added,
      empty);

  free_activities_array (added);
  g_ptr_array_free (empty, TRUE);
}

struct remove_activity_foreach_buddy_ctx
{
  GabbleOlpcView *view;
  TpHandle room;
  /* buddies who have to be removed */
  TpHandleSet *removed;
};

static void
remove_activity_foreach_buddy (TpHandle buddy,
                               TpHandleSet *set,
                               struct remove_activity_foreach_buddy_ctx *ctx)
{
  if (set == NULL)
    return;

  if (tp_handle_set_remove (set, ctx->room))
    {
      if (tp_handle_set_size (set) == 0)
        {
          /* No more activity for this buddy. Remove it */
          tp_handle_set_add (ctx->removed, buddy);
        }

      g_signal_emit (G_OBJECT (ctx->view), signals[BUDDY_ACTIVITIES_CHANGED],
          0, buddy);
    }
}

void
gabble_olpc_view_remove_activities (GabbleOlpcView *self,
                                    TpHandleSet *rooms)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GPtrArray *removed, *empty;
  GArray *array;
  guint i;
  TpHandleRepoIface *contact_repo;

  if (tp_handle_set_size (rooms) == 0)
    return;

  contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  /* for easier iteration */
  array = tp_handle_set_to_array (rooms);

  removed = g_ptr_array_new ();
  empty = g_ptr_array_new ();

  for (i = 0; i < array->len; i++)
    {
      TpHandle room;
      GabbleOlpcActivity *activity;
      struct remove_activity_foreach_buddy_ctx ctx;

      room = g_array_index (array, TpHandle, i);

      activity = g_hash_table_lookup (priv->activities,
          GUINT_TO_POINTER (room));
      if (activity == NULL)
        continue;

      add_activity_to_array (room, activity, removed);
      g_hash_table_remove (priv->activities, GUINT_TO_POINTER (room));

      /* remove the activity from all rooms set */
      ctx.view = self;
      ctx.room = room;
      ctx.removed = tp_handle_set_new (contact_repo);

      g_hash_table_foreach (priv->buddy_rooms,
          (GHFunc) remove_activity_foreach_buddy, &ctx);

      gabble_olpc_view_remove_buddies (self, ctx.removed);

      tp_handle_set_destroy (ctx.removed);
    }

  gabble_svc_olpc_channel_interface_view_emit_activities_changed (self, empty,
      removed);

  free_activities_array (removed);
  g_ptr_array_free (empty, TRUE);
  g_array_free (array, TRUE);
}

GPtrArray *
gabble_olpc_view_get_buddy_activities (GabbleOlpcView *self,
                                       TpHandle buddy)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GPtrArray *activities;
  TpHandleSet *rooms_set;
  GArray *rooms;
  guint i;

  activities = g_ptr_array_new ();

  rooms_set = g_hash_table_lookup (priv->buddy_rooms,
      GUINT_TO_POINTER (buddy));
  if (rooms_set == NULL || tp_handle_set_size (rooms_set) == 0)
    return activities;

  /* Convert to an array for easier iteration */
  rooms = tp_handle_set_to_array (rooms_set);
  for (i = 0; i < rooms->len; i++)
    {
      TpHandle room;
      GabbleOlpcActivity *activity;

      room = g_array_index (rooms, TpHandle, i);

      activity = g_hash_table_lookup (priv->activities,
          GUINT_TO_POINTER (room));
      if (activity == NULL)
        {
          /* This shouldn't happen as long as:
           *
           * - Gadget doesn't send us <joined> stanzas about an activity
           *   which was not previously announced as being part of the view.
           *
           * - We don't call gabble_olpc_view_add_buddies with an activity
           *   which was not previoulsy added to the view.
           */
          DEBUG ("Buddy %d is supposed to be in activity %d but view doesn't"
              " contain its info", buddy, room);
          continue;
        }

      g_ptr_array_add (activities, activity);
    }

  g_array_free (rooms, TRUE);

  return activities;
}

void
gabble_olpc_view_buddies_left_activity (GabbleOlpcView *self,
                                        GArray *buddies,
                                        TpHandle room)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  guint i;
  TpHandleRepoIface *contact_repo;
  TpHandleSet *removed;

  contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  removed = tp_handle_set_new (contact_repo);

  for (i = 0; i < buddies->len; i++)
    {
      TpHandleSet *set;
      TpHandle buddy;

      buddy = g_array_index (buddies, TpHandle, i);
      set = g_hash_table_lookup (priv->buddy_rooms, GUINT_TO_POINTER (buddy));
      if (set == NULL)
        continue;

      if (tp_handle_set_remove (set, room))
        {
          if (tp_handle_set_size (set) == 0)
            {
              /* Remove from the view */
              tp_handle_set_add (removed, buddy);
            }

          g_signal_emit (G_OBJECT (self), signals[BUDDY_ACTIVITIES_CHANGED],
              0, buddy);
        }
    }

  gabble_olpc_view_remove_buddies (self, removed);

  tp_handle_set_destroy (removed);
}

static LmHandlerResult
buddy_view_query_result_cb (GabbleConnection *conn,
                            LmMessage *sent_msg,
                            LmMessage *reply_msg,
                            GObject *_view,
                            gpointer user_data)
{
  LmMessageNode *view_node;
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (_view);

  view_node = lm_message_node_get_child_with_namespace (reply_msg->node,
      "view", NS_OLPC_BUDDY);
  if (view_node == NULL)
    return LM_HANDLER_RESULT_REMOVE_MESSAGE;

  /* FIXME: make sense to call this conn-olpc function ? */
  add_buddies_to_view_from_node (conn, self, view_node, "buddy", 0);

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

gboolean
gabble_olpc_buddy_view_send_request (GabbleOlpcView *self,
                                     GError **error)

{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  LmMessage *query;
  gchar *max_str, *id_str;

  max_str = g_strdup_printf ("%u", priv->max_size);
  id_str = g_strdup_printf ("%u", priv->id);

  /* TODO: Implement multi criterias properties */
  /* TODO: Always use the max_size argument */
  if (g_hash_table_size (priv->properties) != 0)
    {
      LmMessageNode *properties_node;

      query = lm_message_build_with_sub_type (priv->conn->olpc_gadget_buddy,
        LM_MESSAGE_TYPE_IQ, LM_MESSAGE_SUB_TYPE_GET,
        '(', "view", "",
            '@', "xmlns", NS_OLPC_BUDDY,
            '@', "id", id_str,
            '(', "buddy", "",
              '(', "properties", "",
                '*', &properties_node,
                '@', "xmlns", NS_OLPC_BUDDY_PROPS,
              ')',
            ')',
        ')',
        NULL);

      lm_message_node_add_children_from_properties (properties_node,
          priv->properties, "property");
    }
  else if (priv->alias != NULL)
    {
      query = lm_message_build_with_sub_type (priv->conn->olpc_gadget_buddy,
        LM_MESSAGE_TYPE_IQ, LM_MESSAGE_SUB_TYPE_GET,
        '(', "view", "",
            '@', "xmlns", NS_OLPC_BUDDY,
            '@', "id", id_str,
            '(', "buddy", "",
              '@', "alias", priv->alias,
            ')',
        ')',
        NULL);
    }
  else
    {
      query = lm_message_build_with_sub_type (priv->conn->olpc_gadget_buddy,
          LM_MESSAGE_TYPE_IQ, LM_MESSAGE_SUB_TYPE_GET,
          '(', "view", "",
              '@', "xmlns", NS_OLPC_BUDDY,
              '@', "id", id_str,
              '(', "random", "",
                '@', "max", max_str,
              ')',
          ')',
          NULL);
    }

  g_free (max_str);
  g_free (id_str);

  if (!_gabble_connection_send_with_reply (priv->conn, query,
        buddy_view_query_result_cb, G_OBJECT (self), NULL, NULL))
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
        "Failed to send buddy search query to server");

      DEBUG ("Failed to send buddy search query to server");
      lm_message_unref (query);
      return FALSE;
    }

  lm_message_unref (query);

  return TRUE;
}

static void
channel_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelClass *klass = (TpSvcChannelClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_implement_##x (\
    klass, gabble_olpc_buddy_view_##x)
  IMPLEMENT(close);
  IMPLEMENT(get_channel_type);
  IMPLEMENT(get_handle);
  IMPLEMENT(get_interfaces);
#undef IMPLEMENT
}