#include <gtest/gtest.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../src/tcp/socket.h"

TEST(SocketTest, DefaultConstructorCreatesValidSocket)
{
    Socket sock;
    EXPECT_TRUE(sock.is_valid());
    EXPECT_GE(sock.fd(), 0);
}

TEST(SocketTest, WrapperConstructorWithCustomFd)
{
    int raw_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_GE(raw_fd, 0);

    {
        Socket sock(raw_fd);
        EXPECT_TRUE(sock.is_valid());
        EXPECT_EQ(sock.fd(), raw_fd);
    } // sock out of scope so raw_fd should close

    char buf[1];
    EXPECT_EQ(::read(raw_fd, buf, 1), -1);
    EXPECT_EQ(errno, EBADF);
}
