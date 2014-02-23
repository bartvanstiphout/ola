/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * NetworkManagerInterface.h
 * The glue between the ResponderHelper and the OLA network code.
 * Copyright (C) 2013-2014 Peter Newman
 */

/**
 * @addtogroup rdm_resp
 * @{
 * @file NetworkManagerInterface.h
 * @brief The interface for the NetworkManager.
 * @}
 */

#ifndef INCLUDE_OLA_RDM_NETWORKMANAGERINTERFACE_H_
#define INCLUDE_OLA_RDM_NETWORKMANAGERINTERFACE_H_

#include <ola/network/IPV4Address.h>
#include <ola/network/Interface.h>
#include <ola/network/InterfacePicker.h>
#include <ola/rdm/RDMEnums.h>

#include <string>
#include <vector>

namespace ola {
namespace rdm {

/**
 * @brief Gets global network information.
 */
class NetworkManagerInterface {
 public:
  NetworkManagerInterface() {}
  virtual ~NetworkManagerInterface() {}

  /**
   * Get the interface picker
   */
  virtual const ola::network::InterfacePicker *GetInterfacePicker() const = 0;

  /**
   * Get the DHCP status of an interface
   * @param iface the interface to check the DHCP status of
   * @return One of DHCP_STATUS_ENABLED, DHCP_STATUS_DISABLED or
   * DHCP_STATUS_UNKNOWN.
   */
  virtual dhcp_status GetDHCPStatus(
      const ola::network::Interface &iface) const = 0;

  /**
   * Get the IPv4 default route
   * @param[out] the machine's default route as an IPV4Address object
   * @return true if we managed to fetch the default route, false otherwise
   * @note if it manages to fetch the route information and there isn't a route,
   * it will return the special wildcard address, which can be tested for with
   * IsWildcard().
   */
  virtual bool GetIPV4DefaultRoute(
      ola::network::IPV4Address *default_route) const = 0;

  /**
   * Get the hostname
   */
  virtual const std::string GetHostname() const = 0;

  /**
   * Get the domain name
   */
  virtual const std::string GetDomainName() const = 0;

  /**
   * Get name servers
   */
  virtual bool GetNameServers(
      std::vector<ola::network::IPV4Address> *name_servers) const = 0;
};
}  // namespace rdm
}  // namespace ola
#endif  // INCLUDE_OLA_RDM_NETWORKMANAGERINTERFACE_H_
