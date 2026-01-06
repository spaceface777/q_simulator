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
#include <cassert>

#include "socket.h"
#include "tcp.h"
#include "../packet/packet.h"
#include "../util.h"

namespace server
{
    static constexpr unsigned int TIMEOUT_MICROS = 250000;

    std::atomic_bool stopped = false;
    std::vector<unsigned int> fds;
    std::map<unsigned int, uint8_t> fdToId;

    void stop()
    {
        stopped = true;
    }

    /**
     * Sets up the port for a server connection.
     */
    static void setup_port(int fd, unsigned int port)
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

    /**
     * Handles incoming connection requests.
     * @param self_fd The fd of the current process.
     */
    static void handle_connection_requests(int self_fd)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(self_fd, &rfds);

        timeval timeout = {.tv_usec = TIMEOUT_MICROS};

        int selected = select(self_fd + 1, &rfds, NULL, NULL, &timeout);
        if (selected <= 0)
        {
            return;
        }

        sockaddr other_addr;
        socklen_t other_len = sizeof(other_addr);
        int other = ::accept(self_fd, &other_addr, &other_len);
        if (other < 0)
        {
            return;
        }

        fds.push_back(other);
    }

    /**
     * Handles incoming packets.
     */
    static void handle_packets()
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;
        for (const unsigned int fd : fds)
        {
            FD_SET(fd, &rfds);
            max_fd = std::max(max_fd, (int) fd);
        }

        timeval timeout = {.tv_usec = TIMEOUT_MICROS};

        int selected = select(max_fd + 1, &rfds, NULL, NULL, &timeout);
        if (selected <= 0)
        {
            return;
        }

        for (const unsigned int fd : fds)
        {
            if (!FD_ISSET(fd, &rfds))
            {
                continue;
            }

            // there is a value to be read, yay
            char buff[MAX_TCP_BYTES_SIZE];
            const ssize_t size = ::read(fd, buff, MAX_TCP_BYTES_SIZE);
            if (size <= 0)
            {
                continue;
            }

            const std::string_view trimmed = util::ltrim(buff);
            const std::size_t first_nl = trimmed.find('\n');
            if (first_nl == trimmed.npos) // invalid message
            {
                continue;
            }

            const std::string_view header = trimmed.substr(0, first_nl);
            if (header == "[introduction]") {
                const packet::Introduction intro = packet::Introduction::from_toml(buff);
                if (intro.is_valid()) {
                    fdToId.insert({fd, intro.id()});
                    std::cout << "[S] Client " << std::to_string(intro.id()) << " assigned to fd " << std::to_string(fd) << std::endl;
                }
            }
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

        fds.reserve(MAX_MODULES);

        fd_set rfds;
        struct timeval timeout;
        while (!stopped)
        {
            handle_connection_requests(self);

            handle_packets();
        }
    }

}