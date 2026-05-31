/*
 * vnetif - Virtual Network Interface Driver
 * Integration test suite.
 *
 * Tests interact with the live kernel module via procfs and standard
 * networking tools.  The binary must be run as root from the project root
 * so that build/vnetif.ko is reachable.
 *
 * Procfs layout under test:
 *   /proc/vnetif/status   -- list interfaces, MTU, and IPv4 addresses
 *   /proc/vnetif/create   -- create a new interface
 *   /proc/vnetif/destroy  -- destroy an interface by name
 *   /proc/vnetif/setaddr  -- assign an IPv4 address: "IFNAME IP"
 *   /proc/vnetif/setmtu   -- change MTU: "IFNAME MTU"
 *
 * Copyright (C) 2026 Ridha MASTOURI <mastouri.rida@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Shell helpers
// ---------------------------------------------------------------------------

static int shell_cmd(const std::string &cmd)
{
    return ::system(cmd.c_str());
}

static std::string capture(const std::string &cmd)
{
    FILE *pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        return {};
    char buf[256];
    std::string out;
    while (::fgets(buf, sizeof(buf), pipe))
        out += buf;
    ::pclose(pipe);
    return out;
}

// Write directly to a procfs entry; returns write(2) return value or -errno.
static ssize_t proc_write(const char *path, const std::string &data)
{
    int fd = ::open(path, O_WRONLY);
    if (fd < 0)
        return -errno;
    ssize_t n = ::write(fd, data.data(), data.size());
    int saved_errno = errno;
    ::close(fd);
    return (n < 0) ? -saved_errno : n;
}

// ---------------------------------------------------------------------------
// Assertion helpers
// ---------------------------------------------------------------------------

static bool iface_exists(const std::string &name)
{
    return shell_cmd("ip link show dev " + name + " >/dev/null 2>&1") == 0;
}

static bool proc_status_lists(const std::string &name)
{
    return capture("cat /proc/vnetif/status").find(name) != std::string::npos;
}

static bool iface_has_ip(const std::string &iface, const std::string &ip_cidr)
{
    return capture("ip -4 addr show dev " + iface).find(ip_cidr)
           != std::string::npos;
}

static bool proc_status_has_ip(const std::string &ip)
{
    return capture("cat /proc/vnetif/status").find(ip) != std::string::npos;
}

static int iface_get_mtu(const std::string &iface)
{
    std::string out = capture("ip link show dev " + iface +
                               " | awk '/mtu/{for(i=1;i<=NF;i++) if($i==\"mtu\") print $(i+1)}'");
    if (out.empty())
        return -1;
    return std::stoi(out);
}

static std::string iface_get_mac(const std::string &iface)
{
    std::string out = capture("ip link show dev " + iface +
                               " | awk '/link\\/ether/{print $2}'");
    out.erase(out.find_last_not_of(" \n\r\t") + 1);
    return out;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr const char *MOD_NAME   = "vnetif";
static constexpr const char *MOD_PATH   = "build/vnetif.ko";
static constexpr const char *IF0        = "vnet0";
static constexpr const char *IF1        = "vnet1";
static constexpr const char *IF2        = "vnet2";
static constexpr const char *IP0        = "10.10.10.1";
static constexpr const char *IP1        = "10.10.20.1";
static constexpr const char *IP2        = "10.10.30.1";
static constexpr const char *BR_NAME    = "br-vtest";
static constexpr const char *VETH_HOST  = "veth-host";
static constexpr const char *VETH_NS    = "veth-ns";
static constexpr const char *NS_NAME    = "vnetif-ns1";
static constexpr const char *NS_IP_CIDR = "10.10.10.2/24";

static constexpr const char *PROC_STATUS  = "/proc/vnetif/status";
static constexpr const char *PROC_CREATE  = "/proc/vnetif/create";
static constexpr const char *PROC_DESTROY = "/proc/vnetif/destroy";
static constexpr const char *PROC_SETADDR = "/proc/vnetif/setaddr";
static constexpr const char *PROC_SETMTU  = "/proc/vnetif/setmtu";

// ---------------------------------------------------------------------------
// Full cleanup (idempotent, errors suppressed)
// ---------------------------------------------------------------------------

static void full_cleanup()
{
    shell_cmd(std::string("ip netns del ")  + NS_NAME   + " 2>/dev/null");
    shell_cmd(std::string("ip link set ")   + IF0       + " nomaster 2>/dev/null");
    shell_cmd(std::string("ip link del ")   + VETH_HOST + " 2>/dev/null");
    shell_cmd(std::string("ip link del ")   + BR_NAME   + " 2>/dev/null");
    shell_cmd(std::string("rmmod ")         + MOD_NAME  + " 2>/dev/null");
}

// ---------------------------------------------------------------------------
// Global environment: clean slate before and after the suite
// ---------------------------------------------------------------------------

class CleanupEnvironment : public ::testing::Environment {
public:
    void SetUp()    override { full_cleanup(); }
    void TearDown() override { full_cleanup(); }
};

// ---------------------------------------------------------------------------
// Module / interface helpers
// ---------------------------------------------------------------------------

static void load_module()
{
    shell_cmd(std::string("rmmod ") + MOD_NAME + " 2>/dev/null");
    ASSERT_EQ(0, shell_cmd(std::string("insmod ") + MOD_PATH))
        << "insmod failed -- run tests from project root as root";
    shell_cmd(std::string("nmcli device set ") + IF0 + " managed no 2>/dev/null");
}

static void create_iface(const std::string &expected_name)
{
    ASSERT_GT(proc_write(PROC_CREATE, "1"), 0)
        << "write to " << PROC_CREATE << " failed";
    shell_cmd("nmcli device set " + expected_name + " managed no 2>/dev/null");
}

static void destroy_iface(const std::string &name)
{
    ASSERT_GT(proc_write(PROC_DESTROY, name), 0)
        << "write to " << PROC_DESTROY << " failed for " << name;
}

static void set_addr(const std::string &iface, const std::string &ip)
{
    ASSERT_GT(proc_write(PROC_SETADDR, iface + " " + ip), 0)
        << "write to " << PROC_SETADDR << " failed for " << iface;
    ::usleep(300000);
}

static void set_mtu(const std::string &iface, int mtu)
{
    ASSERT_GT(proc_write(PROC_SETMTU, iface + " " + std::to_string(mtu)), 0)
        << "write to " << PROC_SETMTU << " failed for " << iface;
}

// ---------------------------------------------------------------------------
// Interface management tests
// ---------------------------------------------------------------------------

class IfaceTest : public ::testing::Test {
protected:
    void SetUp()    override { full_cleanup(); load_module(); }
    void TearDown() override { full_cleanup(); }
};

TEST_F(IfaceTest, DefaultInterfaceCreatedOnLoad)
{
    EXPECT_TRUE(iface_exists(IF0));
    EXPECT_TRUE(proc_status_lists(IF0));
}

TEST_F(IfaceTest, ProcStatusReadable)
{
    EXPECT_EQ(0, shell_cmd("test -r " + std::string(PROC_STATUS)));
}

TEST_F(IfaceTest, ProcCreateWritable)
{
    EXPECT_EQ(0, shell_cmd("test -w " + std::string(PROC_CREATE)));
}

TEST_F(IfaceTest, ProcDestroyWritable)
{
    EXPECT_EQ(0, shell_cmd("test -w " + std::string(PROC_DESTROY)));
}

TEST_F(IfaceTest, ProcSetaddrWritable)
{
    EXPECT_EQ(0, shell_cmd("test -w " + std::string(PROC_SETADDR)));
}

TEST_F(IfaceTest, ProcSetmtuWritable)
{
    EXPECT_EQ(0, shell_cmd("test -w " + std::string(PROC_SETMTU)));
}

TEST_F(IfaceTest, CreateInterface)
{
    create_iface(IF1);
    EXPECT_TRUE(iface_exists(IF1));
    EXPECT_TRUE(proc_status_lists(IF1));
}

TEST_F(IfaceTest, CreateMultipleInterfaces)
{
    create_iface(IF1);
    create_iface(IF2);
    EXPECT_TRUE(iface_exists(IF1));
    EXPECT_TRUE(iface_exists(IF2));
}

TEST_F(IfaceTest, DestroyInterface)
{
    create_iface(IF1);
    destroy_iface(IF1);
    EXPECT_FALSE(iface_exists(IF1));
    EXPECT_FALSE(proc_status_lists(IF1));
}

TEST_F(IfaceTest, DestroyOnePreservesOthers)
{
    create_iface(IF1);
    create_iface(IF2);
    destroy_iface(IF1);

    EXPECT_TRUE(iface_exists(IF0));
    EXPECT_FALSE(iface_exists(IF1));
    EXPECT_TRUE(iface_exists(IF2));
}

TEST_F(IfaceTest, DestroyNonexistentReturnsError)
{
    EXPECT_LT(proc_write(PROC_DESTROY, "vnet99"), 0);
}

TEST_F(IfaceTest, DestroyBlankNameReturnsError)
{
    EXPECT_LT(proc_write(PROC_DESTROY, "   "), 0);
}

TEST_F(IfaceTest, SetaddrMissingIPReturnsError)
{
    EXPECT_LT(proc_write(PROC_SETADDR, "vnet0"), 0);
}

TEST_F(IfaceTest, SetaddrInvalidIPReturnsError)
{
    EXPECT_LT(proc_write(PROC_SETADDR, "vnet0 999.999.999.999"), 0);
}

TEST_F(IfaceTest, SetaddrNonexistentIfaceReturnsError)
{
    EXPECT_LT(proc_write(PROC_SETADDR, "vnet99 10.0.0.1"), 0);
}

TEST_F(IfaceTest, SetIPOnInterface)
{
    set_addr(IF0, IP0);
    EXPECT_TRUE(iface_has_ip(IF0, std::string(IP0) + "/24"));
}

TEST_F(IfaceTest, SetIPAppearsInProcStatus)
{
    set_addr(IF0, IP0);
    EXPECT_TRUE(proc_status_has_ip(IP0));
}

TEST_F(IfaceTest, SetIPOnMultipleInterfaces)
{
    create_iface(IF1);
    create_iface(IF2);
    set_addr(IF0, IP0);
    set_addr(IF1, IP1);
    set_addr(IF2, IP2);

    EXPECT_TRUE(iface_has_ip(IF0, std::string(IP0) + "/24"));
    EXPECT_TRUE(iface_has_ip(IF1, std::string(IP1) + "/24"));
    EXPECT_TRUE(iface_has_ip(IF2, std::string(IP2) + "/24"));
}

TEST_F(IfaceTest, DestroyPreservesIPsOnSurvivors)
{
    create_iface(IF1);
    set_addr(IF0, IP0);
    set_addr(IF1, IP1);
    destroy_iface(IF1);
    EXPECT_TRUE(iface_has_ip(IF0, std::string(IP0) + "/24"));
}

// ---------------------------------------------------------------------------
// MTU tests
// ---------------------------------------------------------------------------

class MTUTest : public ::testing::Test {
protected:
    void SetUp()    override { full_cleanup(); load_module(); }
    void TearDown() override { full_cleanup(); }
};

TEST_F(MTUTest, DefaultMTUIs1500)
{
    EXPECT_EQ(1500, iface_get_mtu(IF0));
}

TEST_F(MTUTest, SetMTUJumboFrame)
{
    set_mtu(IF0, 9000);
    EXPECT_EQ(9000, iface_get_mtu(IF0));
}

TEST_F(MTUTest, SetMTUMinimum)
{
    set_mtu(IF0, 68); // ETH_MIN_MTU
    EXPECT_EQ(68, iface_get_mtu(IF0));
}

TEST_F(MTUTest, SetMTUTooSmallReturnsError)
{
    EXPECT_LT(proc_write(PROC_SETMTU, std::string(IF0) + " 0"), 0);
}

TEST_F(MTUTest, SetMTUNonexistentIfaceReturnsError)
{
    EXPECT_LT(proc_write(PROC_SETMTU, "vnet99 1500"), 0);
}

TEST_F(MTUTest, MTUAppearsInProcStatus)
{
    set_mtu(IF0, 4096);
    std::string status = capture("cat /proc/vnetif/status");
    EXPECT_NE(std::string::npos, status.find("4096")) << status;
}

// ---------------------------------------------------------------------------
// MAC address tests
// ---------------------------------------------------------------------------

class MacTest : public ::testing::Test {
protected:
    void SetUp()    override { full_cleanup(); load_module(); }
    void TearDown() override { full_cleanup(); }
};

TEST_F(MacTest, ChangeMACAddress)
{
    const std::string new_mac = "02:00:00:de:ad:01";
    shell_cmd(std::string("ip link set dev ") + IF0 + " address " + new_mac);
    EXPECT_EQ(new_mac, iface_get_mac(IF0));
}

TEST_F(MacTest, InvalidMACReturnsError)
{
    int rc = shell_cmd(std::string("ip link set dev ") + IF0 +
                       " address ff:ff:ff:ff:ff:ff 2>/dev/null");
    EXPECT_NE(0, rc) << "broadcast MAC should be rejected";
}

// ---------------------------------------------------------------------------
// Local ping tests
// ---------------------------------------------------------------------------

class LocalPingTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        full_cleanup();
        load_module();
        set_addr(IF0, IP0);
    }
    void TearDown() override { full_cleanup(); }
};

TEST_F(LocalPingTest, SelfPingSucceeds)
{
    int rc = shell_cmd(std::string("ping -I ") + IF0 +
                       " " + IP0 + " -c 3 -W 1 -q 2>/dev/null");
    EXPECT_EQ(0, rc);
}

TEST_F(LocalPingTest, SelfPingOnSecondInterface)
{
    create_iface(IF1);
    set_addr(IF1, IP1);
    int rc = shell_cmd(std::string("ping -I ") + IF1 +
                       " " + IP1 + " -c 3 -W 1 -q 2>/dev/null");
    EXPECT_EQ(0, rc);
}

TEST_F(LocalPingTest, PingWithoutAddressTimesOut)
{
    create_iface(IF1);
    int rc = shell_cmd(std::string("ping -I ") + IF1 +
                       " 10.10.20.1 -c 2 -W 1 -q 2>/dev/null");
    EXPECT_NE(0, rc);
}

// ---------------------------------------------------------------------------
// Remote ping tests via bridge + network namespace
// ---------------------------------------------------------------------------

class RemotePingTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        full_cleanup();
        load_module();
        set_addr(IF0, IP0);
        ASSERT_NO_FATAL_FAILURE(setup_topology());
    }
    void TearDown() override { full_cleanup(); }

private:
    void setup_topology()
    {
        ASSERT_EQ(0, shell_cmd(std::string("ip link add ") + BR_NAME + " type bridge"));
        ASSERT_EQ(0, shell_cmd(std::string("ip link set ") + BR_NAME +
                               " address 00:00:00:00:00:01"));
        ASSERT_EQ(0, shell_cmd(std::string("ip link set ") + BR_NAME + " up"));

        ASSERT_EQ(0, shell_cmd(std::string("ip link add ") + VETH_HOST +
                               " type veth peer name " + VETH_NS));
        ASSERT_EQ(0, shell_cmd(std::string("ip link set ") + VETH_HOST +
                               " master " + BR_NAME));
        ASSERT_EQ(0, shell_cmd(std::string("ip link set ") + VETH_HOST + " up"));

        ASSERT_EQ(0, shell_cmd(std::string("ip link set ") + IF0 +
                               " master " + BR_NAME));
        ASSERT_EQ(0, shell_cmd(std::string("ip link set ") + IF0 + " up"));

        std::string mac = capture(
            std::string("ip link show dev ") + IF0 +
            " | awk '/link\\/ether/{print $2}'");
        mac.erase(mac.find_last_not_of(" \n\r\t") + 1);
        shell_cmd("bridge fdb del " + mac + " dev " + std::string(IF0) +
                  " master 2>/dev/null");

        ASSERT_EQ(0, shell_cmd(std::string("ip netns add ") + NS_NAME));
        ASSERT_EQ(0, shell_cmd(std::string("ip link set ") + VETH_NS +
                               " netns " + NS_NAME));
        ASSERT_EQ(0, shell_cmd(std::string("ip netns exec ") + NS_NAME +
                               " ip link set lo up"));
        ASSERT_EQ(0, shell_cmd(std::string("ip netns exec ") + NS_NAME +
                               " ip link set " + VETH_NS + " up"));
        ASSERT_EQ(0, shell_cmd(std::string("ip netns exec ") + NS_NAME +
                               " ip addr add " + NS_IP_CIDR + " dev " + VETH_NS));
    }
};

TEST_F(RemotePingTest, ARPResolvesNeighbor)
{
    shell_cmd(std::string("ip netns exec ") + NS_NAME +
              " ip neigh flush all 2>/dev/null");
    shell_cmd(std::string("ip netns exec ") + NS_NAME +
              " ping " + IP0 + " -c 1 -W 2 -q 2>/dev/null");

    std::string neigh = capture(
        std::string("ip netns exec ") + NS_NAME +
        " ip neigh show " + IP0);
    EXPECT_NE(std::string::npos, neigh.find("lladdr")) << neigh;
}

TEST_F(RemotePingTest, PingFromNamespaceSucceeds)
{
    shell_cmd(std::string("ip netns exec ") + NS_NAME +
              " ip neigh flush all 2>/dev/null");
    int rc = shell_cmd(std::string("ip netns exec ") + NS_NAME +
                       " ping " + IP0 + " -c 3 -W 2 -q 2>/dev/null");
    EXPECT_EQ(0, rc);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new CleanupEnvironment);
    return RUN_ALL_TESTS();
}