#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstring>
#include <utility>
#include <thread>
#include <map>
#include <mutex>
#include <queue>

#include "socket.h"
#include "tcp.h"
#include "../packet/packet.h"
#include "../util.h"

namespace server
{
    std::atomic_bool stopped = false;

    void stop()
    {
        stopped = true;
    }

    std::mutex incoming_lock;
    std::queue<packet::Packet> incoming;

    /**
     * Handles incoming messages for a single module.
     */
    void handle_incoming(int self_fd, int other_fd)
    {
        Socket self(self_fd);
        Socket other(other_fd);
        int id = -1;

        // initialize id variable
        {
            char buff[MAX_TCP_BYTES_SIZE];
            const ssize_t count = ::read(other.fd(), buff, MAX_TCP_BYTES_SIZE);
            if (count <= 0)
            {
                return;
            }

            try
            {
                const packet::Introduction introduction = packet::Introduction::from_toml(buff);
                id = introduction.id();
                std::cout << "Registered for id " << std::to_string(id) << std::endl;
            }
            catch (const packet::DeformedPacketException &e)
            {
                std::cout << "[S] 1y" << std::endl;
                return;
            }

            const packet::Confirm confirm;
            const std::string toml = confirm.to_toml();
            if (::write(other.fd(), toml.c_str(), toml.size()) <= 0)
            {
                return;
            }
        }

        // while (!stopped)
        {
            char buff[MAX_TCP_BYTES_SIZE];
            const ssize_t count = ::read(other.fd(), buff, MAX_TCP_BYTES_SIZE);
            if (count <= 0)
            {
                return;
            }

            std::cout << "[S] 1" << std::endl;

            try
            {
                std::cout << "Received some shi from " << std::to_string(id) << std::endl;
            }
            catch (const packet::DeformedPacketException &e)
            {
                return;
            }
        }
    }

    /**
     * Sets up the port for a server connection.
     */
    void setup_port(int fd, unsigned int port)
    {
        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = ::htons(port),
            .sin_addr = {.s_addr = ::htonl(INADDR_ANY)}};

        // assign fd to port
        if (::bind(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) != 0)
        {
            close(fd);
            throw std::runtime_error("Failed to bind socket: " + std::string(::strerror(errno)));
        }

        // enable listening
        if (::listen(fd, MAX_TCP_BACKLOG) != 0)
        {
            close(fd);
            throw std::runtime_error("Failed to set listening mode: " + std::string(::strerror(errno)));
        }
    }

    void run(unsigned int port)
    {
        int self = ::socket(AF_INET, SOCK_STREAM, 0);
        if (self < 0)
        {
            throw std::runtime_error("Failed to initialize socket: " + std::string(::strerror(errno)));
        }

        setup_port(self, port);

        while (!stopped)
        {
            std::cout << "Waiting to connect to peer..." << std::endl;

            struct sockaddr other_addr;
            socklen_t other_len = sizeof(other_addr);
            int other = ::accept(self, &other_addr, &other_len);
            if (other < 0)
            {
                continue;
            }

            std::thread thread(handle_incoming, self, other);
            thread.detach();
        }
    }

}