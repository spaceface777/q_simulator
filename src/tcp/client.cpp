#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <queue>

#include "../packet/packet.h"
#include "../util.h"
#include "../defs.h"
#include "socket.h"
#include "client.h"

namespace client
{
    static std::atomic_bool stopped = false;

    static void setup_connection(int fd, unsigned int port, unsigned int id)
    {
        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};

        if (::connect(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) < 0)
        {
            throw new std::runtime_error("Failed to connect to socket");
            return;
        }
    }

    static void _run(Client *client)
    {
        const Socket socket;
        if (!socket.is_valid())
        {
            throw std::runtime_error("Failed to bind socket");
        }

        client->set_fd(socket.fd());
        setup_connection(socket.fd(), client->port(), client->id());

        std::cout << "Connected to module " << std::to_string(client->id()) << std::endl;

        while (!stopped)
        {
            if (client->empty())
            {
                std::this_thread::sleep_for(TIMEOUT);
                continue;
            }

            const packet::Packet &packet = client->next();

            std::cout << "Size " << std::to_string(client->size()) << std::endl;
            std::cout << "Sending " << packet.to_toml() << std::endl;
            const std::string toml = packet.to_toml();
            if (::write(client->fd(), toml.c_str(), toml.length() + 1) < 0) // include '\0'
            {
                std::this_thread::sleep_for(TIMEOUT);
                continue;
            }

            client->pop();
        }
    }

    void Client::run()
    {
        _thread_ptr = std::make_unique<std::thread>(std::thread(_run, this));
    }

    void Client::stop()
    {
        stopped = true;

        if (_thread_ptr && _thread_ptr->joinable())
        {
            _thread_ptr->join();
        }
    }
}