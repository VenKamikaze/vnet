// SPDX-License-Identifier: MIT
#include "test_util.h"

#include <random>

TEST(Addr, Ipv4RoundTrip) {
    vnet_addr a = {};
    ASSERT_EQ(vnet_resolve(nullptr, "1.2.3.4:5000", &a), 0);
    EXPECT_EQ(a.family, VNET_AF_IPV4);
    EXPECT_EQ(a.port, 5000);
    EXPECT_EQ(a.bytes[0], 1);
    EXPECT_EQ(a.bytes[3], 4);
    EXPECT_EQ(Str(a), "1.2.3.4:5000");
}

TEST(Addr, Ipv4Extremes) {
    EXPECT_EQ(Str(A("0.0.0.0:0")), "0.0.0.0:0");
    EXPECT_EQ(Str(A("255.255.255.255:65535")), "255.255.255.255:65535");
}

TEST(Addr, Ipv6RoundTripAndCompression) {
    struct Case { const char *in, *out; } cases[] = {
        {"[::1]:5000", "[::1]:5000"},
        {"[::]:1", "[::]:1"},
        {"[1::]:1", "[1::]:1"},
        {"[2001:db8::1]:80", "[2001:db8::1]:80"},
        {"[2001:0db8:0000:0000:0000:0000:0000:0001]:80", "[2001:db8::1]:80"},
        {"[2001:DB8:0:0:1:0:0:1]:80", "[2001:db8::1:0:0:1]:80"},   // longest run wins, first on tie
        {"[1:0:0:2:0:0:0:3]:7", "[1:0:0:2::3]:7"},
        {"[1:2:3:4:5:6:7:8]:9", "[1:2:3:4:5:6:7:8]:9"},
        {"[1:0:3:4:5:6:7:8]:9", "[1:0:3:4:5:6:7:8]:9"},           // single zero group is not compressed
        {"[fe80::1:2]:443", "[fe80::1:2]:443"},
        {"[64:ff9b::1.2.3.4]:5", "[64:ff9b::102:304]:5"},         // embedded IPv4 tail
    };
    for (auto &c : cases) {
        vnet_addr a = {};
        ASSERT_EQ(vnet_resolve(nullptr, c.in, &a), 0) << c.in;
        EXPECT_EQ(a.family, VNET_AF_IPV6) << c.in;
        EXPECT_EQ(Str(a), c.out) << c.in;
    }
}

TEST(Addr, Ipv6BytesAreNetworkOrder) {
    vnet_addr a = A("[2001:db8::ff]:1");
    EXPECT_EQ(a.bytes[0], 0x20);
    EXPECT_EQ(a.bytes[1], 0x01);
    EXPECT_EQ(a.bytes[2], 0x0d);
    EXPECT_EQ(a.bytes[3], 0xb8);
    EXPECT_EQ(a.bytes[15], 0xff);
    EXPECT_EQ(a.bytes[14], 0x00);
}

TEST(Addr, LocalhostMapsToLoopbackAddress) {
    EXPECT_EQ(Str(A("localhost:9")), "127.0.0.1:9");
}

TEST(Addr, Ipv4MappedIpv6IsCanonicalisedToIpv4) {
    vnet_addr a = A("[::ffff:1.2.3.4]:77");
    EXPECT_EQ(a.family, VNET_AF_IPV4);
    vnet_addr plain = A("1.2.3.4:77");
    EXPECT_TRUE(vnet_addr_equal(&a, &plain));
    EXPECT_EQ(Str(a), "1.2.3.4:77");
    EXPECT_EQ(Str(A("[::ffff:102:304]:77")), "1.2.3.4:77");
}

TEST(Addr, RejectsMalformed) {
    const char *bad[] = {
        "", "1.2.3.4", "1.2.3.4:", ":5000", "1.2.3:5000", "1.2.3.4.5:5", "256.1.1.1:5",
        "01.2.3.4:5", "1.2.3.4:65536", "1.2.3.4:-1", "1.2.3.4:+1", "1.2.3.4:5x", " 1.2.3.4:5",
        "1.2.3.4 :5", "1.2.3.4:123456", "example.com:5", "::1:5000", "[::1]", "[::1]:",
        "[::1]5000", "[::1:5000", "::1]:5", "[1:2:3:4:5:6:7:8:9]:1", "[1:2:3:4:5:6:7]:1",
        "[1::2::3]:1", "[:1]:5", "[1:]:5", "[12345::]:5", "[g::1]:5", "[::1.2.3]:5",
        "[1:2:3:4:5:6:7::8]:5", "1.2.3.4:5:6", "[1.2.3.4]:5", "[]:5",
    };
    for (const char *s : bad) {
        vnet_addr a = {};
        a.port = 99;
        EXPECT_EQ(vnet_resolve(nullptr, s, &a), VNET_EINVAL) << "'" << s << "'";
    }
    vnet_addr a;
    EXPECT_EQ(vnet_resolve(nullptr, nullptr, &a), VNET_EINVAL);
    EXPECT_EQ(vnet_resolve(nullptr, "1.2.3.4:5", nullptr), VNET_EINVAL);
}

TEST(Addr, ToStringBufferHandling) {
    vnet_addr a = A("123.45.67.89:12345");  // 18 chars
    char buf[32];
    EXPECT_EQ(vnet_addr_to_string(&a, buf, sizeof buf), 18);
    EXPECT_STREQ(buf, "123.45.67.89:12345");
    char exact[19];
    EXPECT_EQ(vnet_addr_to_string(&a, exact, sizeof exact), 18);
    char tight[18];
    std::memset(tight, 'x', sizeof tight);
    EXPECT_EQ(vnet_addr_to_string(&a, tight, sizeof tight), VNET_ENOBUFS);
    EXPECT_EQ(tight[sizeof tight - 1], '\0');  // still a valid string
    EXPECT_EQ(vnet_addr_to_string(&a, nullptr, 0), VNET_ENOBUFS);
    vnet_addr none = {};
    EXPECT_EQ(vnet_addr_to_string(&none, buf, sizeof buf), VNET_EINVAL);
    EXPECT_EQ(vnet_addr_to_string(nullptr, buf, sizeof buf), VNET_EINVAL);
}

TEST(Addr, EqualityComparesFamilyBytesAndPort) {
    vnet_addr a = A("1.2.3.4:5"), b = A("1.2.3.4:5");
    EXPECT_TRUE(vnet_addr_equal(&a, &b));
    b = A("1.2.3.4:6");
    EXPECT_FALSE(vnet_addr_equal(&a, &b));
    b = A("1.2.3.5:5");
    EXPECT_FALSE(vnet_addr_equal(&a, &b));
    vnet_addr c = A("[::1]:5"), d = A("[::2]:5");
    EXPECT_FALSE(vnet_addr_equal(&c, &d));
    EXPECT_FALSE(vnet_addr_equal(&a, &c));
    EXPECT_TRUE(vnet_addr_equal(&c, &c));
    EXPECT_FALSE(vnet_addr_equal(&a, nullptr));
    // Garbage in the unused tail of an IPv4 address must not matter.
    b = A("1.2.3.4:5");
    b.bytes[10] = 0x77;
    EXPECT_TRUE(vnet_addr_equal(&a, &b));
}

// Property test: every address formats to a string that resolves back to the
// same address, including awkward zero patterns.
TEST(Addr, RandomRoundTrip) {
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 5000; i++) {
        vnet_addr a = {};
        if (rng() % 3 == 0) {
            a.family = VNET_AF_IPV4;
            for (int k = 0; k < 4; k++) a.bytes[k] = (uint8_t)(rng() % 4 == 0 ? 0 : rng());
        } else {
            a.family = VNET_AF_IPV6;
            for (int g = 0; g < 8; g++) {
                bool zero = rng() % 2;
                uint16_t v = zero ? 0 : (uint16_t)rng();
                a.bytes[2 * g] = (uint8_t)(v >> 8);
                a.bytes[2 * g + 1] = (uint8_t)v;
            }
            // Avoid the mapped range, which canonicalises by design.
            if (a.bytes[10] == 0xff && a.bytes[11] == 0xff) a.bytes[0] = 1;
        }
        a.port = (uint16_t)rng();
        char buf[96];
        ASSERT_GT(vnet_addr_to_string(&a, buf, sizeof buf), 0);
        vnet_addr b = {};
        ASSERT_EQ(vnet_resolve(nullptr, buf, &b), 0) << buf;
        ASSERT_TRUE(vnet_addr_equal(&a, &b)) << buf;
    }
}

TEST(Addr, StrerrorCoversEveryCodeAndNeverReturnsNull) {
    const int codes[] = {VNET_OK, VNET_EAGAIN, VNET_EINVAL, VNET_EMSGSIZE, VNET_EBADF,
                         VNET_EADDRINUSE, VNET_ENOMEM, VNET_ENOSOCK, VNET_EAFNOSUPPORT,
                         VNET_EIO, VNET_ENOTSUP, VNET_ENOBUFS};
    std::vector<std::string> seen;
    for (int c : codes) {
        const char *s = vnet_strerror(c);
        ASSERT_NE(s, nullptr);
        EXPECT_STRNE(s, "unknown error") << c;
        for (auto &p : seen) EXPECT_NE(p, s);
        seen.push_back(s);
    }
    EXPECT_STREQ(vnet_strerror(-12345), "unknown error");
}
