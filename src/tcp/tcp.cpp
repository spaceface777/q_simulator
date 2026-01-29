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
#include <cassert>
#include <cerrno>
#include <algorithm>
#include <poll.h>

#include "tcp.h"
#include "../packet/packet.h"
#include "../util.h"

namespace server
{
    static constexpr std::chrono::duration TIMEOUT = std::chrono::milliseconds(100);

    std::atomic_bool stopped = false;
    std::vector<unsigned int> fds;
    std::unordered_map<unsigned int, uint8_t> fdToId;
    std::unordered_map<uint8_t, unsigned int> idToFd;

    void stop()
    {
        stopped = true;
    }

    /**
     * Sets up the port for a server connection.
     */
    static int setup_port(unsigned int port)
    {
        int fd = -1;
        uint8_t attempts = 0;
        while ((fd = ::socket(AF_INET, SOCK_STREAM, 0)) < 0)
        {
            std::cerr << "Failed to initialize socket: " + std::string(::strerror(errno)) << std::endl;

            std::this_thread::sleep_for(std::chrono::seconds(1));

            attempts++;
            if (attempts > 10)
            {
                throw new std::runtime_error("Failed to initialize socket");
            }
        }

        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = ::htons(port),
            .sin_addr = {.s_addr = ::htonl(INADDR_ANY)}};

        // assign fd to port
        attempts = 0;
        while (::bind(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) != 0)
        {
            ::close(fd);
            std::cerr << "Failed to bind socket: " + std::string(::strerror(errno)) << std::endl;

            std::this_thread::sleep_for(std::chrono::seconds(1));

            attempts++;
            if (attempts > 10)
            {
                throw new std::runtime_error("Failed to bind socket");
            }
        }

        // enable listening
        attempts = 0;
        while (::listen(fd, MAX_TCP_BACKLOG) != 0)
        {
            ::close(fd);
            std::cerr << "Failed to set listening mode: " + std::string(::strerror(errno)) << std::endl;

            std::this_thread::sleep_for(std::chrono::seconds(1));

            attempts++;
            if (attempts > 10)
            {
                throw new std::runtime_error("Failed to set listening mode");
            }
        }

        return fd;
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

        if (fdToId.count(fd) == 0)
        {
            return;
        }

        unsigned int id = fdToId.at(fd);
        fdToId.erase(fd);
        idToFd.erase(id);
    }

    static void parse_packet(unsigned int fd, char *buff)
    {
        std::optional<packet::Introduction> intro = packet::Introduction::from_toml(buff);
        if (intro)
        {
            const packet::Introduction packet = intro.value();
            const unsigned int id = packet.id();
            fdToId.insert({fd, id});
            idToFd.insert({id, fd});
            std::cout << "[S] Client " << std::to_string(id) << " assigned to fd " << std::to_string(fd) << std::endl;
            return;
        }

        std::optional<packet::Update> update = packet::Update::from_toml(buff);
        if (update)
        {
            const packet::Update packet = update.value();
            std::cout << "Received update for " << std::to_string(packet.id()) << "'s " << packet.device() << ": " << std::to_string(packet.value()) << std::endl;
            return;
        }

        std::optional<packet::PacketSend> sent = packet::PacketSend::from_toml(buff);
        if (sent)
        {
            const packet::PacketSend packet = sent.value();
            std::cout << "Received packet send request to " << std::to_string(packet.module()) << " with content " << packet.content() << std::endl;
            send(0, packet);
            return;
        }
    }

    /**
     * Handles incoming packets.
     */
    static void handle_packets()
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

    void send(int id, packet::Packet packet)
    {
        if (stopped || idToFd.count(id) == 0)
        {
            return;
        }

        const unsigned int fd = idToFd.at(id);
    }

    void run(unsigned int port)
    {
        int fd = setup_port(port);

        fds.reserve(MAX_MODULES);

        fd_set rfds;
        struct timeval timeout;
        while (!stopped)
        {
            handle_connection_requests(fd);

            handle_packets();
        }
    }

}
