#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstring>
#include <thread>

#include "../packet/packet.h"
#include "../util.h"
#include "socket.h"
#include "client.h"

namespace client
{
    static std::atomic_bool stopped = false;

    void Client::stop()
    {
        stopped = true;
    }

    static void setup_connection(int fd, unsigned int port, unsigned int id)
    {
        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};

        // connect to peer
        uint8_t attempts = 0;
        while (connect(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) < 0)
        {
            std::cerr << "[" + std::to_string(id) + "] Failed to connect to server: " + std::string(::strerror(errno)) << std::endl;
            std::cerr << "[" + std::to_string(id) + "] Attempting to reconnect" << std::endl;

            std::this_thread::sleep_for(std::chrono::seconds(1));

            attempts++;
            if (attempts > 10)
            {
                throw new std::runtime_error("Failed to connect to server");
            }
        }

        const packet::Introduction intro(id);
        const std::string toml = intro.to_toml();

        attempts = 0;
        while (::write(fd, toml.c_str(), toml.size()) <= 0)
        {
            std::cerr << "[" + std::to_string(id) + "] Failed to introduce: " + std::string(::strerror(errno)) << std::endl;
            std::cerr << "[" + std::to_string(id) + "] Attempting to resend" << std::endl;

            std::this_thread::sleep_for(std::chrono::seconds(1));

            attempts++;
            if (attempts > 10)
            {
                throw new std::runtime_error("Failed to send introduction to server");
            }
        }
    }

    void Client::run()
    {
        Socket socket;
        if (!socket.is_valid())
        {
            throw std::runtime_error("Failed to bind socket: " + std::string(::strerror(errno)));
        }

        setup_connection(socket.fd(), _port, _id);

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        for (size_t i = 0; i < 20; i++)
        {
            const packet::Update update(_id, "led", 0x203040);
            const std::string toml = update.to_toml();
            if (::write(socket.fd(), toml.c_str(), toml.size()) <= 0)
            {
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

}

#ifdef CLIENT
int main(int argc, char const *argv[])
{
    int id = std::stoi(argv[1]);
    int port = std::stoi(argv[2]);

    std::cout << "Started client on port " << std::to_string(port) << std::endl;

    client::Client client(id, 7777);
    client.run();

    return 0;
}
#endif