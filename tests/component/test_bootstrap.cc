/*
  Copyright (c) 2017, 2018, Oracle and/or its affiliates. All rights reserved.

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
#include <fstream>
#include <regex>
#include <system_error>

#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#ifndef __APPLE__
#include <ifaddrs.h>
#include <net/if.h>
#endif
#endif

#include "gmock/gmock.h"
#include "router_component_test.h"

/**
 * @file
 * @brief Component Tests for the bootstrap operation
 */

Path g_origin_path;

/**
 * @todo  this is a copy of HostnameOperations from cluster_metadata.cc
 *        and shouldn't be here.
 */
static
std::string get_my_hostname() {
  char buf[1024] = {0};
#if defined(_WIN32) || defined(__APPLE__) || defined(__FreeBSD__)
  if (gethostname(buf, sizeof(buf)) < 0) {
    // log_error("Could not get hostname: %s", mysql_harness::get_message_error(msg);
    throw std::runtime_error("Could not get local hostname");
  }
#else
  struct ifaddrs *ifa = nullptr, *ifap;
  int ret = -1, family;
  socklen_t addrlen;

  std::shared_ptr<ifaddrs> ifa_deleter(nullptr, [&](void*){if (ifa) freeifaddrs(ifa);});
  if ((ret = getifaddrs(&ifa)) != 0 || !ifa) {
    throw std::runtime_error("Could not get local host address: " + std::generic_category().default_error_condition(errno).message()
                             + " (ret: " + std::to_string(ret)
                             + ", errno: " + std::to_string(errno) + ")");
  }
  for (ifap = ifa; ifap != NULL; ifap = ifap->ifa_next) {
    if ((ifap->ifa_addr == NULL) || (ifap->ifa_flags & IFF_LOOPBACK) || (!(ifap->ifa_flags & IFF_UP)))
      continue;
    family = ifap->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6)
      continue;
    if (family == AF_INET6) {
      struct sockaddr_in6 *sin6;

      sin6 = (struct sockaddr_in6 *)ifap->ifa_addr;
      if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) || IN6_IS_ADDR_MC_LINKLOCAL(&sin6->sin6_addr))
        continue;
    }
    addrlen = static_cast<socklen_t>((family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
    ret = getnameinfo(ifap->ifa_addr, addrlen, buf,
        static_cast<socklen_t>(sizeof(buf)), NULL, 0, NI_NAMEREQD);
  }
  if (ret != EAI_NONAME && ret != 0) {
    throw std::runtime_error("Could not get local host address: " + std::string(gai_strerror(ret))
                             + " (ret: " + std::to_string(ret)
                             + ", errno: " + std::to_string(errno) + ")");
  }
#endif
  return buf;
}


class RouterBootstrapTest : public RouterComponentTest, public ::testing::Test {
 protected:
  virtual void SetUp() {
    set_origin(g_origin_path);
    RouterComponentTest::SetUp();
    bootstrap_dir = get_tmp_dir();
    tmp_dir = get_tmp_dir();
    my_hostname = get_my_hostname();
  }

  virtual void TearDown() {
    purge_dir(tmp_dir);
    purge_dir(bootstrap_dir);
  }

  TcpPortPool port_pool_;
  std::string bootstrap_dir;
  std::string tmp_dir;
  std::string my_hostname;

  struct Config {
    std::string ip;
    unsigned int port;
    std::string in_filename;
    std::string out_filename;
  };

  void bootstrap_failover(const std::vector<Config> &servers,
      const std::vector<std::string> &router_options = {},
      int expected_exitcode = 0,
      const std::vector<std::string> &expected_output_regex = {},
      unsigned wait_for_exit_timeout_ms = 10000);

  friend std::ostream &operator<<(std::ostream & os, const std::vector<std::tuple<RouterComponentTest::CommandHandle, unsigned int>> &T);
};

std::ostream &operator<<(std::ostream & os, const std::vector<std::tuple<RouterBootstrapTest::CommandHandle, unsigned int>> &T) {
  for (auto &t: T) {
    auto &proc = std::get<0>(t);

    os << "member@" << std::to_string(std::get<1>(t)) << ": "
      << proc.get_current_output()
      << std::endl;
  }
  return os;
}

/**
 * the tiny power function that does all the work.
 *
 * - build environment
 * - start mock servers based on Config[]
 * - pass router_options to the launched router
 * - check the router exits as expected
 * - check output of router contains the expected lines
 */
void
RouterBootstrapTest::bootstrap_failover(
    const std::vector<Config> &mock_server_configs,
    const std::vector<std::string> &router_options,
    int expected_exitcode,
    const std::vector<std::string> &expected_output_regex,
    unsigned wait_for_exit_timeout_ms) {
  std::string cluster_name("mycluster");

  // build environment
  std::map<std::string, std::string> env_vars = {
    { "MYSQL_SERVER_MOCK_CLUSTER_NAME", cluster_name },
    { "MYSQL_SERVER_MOCK_HOST_NAME", my_hostname },
  };

  unsigned int ndx = 1;

  for (const auto &mock_server_config: mock_server_configs) {
    env_vars.emplace("MYSQL_SERVER_MOCK_HOST_" + std::to_string(ndx), mock_server_config.ip);
    env_vars.emplace("MYSQL_SERVER_MOCK_PORT_" + std::to_string(ndx), std::to_string(mock_server_config.port));
    ndx++;
  };

  std::vector<std::tuple<CommandHandle, unsigned int>> mock_servers;

  // start the mocks
  for (const auto &mock_server_config: mock_server_configs) {
    unsigned int port = mock_server_config.port;
    const std::string &in_filename = mock_server_config.in_filename;
    const std::string &out_filename = mock_server_config.out_filename;

    if (in_filename.size()) {
      rewrite_js_to_tracefile(in_filename, out_filename, env_vars);
    }

    if (out_filename.size()) {
      mock_servers.emplace_back(
            launch_mysql_server_mock(out_filename, port, false),
            port);
    }
  }

  // wait for all mocks to be up
  for (auto &mock_server: mock_servers) {
    auto &proc = std::get<0>(mock_server);
    unsigned int port = std::get<1>(mock_server);

    bool ready = wait_for_port_ready(port, 1000);
    EXPECT_TRUE(ready) << proc.get_full_output();
  }

  std::string router_cmdline;

  if (router_options.size()) {
    for (const auto &piece: router_options) {
      router_cmdline += piece;
      router_cmdline += " ";
    }
  } else {
    router_cmdline = "--bootstrap=" + env_vars.at("MYSQL_SERVER_MOCK_HOST_1") + ":" + env_vars.at("MYSQL_SERVER_MOCK_PORT_1")
      + " -d " + bootstrap_dir;
  }

  // launch the router
  auto router = launch_router(router_cmdline);

  // type in the password
  router.register_response("Please enter MySQL password for root: ", "fake-pass\n");

  // wait_for_exit() throws at timeout.
  EXPECT_NO_THROW(EXPECT_EQ(router.wait_for_exit(wait_for_exit_timeout_ms), expected_exitcode));

  // split the output into lines
  std::vector<std::string> lines;
  {
    std::istringstream ss { router.get_full_output() };

    for (std::string line; std::getline(ss, line); ) {
      lines.emplace_back(line);
    }
  }

  for (auto const &re_str: expected_output_regex) {
    EXPECT_THAT(lines, ::testing::Contains(::testing::ContainsRegex(re_str)))
      << "router:" << router.get_full_output() << std::endl
      << mock_servers;
  }

  if (0 == expected_exitcode) {
    // fetch all the content for debugging
    for (auto &mock_server: mock_servers) {
      std::get<0>(mock_server).get_full_output();
    }
    EXPECT_THAT(lines, ::testing::Contains("MySQL Router  has now been configured for the InnoDB cluster '" + cluster_name + "'."))
      << "router:" << router.get_full_output() << std::endl
      << mock_servers;
  }
}

/**
 * @test
 *       verify that the router's \c --bootstrap can bootstrap
 *       from metadata-servers's PRIMARY over TCP/IP
 * @test
 *       Group Replication roles:
 *       - PRIMARY
 *       - SECONDARY (not used)
 *       - SECONDARY (not used)
 */
TEST_F(RouterBootstrapTest, BootstrapOk) {
  std::vector<Config> config {
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      get_data_dir().join("bootstrap.json").str()
    },
  };

  bootstrap_failover(config);
}

/**
 * @test
 *       verify that the router's \c --bootstrap can bootstrap
 *       from metadata-server's PRIMARY over TCP/IP and generate
 *       a configuration with unix-sockets only
 * @test
 *       Group Replication roles:
 *       - PRIMARY
 *       - SECONDARY (not used)
 *       - SECONDARY (not used)
 */
TEST_F(RouterBootstrapTest, BootstrapOnlySockets) {
  std::vector<Config> mock_servers {
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      get_data_dir().join("bootstrap.json").str()
    },
  };

  std::vector<std::string> router_options = {
    "--bootstrap=" + mock_servers.at(0).ip + ":" + std::to_string(mock_servers.at(0).port),
    "-d", bootstrap_dir,
    "--conf-skip-tcp",
    "--conf-use-sockets"
  };

  bootstrap_failover(mock_servers, router_options,
#ifndef _WIN32
      0,
      {
        "- Read/Write Connections: .*/mysqlx.sock",
        "- Read/Only Connections: .*/mysqlxro.sock"
      }
#else
      1,
      {
        "Error: unknown option '--conf-skip-tcp'"
      }
#endif
  );
}

/**
 * @test
 *       verify that the router's \c --bootstrap detects a unsupported
 *       metadata schema version
 * @test
 *       Group Replication roles:
 *       - PRIMARY
 *       - SECONDARY (not used)
 *       - SECONDARY (not used)
 */
TEST_F(RouterBootstrapTest, BootstrapUnsupportedSchemaVersion) {
  std::vector<Config> mock_servers {
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      get_data_dir().join("bootstrap_unsupported_schema_version.json").str()
    },
  };

  // check that it failed as expected
  bootstrap_failover(mock_servers, {},
      1,
      {
        "^Error: This version of MySQL Router is not compatible with the provided MySQL InnoDB cluster metadata"
      }
  );
}

/**
 * @test
 *       verify that bootstrap will fail-over to another node if the initial
 *       node is not writable
 * @test
 *       Group Replication roles:
 *       - SECONDARY
 *       - PRIMARY
 *       - SECONDARY (not used)
 */
TEST_F(RouterBootstrapTest, BootstrapFailoverSuperReadonly) {
  std::vector<Config> config {
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_super_read_only_1.js").str(),
      Path(tmp_dir).join("bootstrap_failover_super_read_only_1.json").str()
    },
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      get_data_dir().join("bootstrap_failover_super_read_only_2.json").str()
    },
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      ""
    },
  };

  bootstrap_failover(config);
}

/**
 * @test
 *       verify that bootstrap will fail-over to another node if the initial
 *       node is not writable and 2nd candidate has connection problems
 * @test
 *       Group Replication roles:
 *       - SECONDARY
 *       - <connect-failure>
 *       - PRIMARY
 * @test
 *       connection problems could be anything from 'auth-failure' to 'network-errors'.
 *       This test uses a \c port==0 to create a failure which is reserved and unassigned.
 *
 * @note The implementation uses \c port=65536 to circumvents libmysqlclients \code{.py}
 *       if port == 0:
 *         port = 3306 \endcode default port assignment.
 *       As the port will later be narrowed to an 16bit unsigned integer \code port & 0xffff \endcode
 *       the code will connect to port 0 in the end.
 *
 * @todo As soon as the mysql-server-mock supports authentication failures
 *       the code can take that into account too.
 */
TEST_F(RouterBootstrapTest, BootstrapFailoverSuperReadonly2ndNodeDead) {
  std::vector<Config> config {
    // member-1, PRIMARY, fails at first write
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_super_read_only_1.js").str(),
      Path(tmp_dir).join("member-1.json").str()
    },
    // member-2, unreachable
    {
      "127.0.0.1", 65536, // 65536 % 0xffff = 0 (port 0), but we bypass libmysqlclient's default-port assignment
      "",
      ""
    },
    // member-3, succeeds
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      get_data_dir().join("bootstrap_failover_super_read_only_2.json").str()
    },
  };

  bootstrap_failover(config, {}, 0, {
      "^Fetching Group Replication Members",
      "^Failed connecting to 127\\.0\\.0\\.1:65536: .*, trying next$",
  });
}

/**
 * @test
 *       verify that bootstrap fails over and continues if create-account.drop-user fails
 * @test
 *       Group Replication roles:
 *       - PRIMARY-failing
 *       - PRIMARY-online
 *       - SECONDARY (not used)
 */
TEST_F(RouterBootstrapTest, BootstrapFailoverSuperReadonlyCreateAccountDropUserFails) {
  std::vector<Config> config {
    // member-1: PRIMARY, fails at DROP USER
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_super_read_only_dead_2nd_1.js").str(),
      Path(tmp_dir).join("member-1.json").str()
    },

    // member-2: PRIMARY, succeeds
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_reconfigure_ok.js").str(),
      Path(tmp_dir).join("member-2.json").str()
    },

    // member-3: defined, but unused
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      ""
    },
  };

  bootstrap_failover(config);
}

/**
 * @test
 *       verify that bootstrap fails over and continues if create-account.grant fails
 * @test
 *       Group Replication roles:
 *       - SECONDARY
 *       - PRIMARY
 *       - SECONDARY (not used)
 *
 */
TEST_F(RouterBootstrapTest, BootstrapFailoverSuperReadonlyCreateAccountGrantFails) {
  std::vector<Config> config {
    // member-1: PRIMARY, fails after GRANT
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_at_grant.js").str(),
      Path(tmp_dir).join("member-1.json").str()
    },

    // member-2: PRIMARY, succeeds
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_reconfigure_ok.js").str(),
      Path(tmp_dir).join("member-2.json").str()
    },

    // member-3: defined, but unused
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      ""
    },
  };

  bootstrap_failover(config);
}

/**
 * @test
 *       verify that bootstraping via a unix-socket fails over to the IP-addresses
 *       of the members
 * @test
 *       Group Replication roles:
 *       - SECONDARY
 *       - PRIMARY
 *       - SECONDARY (not used)
 * @test
 *       Initial connect via unix-socket to the 1st node, all further connects via TCP/IP
 *
 * @todo needs unix-socket support in the mock-server
 */
TEST_F(RouterBootstrapTest, DISABLED_BootstrapFailoverSuperReadonlyFromSocket) {
  std::vector<Config> mock_servers {
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_super_read_only_1.js").str(),
      Path(tmp_dir).join("bootstrap_failover_super_read_only_1.json").str()
    },
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      get_data_dir().join("bootstrap_failover_super_read_only_2.json").str()
    },
    {
      "127.0.0.1", port_pool_.get_next_available(),
      "",
      ""
    },
  };

  std::vector<std::string> router_options = {
    "--bootstrap=localhost",
    "--bootstrap-socket=" + mock_servers.at(0).ip,
    "-d", bootstrap_dir
  };

  bootstrap_failover(mock_servers, router_options);
}

/**
 * @test
 *       verify that bootstrap fails over if PRIMARY crashes while bootstrapping
 *
 * @test
 *       Group Replication roles:
 *       - SECONDARY
 *       - PRIMARY (crashing)
 *       - PRIMARY
 */
TEST_F(RouterBootstrapTest, BootstrapFailoverSuperReadonlyNewPrimaryCrash) {
  std::vector<Config> mock_servers {
    // member-1: PRIMARY, fails at DROP USER
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_super_read_only_dead_2nd_1.js").str(),
      Path(tmp_dir).join("member-1.json").str()
    },

    // member-2: PRIMARY, but crashing
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_at_crash.js").str(),
      Path(tmp_dir).join("member-2.json").str()
    },

    // member-3: newly elected PRIMARY, succeeds
    {
      "127.0.0.1", port_pool_.get_next_available(),
      get_data_dir().join("bootstrap_failover_reconfigure_ok.js").str(),
      Path(tmp_dir).join("member-3.json").str()
    },
  };

  bootstrap_failover(mock_servers);
}


int main(int argc, char *argv[]) {
  init_windows_sockets();
  g_origin_path = Path(argv[0]).dirname();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
