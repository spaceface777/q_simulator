#include <arpa/inet.h>
#include <chrono>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../../src/message/message.h"
#include "../../src/module/types.h"
#include "../../src/tcp/server.h"

class ServerTest : public ::testing::Test
{
  protected:
    void SetUp() override { server::stop(); }

    void TearDown() override { server::stop(); }

    int create_client_socket()
    {
        int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd < 0)
            return -1;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server::PORT);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            ::close(client_fd);
            return -1;
        }
        return client_fd;
    }

    // initialize server with a single active client
    int initialize_single_client()
    {
        server::run();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        int client_fd = create_client_socket();
        EXPECT_GE(client_fd, 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(server::size(), 1u);

        const std::string init = message::Init(module::ModuleId{1}).to_toml();
        EXPECT_GE(::write(client_fd, init.c_str(), init.length() + 1), 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        return client_fd;
    }
};

TEST_F(ServerTest, InitialStateIsEmpty)
{
    EXPECT_EQ(server::size(), 0);
}

TEST_F(ServerTest, StartAndStopServer)
{
    EXPECT_NO_THROW(server::run());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_NO_THROW(server::stop());
}

TEST_F(ServerTest, AcceptsClientConnection)
{
    server::run();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client_fd = create_client_socket();
    ASSERT_GE(client_fd, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GE(server::size(), 1u);

    ::close(client_fd);
}

TEST_F(ServerTest, HandlesMultipleConnections)
{
    server::run();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client1 = create_client_socket();
    int client2 = create_client_socket();

    ASSERT_GE(client1, 0);
    ASSERT_GE(client2, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(server::size(), 2u);

    ::close(client1);
    ::close(client2);
}

TEST_F(ServerTest, RemovesDisconnectedClient)
{
    server::run();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client_fd = create_client_socket();
    ASSERT_GE(client_fd, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(server::size(), 1u);

    // close connection from client side
    ::close(client_fd);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(server::size(), 0u);
}

TEST_F(ServerTest, HandlesInit)
{
    int client_fd = initialize_single_client();
    EXPECT_GE(client_fd, 0);

    const auto fds = server::get_module_fds();
    EXPECT_FALSE(fds.empty());
    EXPECT_NE(fds.end(), fds.find(1));

    ::close(client_fd);
}

TEST_F(ServerTest, HandlesPacket)
{
    int client_fd = initialize_single_client();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::string packet = message::Packet(module::ModuleId{1}, "hey").to_toml();
    EXPECT_GE(::write(client_fd, packet.c_str(), packet.length() + 1), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    char buff[MAX_TCP_BYTES_SIZE] = {0};
    EXPECT_GE(::recv(client_fd, buff, MAX_TCP_BYTES_SIZE, MSG_DONTWAIT), 0);

    EXPECT_EQ(packet, std::string(buff));

    ::close(client_fd);
}