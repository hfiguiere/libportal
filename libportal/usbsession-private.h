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

#include "session-private.h"
#include "usbsession.h"

struct _XdpUsbSession {
  GObject parent_instance;
  XdpSession *parent_session; /* strong ref */

  /* USB Session implementation */
  guint signal_id;
};

XdpUsbSession *
_xdp_usb_session_new (XdpPortal *portal, const char *session_path);
