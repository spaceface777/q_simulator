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
#include "client.h"
#include "socket.h"
#include "../message/message.h"
#include "../util.h"
#include "../defs.h"
#include "../module/module.h"

#include <future>

namespace server
{
    static std::future<void> server_future_;
    static std::atomic_bool stopped = false;

    static constexpr int PORT = 7777;

    static std::vector<unsigned int> fds;

    /**
     * Sets up the port for a server connection.
     */
    static void setup_port(const Socket &socket)
    {
        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = ::htons(PORT),
            .sin_addr = {.s_addr = ::htonl(INADDR_ANY)}};

        // assign fd to port
        if (::bind(socket.fd(), (const struct sockaddr *)&addr_in, sizeof(addr_in)) != 0)
        {
            stop();
            throw std::runtime_error("Failed to bind socket: " + std::string(::strerror(errno)));
        }

        // enable listening
        if (::listen(socket.fd(), MAX_TCP_BACKLOG) != 0)
        {
            stop();
            throw std::runtime_error("Failed to set listening mode");
        }
    }

    /**
     * Handles incoming connection requests.
     * @param self_fd The fd of the current process.
     */
    static void handle_connection_requests(const Socket &socket)
    {
        struct pollfd pfd = {
            .fd = socket.fd(),
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
        int other = ::accept(socket.fd(), &other_addr, &other_len);
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
    }

    void send(unsigned int id, const message::Message &message)
    {
        if (stopped || id > MAX_MODULES)
        {
            return;
        }

        const std::optional<std::shared_ptr<client::Client>> client = client::get_client(id);
        if (!client)
        {
            return;
        }

        client.value()->queue(message);
    }

    /**
     * Parses a buffer to a message::Packet, and performs the appropriate action.
     */
    static void parse_packet(unsigned int fd, char *buff)
    {
        std::optional<message::Introduction> intro = message::Introduction::from_toml(buff);
        if (intro)
        {
            const message::Introduction message = intro.value();
            const unsigned int id = message.id();

            const std::optional<std::shared_ptr<client::Client>> existing_client = client::get_client(id);
            if (existing_client)
            {
                std::cout << "Attempted to double up client creation at " << std::to_string(8888 + id) << std::endl;
                return;
            }

            std::shared_ptr<client::Client> ptr = std::make_shared<client::Client>(id, 8888 + id);
            ptr->run();
            client::add_client(ptr);

            std::cout << "Started client at " << std::to_string(8888 + id) << std::endl;

            return;
        }

        std::optional<message::Update> update = message::Update::from_toml(buff);
        if (update)
        {
            const message::Update message = update.value();
            // std::cout << "Received update for " << std::to_string(message.id()) << "'s " << message.device() << ": " << std::to_string(message.value()) << std::endl;

            std::optional<std::shared_ptr<module::Module>> ptr = module::get_module(message.id());
            if (ptr)
            {
                ptr.value()->update(message);
            }

            return;
        }

        std::optional<message::Packet> sent = message::Packet::from_toml(buff);
        if (sent)
        {
            const message::Packet message = sent.value();
            // std::cout << "Received message send request to " << std::to_string(message.module()) << " with content " << message.content() << std::endl;
            send(message.module(), message);
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

    static void run_()
    {
        const Socket socket;
        if (!socket.is_valid())
        {
            stop();
            close(socket.fd());
            throw std::runtime_error("Failed to initialize socket");
        }
        setup_port(socket);

        fds.reserve(MAX_MODULES);

        std::cout << "Server waiting for connections" << std::endl;

        while (!stopped)
        {
            handle_connection_requests(socket);

            handle_packets();

            std::this_thread::sleep_for(TIMEOUT);
        }

        close(socket.fd());
    }

    void run()
    {
        util::require(!stopped, "Cannot run two servers");

        server_future_ = std::async(std::launch::async, []
                                    { run_(); });
    }

    void stop()
    {
        stopped = true;

        for (size_t i = 0; i < fds.size(); i++)
        {
            close(fds[i]);
        }
    }

    std::size_t size()
    {
        return fds.size();
    }

    void check()
    {
        if (!server_future_.valid())
        {
            return;
        }

        auto status = server_future_.wait_for(std::chrono::seconds(0));
        if (status == std::future_status::ready)
        {
            server_future_.get();
        }
    }
}
