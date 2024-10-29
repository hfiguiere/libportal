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

#include "portal-private.h"
#include "usb.h"
#include "usbsession.h"
#include "usbsession-private.h"

/**
 * XdpUsbDevice
 *
 * Describe an acquired device.
 */
typedef struct {
  char *id;
  gboolean success;
  int fd;
  char *error;
} XdpUsbDevice;

static void xdp_usb_device_free (XdpUsbDevice *device);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpUsbDevice, xdp_usb_device_free)

/**
 * XdpUsbDeviceAcquireRequest
 *
 * A device to request.
 */
struct _XdpUsbDeviceAcquireRequest {
  char *id;
  gboolean writable;
};

/**
 * xdp_usb_device_acquire_request_new:
 * @id the device id
 * @writable if the device is writable
 *
 * Returns: (transfer full): a new [struct@XdpUsbDeviceAcquireRequest]
 */
XdpUsbDeviceAcquireRequest *
xdp_usb_device_acquire_request_new (const char *id, gboolean writable)
{
  XdpUsbDeviceAcquireRequest *dev = g_new0 (XdpUsbDeviceAcquireRequest, 1);
  dev->writable = writable;
  dev->id = g_strdup (id);

  return dev;
}

/**
 * xdp_usb_device_acquire_request_copy:
 * @source: a [struct@UsbDeviceAcquireRequest]
 *
 * Returns: (transfer full): an [struct@UsbDeviceAcquireRequest] that is
 *  a copy of @source
 */
XdpUsbDeviceAcquireRequest *
xdp_usb_device_acquire_request_copy (XdpUsbDeviceAcquireRequest *source)
{
  return xdp_usb_device_acquire_request_new (source->id, source->writable);
}

/**
 * xdp_usb_device_acquire_request_free:
 * @device: an [struct@UsbDeviceAcquireRequest].
 *
 * Frees #device.
 */
void
xdp_usb_device_acquire_request_free (XdpUsbDeviceAcquireRequest *device)
{
  g_return_if_fail (device);

  g_free (device->id);
  g_free (device);
}

G_DEFINE_BOXED_TYPE (XdpUsbDeviceAcquireRequest, xdp_usb_device_acquire_request, xdp_usb_device_acquire_request_copy, xdp_usb_device_acquire_request_free)

/* A request-based method call */
typedef struct {
  XdpPortal *portal;
  GTask *task;
  guint signal_id; /* Request::Response signal */
  char *request_path; /* object path for request */
  guint cancelled_id; /* signal id for cancelled gobject signal */

  /* CreateSession */
  XdpUsbSession *session;

  /* AcquireDevices */
  XdpParent *parent;
  char *parent_handle;
  GPtrArray *devices;
} UsbCall;

static void
usb_call_free (UsbCall *call)
{
  if (call->signal_id)
    g_dbus_connection_signal_unsubscribe (call->portal->bus, call->signal_id);

  if (call->cancelled_id)
    g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);

  g_free (call->request_path);

  g_clear_object (&call->portal);
  g_clear_object (&call->task);
  g_clear_object (&call->session);

  g_clear_pointer (&call->parent, xdp_parent_free);
  g_free (call->parent_handle);
  g_ptr_array_free (call->devices, TRUE);

  g_free (call);
}

static void
usb_call_cancelled_cb (GCancellable *cancellable,
                   gpointer data)
{
  UsbCall *call = data;

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          call->request_path,
                          REQUEST_INTERFACE,
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, NULL, NULL);
}

static void
usb_session_call_returned (GObject *object,
                           GAsyncResult *result,
                           gpointer data)
{
  UsbCall *call = data;
  GError *error = NULL;
  g_autoptr(GVariant) ret;
  XdpUsbSession *session;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  if (error)
    {
      if (call->cancelled_id)
        {
          g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);
          call->cancelled_id = 0;
        }
      g_task_return_error (call->task, error);
    }
  else
    {
      const char *session_path = NULL;
      g_variant_get (ret, "(&o)", &session_path);
      session = _xdp_usb_session_new (call->portal, session_path);
      g_task_return_pointer (call->task, session, g_object_unref);
    }
  usb_call_free (call);
}

static void
usb_call_returned (GObject *object,
                   GAsyncResult *result,
                   gpointer data)
{
  UsbCall *call = data;
  GError *error = NULL;
  g_autoptr(GVariant) ret;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  if (error)
    {
      if (call->cancelled_id)
        {
          g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);
          call->cancelled_id = 0;
        }
      g_task_return_error (call->task, error);
      usb_call_free (call);
    }
}

static void
create_session (UsbCall *call)
{
  GVariantBuilder options;
  g_autofree char *session_token = NULL;
  GCancellable *cancellable;

  cancellable = g_task_get_cancellable (call->task);
  if (cancellable)
    call->cancelled_id = g_signal_connect (cancellable, "cancelled", G_CALLBACK (usb_call_cancelled_cb), call);

  session_token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "session_handle_token", g_variant_new_string (session_token));

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.Usb",
                          "CreateSession",
                          g_variant_new ("(a{sv})", &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          usb_session_call_returned,
                          call);
}

/**
 * xdp_portal_usb_create_session_finish:
 * @portal: a [class@Portal]
 * @result: a [iface@Gio.AsyncResult]
 * @error: return location for an error
 *
 * Finishes the USB CreateSession request, and returns a
 * [class@UsbSession]. To get to the [class@Session] within use
 * xdp_usb_session_get_session().
 *
 * Returns: (transfer full) (nullable): a [class@UsbSession]
 */
XdpUsbSession *
xdp_portal_usb_create_session_finish (XdpPortal *portal,
                                      GAsyncResult *result,
                                      GError **error)
{
  g_return_val_if_fail (XDP_IS_PORTAL (portal), NULL);
  g_return_val_if_fail (g_task_is_valid (result, portal), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/**
 * xdp_portal_usb_create_session:
 * @portal: a [class@Portal]
 *
 * Create a session used to watch for USB device events.
 *
 * When the request is done, @callback will be called. You can then
 * call [method@Portal.usb_create_session_finish] to get the results.
 */
void
xdp_portal_usb_create_session (XdpPortal *portal,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer data)
{
  UsbCall *call;

  g_return_if_fail (XDP_IS_PORTAL (portal));

  call = g_new0 (UsbCall, 1);
  call->portal = g_object_ref (portal);
  call->task = g_task_new (portal, cancellable, callback, data);

  create_session (call);
}


/**
 * xdp_portal_usb_enumerate_devices:
 * @portal: a [class@Portal]
 *
 * Returns: (transfer full): a GVariant
 */
GVariant *
xdp_portal_usb_enumerate_devices (XdpPortal *portal,
                                  GError   **error)
{
  g_return_val_if_fail (XDP_IS_PORTAL (portal), NULL);
  GVariantBuilder options;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);

  return g_dbus_connection_call_sync (portal->bus,
                                      PORTAL_BUS_NAME,
                                      PORTAL_OBJECT_PATH,
                                      "org.freedesktop.portal.Usb",
                                      "EnumerateDevices",
                                      g_variant_new("(a{sv})", &options),
                                      G_VARIANT_TYPE ("(a(sa{sv}))"),
                                      G_DBUS_CALL_FLAGS_NONE,
                                      -1,
                                      NULL, error);
}

static void acquire_devices (UsbCall *call);

static void
parent_exported (XdpParent *parent,
                 const char *handle,
                 gpointer data)
{
  UsbCall *call = data;
  call->parent_handle = g_strdup (handle);
  acquire_devices (call);
}

static void
acquire_response_received (GDBusConnection *bus,
                           const char *sender_name,
                           const char *object_path,
                           const char *interface_name,
                           const char *signal_name,
                           GVariant *parameters,
                           gpointer data)
{
  UsbCall *call = data;
  guint32 response;
  GVariant *devices;
  g_autoptr(GVariant) ret = NULL;

  if (call->cancelled_id)
    {
      g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);
      call->cancelled_id = 0;
    }

  g_variant_get (parameters, "(s@a(sa{sv}))", &response, &devices);

  if (response == 0)
    {
      GSList *device_list = NULL;
      char *device_id = NULL;
      GVariant *device;
      g_autoptr(GVariantIter) iter = g_variant_iter_new (devices);
      while (g_variant_iter_next (iter, "&s@a{sv}", &device_id, &device))
        {
          gint fd = -1;
          gboolean success = FALSE;

          if (g_variant_lookup (device, "success", "b", &success) && success)
            if (g_variant_lookup (device, "fd", "h", fd))
              /* XXX shall we also pass the id ? */
              device_list = g_slist_append (device_list, GINT_TO_POINTER (fd));
        }

      g_task_return_pointer (call->task, device_list, (GDestroyNotify)g_slist_free);
    }
  else if (response == 1)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Acquire USB devices canceled");
  else
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Acquire USB devices failed");

  usb_call_free (call);
}

static void
acquire_cancelled_cb (GCancellable *cancellable,
                      gpointer data)
{
  UsbCall *call = data;

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          call->request_path,
                          REQUEST_INTERFACE,
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, NULL, NULL);

  g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Acquire USB devices call canceled by caller");

  usb_call_free (call);
}

static void
add_to_variant (XdpUsbDeviceAcquireRequest *dev, GVariantBuilder *builder)
{
  g_auto(GVariantDict) dict = G_VARIANT_DICT_INIT (NULL);
  g_variant_dict_insert (&dict, "writable", "b", dev->writable);

  g_variant_builder_add (builder, "(s@a{sv})", dev->id, g_variant_dict_end (&dict));
}

static void
acquire_devices (UsbCall *call)
{
  g_autoptr(GVariant) options;
  g_autoptr(GVariant) devices;
  GVariantBuilder options_builder;
  GVariantBuilder devices_builder;
  g_autofree char *token = NULL;
  GCancellable *cancellable;

  if (call->parent_handle == NULL)
    {
      call->parent->parent_export (call->parent, parent_exported, call);
      return;
    }
  token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  call->request_path = g_strconcat (REQUEST_PATH_PREFIX, call->portal->sender, "/", token, NULL);
  call->signal_id = g_dbus_connection_signal_subscribe (call->portal->bus,
                                                        PORTAL_BUS_NAME,
                                                        REQUEST_INTERFACE,
                                                        "Response",
                                                        call->request_path,
                                                        NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                        acquire_response_received,
                                                        call,
                                                        NULL);

  cancellable = g_task_get_cancellable (call->task);
  if (cancellable)
    call->cancelled_id = g_signal_connect (cancellable, "cancelled", G_CALLBACK (acquire_cancelled_cb), call);

  g_variant_builder_init (&devices_builder, G_VARIANT_TYPE ("a(sa{sv})"));
  g_ptr_array_foreach (call->devices, (GFunc) add_to_variant, &devices_builder);
  devices = g_variant_ref_sink (g_variant_builder_end (&devices_builder));

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options_builder, "{sv}", "handle_token", g_variant_new_string (token));
  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.Usb",
                          "AcquireDevices",
                          g_variant_new("(s@a(sa{sv})@a{sv})", call->parent_handle, devices, options),
                          G_VARIANT_TYPE ("o"),
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, usb_call_returned, call);
}

static XdpUsbDeviceAcquireRequest*
usb_device_acquire_request_copy (XdpUsbDeviceAcquireRequest *source, gpointer)
{
  return xdp_usb_device_acquire_request_copy (source);
}

/**
 * xdp_portal_usb_acquire_devices:
 * @portal: a [class@Portal]
 * @parent: (nullable): parent window information
 * @devices: (element-type XdpUsbDeviceAcquireRequest): the devices
 * @cancellable: (nullable): optional [class@Gio.Cancellable]
 * @callback: (scope async): a callback to call when the request is done
 * @data: (closure): data to pass to @callback
 *
 * Acquire devices.
 *
 * When the request is done, @callback will be called. You can then
 * call [method@Portal.usb_finish_acquire_devices] to get the results.
 */
void
xdp_portal_usb_acquire_devices (XdpPortal *portal,
                                XdpParent *parent,
                                GPtrArray *devices,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer data)
{
  UsbCall *call;

  g_return_if_fail (XDP_IS_PORTAL (portal));

  call = g_new0 (UsbCall, 1);
  call->portal = g_object_ref (portal);
  if (parent)
    call->parent = xdp_parent_copy (parent);
  else
    call->parent_handle = g_strdup ("");
  call->devices = g_ptr_array_copy (devices, (GCopyFunc) usb_device_acquire_request_copy, NULL);

  call->task = g_task_new (portal, cancellable, callback, data);
  g_task_set_source_tag (call->task, xdp_portal_usb_acquire_devices);

  acquire_devices (call);
}

/*
 * xdp_portal_usb_acquire_devices_finish:
 * @portal: a [class@Portal]
 * @result: a [iface@Gio.AsyncResult]
 * @error: return location for an error
 *
 * Finish the acquire device request.
 *
 * Returns: (transfer-full utf8) The object path of the request
 *   to pass to [method@XdgPortal.usb_finish_acquire_devices].
 */
char *
xdp_portal_usb_acquire_devices_finish (XdpPortal     *portal,
                                       GAsyncResult  *result,
                                       GError       **error)
{
  g_return_val_if_fail (XDP_IS_PORTAL (portal), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, portal), FALSE);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == xdp_portal_usb_acquire_devices, FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}


static XdpUsbDevice *
xdp_usb_device_new (const char *id, int fd)
{
  XdpUsbDevice *device = g_new0 (XdpUsbDevice, 1);
  device->id = g_strdup (id);
  device->success = TRUE;
  device->error = NULL;
  device->fd = fd;
  return device;
}

static XdpUsbDevice *
xdp_usb_device_error_new (const char *id, const char *error)
{
  XdpUsbDevice *device = g_new0 (XdpUsbDevice, 1);
  device->id = g_strdup (id);
  device->success = FALSE;
  device->error = g_strdup (error);
  device->fd = -1;
  return device;
}

static void
xdp_usb_device_free (XdpUsbDevice *device)
{
  g_return_if_fail (device);

  g_free (device->id);
  g_free (device->error);
  g_free (device);
}

/*
 * xdp_usb_device_list_free:
 * @device_list: (element-type XdpUsbDevice): A list
 *   of [struct@UsbDevice] for the devices.
 *
 * Free the device list.
 */
void
xdp_usb_device_list_free (GSList *device_list)
{
  g_slist_free_full (device_list, (GDestroyNotify) xdp_usb_device_free);
}

static GSList *
append_variant_to_fd_list (GSList *fd_list, GVariant *fd_list_variant)
{
  g_autoptr(GVariant) device;
  const char *id;
  g_autoptr(GVariantIter) iter;
  g_return_val_if_fail (fd_list_variant, fd_list);

  iter = g_variant_iter_new (fd_list_variant);
  while (g_variant_iter_next (iter, "s&a{sv}", &id, &device))
    {
      const char* error = NULL;
      int32_t fd = -1;
      gboolean success = FALSE;
      XdpUsbDevice *xdp_device;

      g_variant_lookup (device, "error", "&s", &error);
      g_variant_lookup (device, "fd", "h", &fd);
      g_variant_lookup (device, "success", "b", &success);

      if (success)
        xdp_device = xdp_usb_device_new (id, fd);
      else
        xdp_device = xdp_usb_device_error_new (id, error);

      fd_list = g_slist_append (fd_list, xdp_device);
    }

  return fd_list;
}

/**
 * xdp_portal_usb_finish_acquire_devices:
 * @portal: a [class@Portal]
 * @object_path: the object path from the request to finish.
 * @error: return location for an error
 *
 * Finishes the USB acquisition request.
 *
 * Returns the result in the form of a List containing file descriptors.
 *   The list should be freed with [method@UsbDeviceList.free].
 *
 * Returns: (nullable) (element-type XdpUsbDevice) (transfer full): A list
 *   of [struct@UsbDevice] for the devices.
 */
GSList *
xdp_portal_usb_finish_acquire_devices (XdpPortal  *portal,
				       const char *object_path,
                                       GError    **error)

{
  gboolean finished = FALSE;
  GVariantBuilder options;
  g_autoptr(GSList) fd_list;
  g_return_val_if_fail (XDP_IS_PORTAL (portal), NULL);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);

  while (!finished)
    {
      gboolean finished_variant = FALSE;
      GVariant *fd_list_variant;
      g_autoptr(GVariant) result = NULL;

      result = g_dbus_connection_call_sync (portal->bus,
                                            PORTAL_BUS_NAME,
                                            PORTAL_OBJECT_PATH,
                                            "org.freedesktop.portal.Usb",
                                            "AcquireDevicesFinish",
                                            g_variant_new("(s@a{sv})", object_path,
                                                          g_variant_builder_end (&options)),
                                            G_VARIANT_TYPE ("(s(sa{sv})b)"),
                                            G_DBUS_CALL_FLAGS_NONE,
                                            -1,
                                            NULL, error);
      if (result)
        {
          g_variant_get (result, "s(sa{sv})@b", &fd_list_variant, &finished_variant);
          fd_list = append_variant_to_fd_list (fd_list, fd_list_variant);
          finished |= finished_variant;
        }
    }

  return g_steal_pointer (&fd_list);
}

static void
add_device(gpointer data, gpointer user_data)
{
  GVariantBuilder *builder = (GVariantBuilder*)user_data;
  g_variant_builder_add (builder, "s", g_strdup ((const char*) data));
}

/**
 * xdp_portal_usb_release_devices:
 * @portal: a [class@Portal]
 * @devices: (element-type utf8): a [class@Glib.SList] of devices.
 */
void
xdp_portal_usb_release_devices        (XdpPortal           *portal,
                                       GSList              *devices,
                                       GError             **error)
{
  g_autoptr(GVariant) ret = NULL;
  GVariantBuilder device_list;
  g_return_if_fail (XDP_IS_PORTAL (portal));

  g_variant_builder_init (&device_list, G_VARIANT_TYPE ("as"));
  g_slist_foreach (devices, add_device, &device_list);

  ret = g_dbus_connection_call_sync (portal->bus,
                                     PORTAL_BUS_NAME,
                                     PORTAL_OBJECT_PATH,
                                     "org.freedesktop.portal.Usb",
                                     "ReleaseDevices",
                                     g_variant_new ("(as)", device_list),
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL, error);
}
