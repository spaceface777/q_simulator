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
#include <unordered_map>
#include <mutex>
#include <queue>
#include <algorithm>
#include <memory>
#include <poll.h>
#include <errno.h>

#include "server.h"
#include "socket.h"
#include "../packet/packet.h"
#include "../util.h"
#include "../defs.h"
#include "../module/module.h"

namespace server
{
    std::atomic_bool stopped = false;
    std::vector<unsigned int> fds;
    std::array<uint8_t, MAX_MODULES> id_to_fd;
    std::queue<std::pair<uint8_t, std::unique_ptr<packet::Packet>>> queue;

    /**
     * Sets up the port for a server connection.
     */
    static void setup_port(const Socket &socket, unsigned int port)
    {
        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = ::htons(port),
            .sin_addr = {.s_addr = ::htonl(INADDR_ANY)}};

        // assign fd to port
        if (::bind(socket.fd(), (const struct sockaddr *)&addr_in, sizeof(addr_in)) != 0)
        {
            throw new std::runtime_error("Failed to bind socket: " + std::string(::strerror(errno)));
        }

        // enable listening
        if (::listen(socket.fd(), MAX_TCP_BACKLOG) != 0)
        {
            throw new std::runtime_error("Failed to set listening mode");
        }
    }

    /**
     * Handles incoming connection requests.
     * @param self_fd The fd of the current process.
     */
    static void handle_connection_requests(int self_fd)
    {
        struct pollfd pfd = {
            .fd = self_fd,
            .events = POLLIN | POLLERR | POLLNVAL,
            .revents = 0,
        };

        int selected = ::poll(&pfd, 1, TIMEOUT.count());
        if (selected <= 0)
        {
            return;
        }

        if (pfd.revents & (POLLERR | POLLNVAL))
        {
            return;
        }

        if ((pfd.revents & POLLIN) == 0)
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
     * Removes a fd from memory.
     */
    static void remove(int fd)
    {
        fds.erase(std::remove(fds.begin(), fds.end(), fd), fds.end());
        close(fd);
    }

    /**
     * Parses a buffer to a packet::Packet, and performs the appropriate action.
     */
    static void parse_packet(unsigned int fd, char *buff)
    {
        std::optional<packet::Introduction> intro = packet::Introduction::from_toml(buff);
        if (intro)
        {
            const packet::Introduction packet = intro.value();
            const unsigned int id = packet.id();

            if (id_to_fd[id] > 0)
            {
                std::cout << "Attempted to double up client creation at " << std::to_string(8888 + id) << std::endl;
                return;
            }

            id_to_fd[id] = fd;
            return;
        }

        std::optional<packet::Update> update = packet::Update::from_toml(buff);
        if (update)
        {
            const packet::Update packet = update.value();
            std::cout << "Received update for " << std::to_string(packet.id()) << "'s " << packet.device() << ": " << std::to_string(packet.value()) << std::endl;

            std::optional<std::shared_ptr<module::Module>> ptr = module::get_module(packet.id());
            if (ptr)
            {
                ptr.value()->update(packet);
            }

            return;
        }

        std::optional<packet::PacketSend> sent = packet::PacketSend::from_toml(buff);
        if (sent)
        {
            const packet::PacketSend packet = sent.value();
            std::cout << "Received packet send request to " << std::to_string(packet.module()) << " with content " << packet.content() << std::endl;
            send(packet.module(), packet);
            return;
        }
    }

    /**
     * Handles incoming packets.
     */
    static void handle_incoming_packets()
    {
        struct pollfd pfds[fds.size()];

        // populate pfds
        for (std::size_t i = 0; i < fds.size(); i++)
        {
            struct pollfd pollfd = {
                .fd = static_cast<int>(fds.at(i)),
                .events = POLLIN | POLLERR | POLLNVAL,
                .revents = 0,
            };

            pfds[i] = pollfd;
        }

        int selected = ::poll(pfds, fds.size(), TIMEOUT.count());
        if (selected == 0)
        {
            return;
        }

        for (size_t i = 0; i < fds.size(); i++)
        {
            const unsigned int fd = fds.at(i);

            if (pfds[i].revents == 0)
            {
                continue;
            }
            else if (pfds[i].revents & (POLLERR | POLLNVAL))
            {
                // invalid fd
                remove(fd);
                std::cout << "Closed connection with " << std::to_string(fd) << std::endl;
                continue;
            }

            // there is a value to be read, yay
            char buff[MAX_TCP_BYTES_SIZE];
            memset(buff, '\0', MAX_TCP_BYTES_SIZE); // avoid leftover buff from previous loop
            const ssize_t size = ::read(fd, buff, MAX_TCP_BYTES_SIZE);
            if (size <= 0)
            {
                remove(fd);
                std::cout << "Closed connection with " << std::to_string(fd) << std::endl;
                continue;
            }

            const std::string_view trimmed = util::ltrim(buff);
            const std::size_t first_nl = trimmed.find('\n');
            if (first_nl == trimmed.npos) // invalid message
            {
                std::cout << "Received invalid message" << std::endl;
                continue;
            }

            parse_packet(fd, buff);
        }
    }

    static void handle_outbound_packets()
    {
        while (!queue.empty())
        {
            std::pair<uint8_t, std::unique_ptr<packet::Packet>> pair = std::move(queue.front());

            if (id_to_fd[pair.first] == 0)
            {
                continue;
            }

            queue.pop();

            const unsigned int fd = id_to_fd[pair.first];
            const std::string toml = pair.second->to_toml();
            const ssize_t size = ::write(fd, toml.c_str(), toml.length() + 1); // include '\0'
            if (size <= 0)
            {
                remove(fd);
                std::cout << "Closed connection with " << std::to_string(fd) << std::endl;
                continue;
            }
        }
    }

    void send(unsigned int id, const packet::Packet &packet)
    {
        if (stopped || id > MAX_MODULES)
        {
            return;
        }

        queue.push({id, std::make_unique<packet::Packet>(packet)});
    }

    void run(unsigned int port)
    {
        const Socket socket;
        if (!socket.is_valid())
        {
            throw new std::runtime_error("Failed to initialize socket");
        }
        setup_port(socket, port);

        fds.reserve(MAX_MODULES);

        while (!stopped)
        {
            handle_connection_requests(socket.fd());

            handle_incoming_packets();

            handle_outbound_packets();

            std::this_thread::sleep_for(TIMEOUT);
        }
    }

    void stop()
    {
        stopped = true;
    }

    std::size_t size()
    {
        return fds.size();
    }

}
