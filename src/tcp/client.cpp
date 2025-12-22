#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <array>
#include <atomic>

#include "../packet/packet.h"
#include "socket.h"
#include "client.h"

#define MAX_TCP_BYTES_SIZE 512

namespace client
{
    std::atomic_bool stopped = false;

    void stop()
    {
        stopped = true;
    }

    void setup_connection(int fd)
    {
        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = htons(7777),
            .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};

        // connect to peer
        if (connect(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) != 0)
        {
            throw std::runtime_error("Failed to connect to peer");
        }
    }

    void run(unsigned long port)
    {
        std::cerr << "Client init" << std::endl;

        Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
        if (!socket.is_valid())
        {
            throw std::runtime_error("Failed to bind socket");
        }

        setup_connection(socket.fd());

        packet::Update update(1, "2", 3);
        const std::string toml = update.to_toml();
        if (write(socket.fd(), toml.c_str(), toml.size()) <= 0)
        {
            throw std::runtime_error("Failed to write to socket");
        }

        std::cerr << "Sent GET" << std::endl;

        std::array<char, MAX_TCP_BYTES_SIZE> buff;
        const ssize_t confirm = read(socket.fd(), buff.data(), buff.size());
        if (confirm <= 0)
        {
            throw std::runtime_error("Failed to get confirmation message");
        }

        std::cerr << "Read ";
        for (const char c : buff)
        {
            std::cerr << c;
        }
        std::cerr << std::endl;

        // while (true)
        // {
        // }
    }

}