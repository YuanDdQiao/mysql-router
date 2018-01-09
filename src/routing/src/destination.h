/*
  Copyright (c) 2015, 2018, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef ROUTING_DESTINATION_INCLUDED
#define ROUTING_DESTINATION_INCLUDED

#include "router_config.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mysqlrouter/datatypes.h"
#include "mysqlrouter/routing.h"
#include "mysql/harness/logging/logging.h"
#include "protocol/protocol.h"
IMPORT_LOG_FUNCTIONS()

/** @class RouteDestination
 * @brief Manage destinations for a Connection Routing
 *
 * This class manages destinations which are used in Connection Routing.
 * A destination is usually a MySQL Server and is stored using the IP
 * or hostname together with the TCP port (defaulting to 3306 for classic
 * protocol or to 33060 for x protocol).
 *
 * RouteDestination is meant to be a base class and used to inherite and
 * create class which change the behavior. For example, the `get_next()`
 * method is usually changed to get the next server in the list.
 */
class RouteDestination {
public:

  using AddrVector = std::vector<mysqlrouter::TCPAddress>;

  /** @brief Default constructor */
  RouteDestination(Protocol::Type protocol = Protocol::get_default(),
                   routing::SocketOperationsBase *sock_ops =
                     routing::SocketOperations::instance()) // default = "real" (not mock) implementation
      : current_pos_(0), socket_operations_(sock_ops), protocol_(protocol) {}

  /** @brief Destructor */
  virtual ~RouteDestination() {}

  RouteDestination(const RouteDestination &other) = delete;
  RouteDestination(RouteDestination &&other) = delete;
  RouteDestination &operator=(const RouteDestination &other) = delete;
  RouteDestination &operator=(RouteDestination &&other) = delete;

  /** @brief Adds a destination
   *
   * Adds a destination using the given address and port number.
   *
   * @param dest destination address
   */
  virtual void add(const mysqlrouter::TCPAddress dest);

  /** @overload */
  virtual void add(const std::string &address, uint16_t port);

  /** @brief Removes a destination
   *
   * Removes a destination using the given address and port number.
   *
   * @param address IP or name
   * @param port Port number
   */
  virtual void remove(const std::string &address, uint16_t port);

  /** @brief Gets destination based on address and port
   *
   * Gets destination base on given address and port and returns a pair
   * with the information.
   *
   * Raises std::out_of_range when the combination of address and port
   * is not in the list of destinations.
   *
   * This function can be used to check whether given destination is in
   * the list.
   *
   * @param address IP or name
   * @param port Port number
   * @return an instance of mysqlrouter::TCPAddress
   */
  virtual mysqlrouter::TCPAddress get(const std::string &address, uint16_t port);

  /** @brief Removes all destinations
   *
   * Removes all destinations from the list.
   */
  virtual void clear();

  /** @brief Gets next connection to destination
   *
   * Returns a socket descriptor for the connection to the MySQL Server or
   * -1 when an error occurred, which means that no destination was
   * available.
   *
   * @param connect_timeout timeout
   * @param error Pointer to int for storing errno
   * @return a socket descriptor
   */
  virtual int get_server_socket(std::chrono::milliseconds connect_timeout, int *error) noexcept = 0;

  /** @brief Gets the number of destinations
   *
   * Gets the number of destinations currently in the list.
   *
   * @return Number of destinations as size_t
   */
  size_t size() noexcept;

  /** @brief Returns whether there are destinations
   *
   * @return whether the destination is empty
   */
  virtual bool empty() const noexcept {
    return destinations_.empty();
  }

  /** @brief Start the destination threads (if any)
   *
   */
  virtual void start() {}

  AddrVector::iterator begin() {
    return destinations_.begin();
  }

  AddrVector::const_iterator begin() const {
    return destinations_.begin();
  }

  AddrVector::iterator end() {
    return destinations_.end();
  }

  AddrVector::const_iterator end() const {
    return destinations_.end();
  }

protected:
  /** @brief Returns socket descriptor of connected MySQL server
   *
   * Returns a socket descriptor for the connection to the MySQL Server or
   * -1 when an error occurred.
   *
   * This method normally calls SocketOperations::get_mysql_socket() (default
   * "real" implementation), but can be configured to call another implementation
   * (e.g. a mock counterpart).
   *
   * @param addr information of the server we connect with
   * @param connect_timeout timeout waiting for connection
   * @param log_errors whether to log errors or not
   * @return a socket descriptor
   */
  virtual int get_mysql_socket(const mysqlrouter::TCPAddress &addr, std::chrono::milliseconds connect_timeout, bool log_errors = true);

  /** @brief Gets the id of the next server to connect to.
   *
   * @throws std::logic_error if destinations list is empty
   */
  size_t get_next_server();

  /** @brief List of destinations */
  AddrVector destinations_;

  /** @brief Destination which will be used next */
  std::atomic<size_t> current_pos_;

  /** @brief Mutex for updating destinations and iterator */
  std::mutex mutex_update_;

  /** @brief socket operation methods (facilitates dependency injection)*/
  routing::SocketOperationsBase *socket_operations_;

  /** @brief Protocol for the destination */
  Protocol::Type protocol_;
};


#endif // ROUTING_DESTINATION_INCLUDED
