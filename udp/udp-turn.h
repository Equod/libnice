/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2008 Collabora Ltd.
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
 *   Dafydd Harries, Collabora Ltd.
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

#ifndef _GOOGLE_RELAY_H
#define _GOOGLE_RELAY_H

#include "udp.h"

G_BEGIN_DECLS

typedef enum {
  NICE_UDP_TURN_SOCKET_COMPATIBILITY_TD9,
  NICE_UDP_TURN_SOCKET_COMPATIBILITY_GOOGLE,
  NICE_UDP_TURN_SOCKET_COMPATIBILITY_MSN,
} NiceUdpTurnSocketCompatibility;

gint
nice_udp_turn_socket_parse_recv (
  NiceUDPSocket *sock,
  NiceAddress *from,
  guint len,
  gchar *buf,
  NiceAddress *recv_from,
  gchar *recv_buf,
  guint recv_len);

gboolean
nice_udp_turn_create_socket_full (
  NiceUDPSocketFactory *man,
  NiceUDPSocket *sock,
  NiceAddress *addr,
  NiceUDPSocket *udp_socket,
  NiceAddress *server_addr,
  gchar *username,
  gchar *password,
  NiceUdpTurnSocketCompatibility compatibility);


void
nice_udp_turn_socket_factory_init (NiceUDPSocketFactory *man);

G_END_DECLS

#endif /* _UDP_BSD_H */
