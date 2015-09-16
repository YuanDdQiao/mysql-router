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

#include "mysqlrouter/fabric_cache.h"
#include "fabric_cache.h"

#include <map>
#include <memory>

std::map<string, std::unique_ptr<FabricCache> > g_fabric_caches;
std::mutex fabrix_caches_mutex;

namespace fabric_cache {

const uint16_t kDefaultFabricPort = 32275;
const string kDefaultFabricAddress{"127.0.0.1:" + mysqlrouter::to_string(kDefaultFabricPort)};
const string kDefaultFabricUser = "";
const string kDefaultFabricPassword = "";

void cache_init(const string &cache_name, const string &host, const int port,
                const string &user,
                const string &password) {
  if (g_fabric_caches.find(cache_name) != g_fabric_caches.end()) {
    return;
  }

  std::lock_guard<std::mutex> lock(fabrix_caches_mutex);
  g_fabric_caches.emplace(
      static_cast<string>(cache_name),
      std::unique_ptr<FabricCache>(
          new FabricCache(host, port, user, password, 1, 1))
  );

  auto cache = g_fabric_caches.find(cache_name);
  cache->second->start();
}

bool have_cache(const string &cache_name) {
  return g_fabric_caches.find(cache_name) != g_fabric_caches.end();
}

LookupResult lookup_group(const string &cache_name, const string &group_id) {
  auto cache = g_fabric_caches.find(cache_name);
  if (cache == g_fabric_caches.end()) {
    throw fabric_cache::base_error("Fabric Cache '" + cache_name + "' not initialized");
  }
  return LookupResult(cache->second->group_lookup(group_id));
}

LookupResult lookup_shard(const string &cache_name, const string &table_name,
                          const string &shard_key) {
  auto cache = g_fabric_caches.find(cache_name);
  if (cache == g_fabric_caches.end()) {
    throw fabric_cache::base_error("Fabric Cache '" + cache_name + "' not initialized");
  }
  return LookupResult(cache->second->shard_lookup(table_name, shard_key));
}

} // namespace fabric_cache

