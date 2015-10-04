/*
  Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "plugin_config.h"
#include "mysql_routing.h"
#include "mysqlrouter/routing.h"
#include "mysqlrouter/fabric_cache.h"

#include <algorithm>
#include <exception>
#include <vector>

#include "mysqlrouter/utils.h"

using std::invalid_argument;
using std::vector;

string RoutingPluginConfig::get_default(const string &option) {

  const std::map<string, string> defaults{
      {"connect_timeout", to_string(routing::kDefaultDestinationConnectionTimeout)},
      {"wait_timeout",    to_string(routing::kDefaultWaitTimeout)},
      {"max_connections", to_string(routing::kDefaultMaxConnections)},
  };

  auto it = defaults.find(option);
  if (it == defaults.end()) {
    return string();
  }
  return it->second;
}

bool RoutingPluginConfig::is_required(const string &option) {
  const vector<string> required{
      "bind_address",
      "mode",
      "destinations",
  };

  return std::find(required.begin(), required.end(), option) != required.end();
}

routing::AccessMode RoutingPluginConfig::get_option_mode(const ConfigSection *section, const string &option) {
  string value;
  string valid;

  for (auto &it: routing::kAccessModeNames) {
    valid += it.first + ", ";
  }
  valid.erase(valid.size() - 2, 2);  // remove the extra ", "

  try {
    value = get_option_string(section, option);
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
  } catch (const invalid_argument) {
    throw invalid_argument(get_log_prefix(option) + " needs to be specified; valid are " + valid);
  }

  auto lookup = routing::kAccessModeNames.find(value);
  if (lookup == routing::kAccessModeNames.end()) {
    throw invalid_argument(get_log_prefix(option) + " is invalid; valid are " + valid + " (was '" + value + "')");
  }

  return lookup->second;
}

string RoutingPluginConfig::get_option_destinations(const ConfigSection *section, const string &option) {
  bool required = is_required(option);
  string value;

  try {
    value = section->get(option);
  } catch (const bad_option &exc) {
    if (required) {
      throw invalid_argument(get_log_prefix(option) + " is required");
    }
  }

  if (value.empty()) {
    if (required) {
      throw invalid_argument(get_log_prefix(option) + " is required and needs a value");
    }
    value = get_default(option);
  }

  try {
    auto uri = URI(value); // raises URIError when URI is invalid
    if (uri.scheme == "fabric+cache") {
      auto fabric_cmd = uri.path[0];
      std::transform(fabric_cmd.begin(), fabric_cmd.end(), fabric_cmd.begin(), ::tolower);
      if (fabric_cmd != "group") {
        throw invalid_argument(
            get_log_prefix(option) + " has an invalid Fabric command in URI; was '" + fabric_cmd + "'");
      }
    } else {
      throw invalid_argument(
          get_log_prefix(option) + " has an invalid URI scheme '" + uri.scheme + "' for URI " + value);
    }
    return value;
  } catch (URIError) {
    std::stringstream ss(value);
    std::string part;
    std::pair<std::string, uint16_t> info;
    while (std::getline(ss, part, ',')) {
      info = mysqlrouter::split_addr_port(part);
      if (info.second == 0) {
        info.second = 3306;
      }
      TCPAddress addr(info.first, info.second);
      if (!addr.is_valid()) {
        throw invalid_argument(get_log_prefix(option) + " has an invalid destination address '" + addr.str() + "'");
      }
    }
  }

  return value;
}