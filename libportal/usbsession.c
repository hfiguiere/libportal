/*
 * Copyright (C) 2024 GNOME Foundation, Inc.
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 3.0 of the
 * License.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-3.0
 */

#include "config.h"

#include <libportal/usbsession.h>

#include "usbsession-private.h"
#include "portal-private.h"

/**
 * XdpUsbSession
 *
 * A USB session to monitor USB devices.
 *
 * The XdpUsbSession object is used to monitor when USB device events,
 * added or removed. Only enumerable USB devices will be reported.
 *
 * This is created by calling [method@Portal.usb_create_session].
 */
enum {
  DEVICE_EVENT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (XdpUsbSession, xdp_usb_session, G_TYPE_OBJECT)

static void
xdp_usb_session_init (XdpUsbSession *session)
{
  session->parent_session = NULL;

  session->signal_id = 0;
}

static void
parent_session_destroy (gpointer data, GObject *old_session)
{
  XdpUsbSession *session = XDP_USB_SESSION (data);

  g_critical ("XdpSession destroyed before XdpUsbSesssion, you lost count of your session refs");

  session->parent_session = NULL;
}

static void
xdp_usb_session_finalize (GObject *object)
{
  XdpUsbSession *session = XDP_USB_SESSION (object);
  XdpSession *parent_session = session->parent_session;

  if (parent_session == NULL)
    {
      g_critical ("XdpSession destroyed before XdpInputCaptureSesssion, you lost count of your session refs");
    }
  else
    {
      if (session->signal_id)
        g_dbus_connection_signal_unsubscribe (session->parent_session->portal->bus, session->signal_id);

      g_object_weak_unref (G_OBJECT (parent_session), parent_session_destroy, session);
      session->parent_session->usb_session = NULL;
      g_clear_pointer (&session->parent_session, g_object_unref);
    }

  G_OBJECT_CLASS (xdp_usb_session_parent_class)->finalize (object);
}

static void
xdp_usb_session_class_init (XdpUsbSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdp_usb_session_finalize;

  /**
   * XdpUsbSession::device-event:
   * @session: the [class@UsbSession]
   * @parameter: the device event
   *
   * Emitted when a device event is received.
   */
  signals[DEVICE_EVENT] =
    g_signal_new ("device-event",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_CLEANUP | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_VARIANT);
}

static void
device_event (GDBusConnection *bus,
              const char *sender_name,
              const char *object_path,
              const char *interface_name,
              const char *signal_name,
              GVariant *parameters,
              gpointer data)
{
  XdpUsbSession *session = data;

  GVariant *events = NULL;
  g_variant_get(parameters, "(o@a(ssa{sv}))", NULL, &events);
  g_signal_emit_by_name (session, "device-event", events);
}

XdpUsbSession *
_xdp_usb_session_new (XdpPortal *portal,
                      const char *session_path)
{
  XdpUsbSession *session;

  g_autoptr(XdpSession) parent_session = _xdp_session_new (portal, session_path, XDP_SESSION_INPUT_CAPTURE);

  session = g_object_new (XDP_TYPE_USB_SESSION, NULL);
  parent_session->usb_session = session; /* weak ref */
  g_object_weak_ref (G_OBJECT (parent_session), parent_session_destroy, session);
  session->parent_session = g_object_ref (parent_session);

  session->signal_id = g_dbus_connection_signal_subscribe (portal->bus,
                                                           PORTAL_BUS_NAME,
                                                           USB_INTERFACE,
                                                           "DeviceEvents",
                                                           NULL, /* no path */
                                                           NULL,
                                                           G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                           device_event,
                                                           session,
                                                           NULL);
  return session;
}

/**
 * xdp_usb_session_close:
 * @session: a [class@XdpUsbSession]
 *
 * Close the session.
 */
void
xdp_usb_session_close (XdpUsbSession *session)
{
  xdp_session_close (session->parent_session);
}

/**
 * xdp_usb_session_get_session:
 * @session: a [class@XdpUsbSession]
 *
 * Return the [class@XdpSession] for this InputCapture session.
 *
 * Returns: (transfer none): a [class@Session] object
 */
XdpSession *
xdp_usb_session_get_session (XdpUsbSession *session)
{
  return session->parent_session;
}
