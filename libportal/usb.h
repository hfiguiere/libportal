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

#pragma once

#include <libportal/types.h>
#include <libportal/session.h>
#include <libportal/usbsession.h>

G_BEGIN_DECLS

typedef struct _XdpUsbDeviceAcquireRequest XdpUsbDeviceAcquireRequest;

XDP_PUBLIC
XdpUsbDeviceAcquireRequest *xdp_usb_device_acquire_request_new (const char *id, gboolean writable);

XDP_PUBLIC
void            xdp_usb_device_acquire_request_free   (XdpUsbDeviceAcquireRequest *device);

XDP_PUBLIC
void            xdp_portal_usb_create_session         (XdpPortal           *portal,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             data);

XDP_PUBLIC
XdpUsbSession * xdp_portal_usb_create_session_finish (XdpPortal *portal,
                                                      GAsyncResult *result,
                                                      GError **error);

XDP_PUBLIC
GVariant *      xdp_portal_usb_enumerate_devices      (XdpPortal           *portal,
                                                       GError             **error);

XDP_PUBLIC
void            xdp_portal_usb_release_devices        (XdpPortal           *portal,
                                                       GSList              *devices,
                                                       GError             **error);

XDP_PUBLIC
void            xdp_portal_usb_acquire_devices        (XdpPortal           *portal,
                                                       XdpParent           *parent,
                                                       GPtrArray           *devices,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             data);

XDP_PUBLIC
char *          xdp_portal_usb_acquire_devices_finish (XdpPortal     *portal,
                                                       GAsyncResult  *result,
                                                       GError       **error);

XDP_PUBLIC
GSList *        xdp_portal_usb_finish_acquire_devices (XdpPortal           *portal,
                                                       const char          *object_path,
                                                       GError              **error);

XDP_PUBLIC
void            xdp_usb_device_list_free              (GSList *device_list);

G_END_DECLS
