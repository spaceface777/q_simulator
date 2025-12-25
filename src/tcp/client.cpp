#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstring>

#include "../packet/packet.h"
#include "../util.h"
#include "socket.h"
#include "client.h"

namespace client
{
    std::atomic_bool stopped = false;

    void Client::stop()
    {
        stopped = true;
    }

    void setup_connection(int fd, unsigned int port)
    {
        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};

        // connect to peer
        if (connect(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) != 0)
        {
            throw std::runtime_error("Failed to connect to peer: " + std::string(::strerror(errno)));
        }
    }

    void Client::run()
    {
        Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
        if (!socket.is_valid())
        {
            throw std::runtime_error("Failed to bind socket: " + std::string(::strerror(errno)));
        }

        setup_connection(socket.fd(), _port);

        packet::Introduction introduction(_port - 7777);
        const std::string toml = introduction.to_toml();
        if (write(socket.fd(), toml.c_str(), toml.size()) <= 0)
        {
            throw std::runtime_error("Failed to write to socket");
        }

        std::cout << "Sent " << toml << std::endl;

        // std::string buff;
        // buff.reserve(MAX_TCP_BYTES_SIZE);
        // const ssize_t confirm = read(socket.fd(), buff.data(), buff.capacity());
        // if (confirm < 0)
        // {
        //     throw std::runtime_error("Failed to get confirmation message: " + std::string(::strerror(errno)));
        // }

        // std::cout << "Read " << buff << std::endl;
    }

}

#ifdef CLIENT
int main(int argc, char const *argv[])
{
    int port = std::stoi(argv[1]);

    std::cout << "Started client on port " << std::to_string(port) << std::endl;

    client::Client client(7777);
    client.run();

    return 0;
}
#endif