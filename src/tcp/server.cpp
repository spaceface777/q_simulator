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
#include <future>

#include "server.h"
#include "socket.h"
#include "../message/message.h"
#include "../util.h"
#include "../defs.h"
#include "../settings.h"
#include "../module/module.h"

namespace server
{
    static std::future<void> server_future_;
    static std::atomic_bool stopped = false;

    static constexpr int PORT = 7777;

    static std::vector<unsigned int> fds;
    static std::unordered_map<uint8_t, int> module_fds;
    static std::mutex socket_mutex;

    /**
     * Sets up the port for a server connection.
     */
    static void setup_port(const Socket &socket)
    {
        struct sockaddr_in addr_in;
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = ::htons(PORT);
        addr_in.sin_addr.s_addr = INADDR_ANY;

        int opt = 1;
        if (::setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        {
            stop();
            throw std::runtime_error("Failed to set SO_REUSEADDR: " + std::string(::strerror(errno)));
        }

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
        struct pollfd pfd;
        pfd.fd = socket.fd();
        pfd.events = POLLIN | POLLERR | POLLNVAL;
        pfd.revents = 0;

        int selected = ::poll(&pfd, 1, 0);
        if (selected <= 0)
        {
            return;
        }
        if (pfd.revents & (POLLERR | POLLNVAL))
        {
            return;
        }
        if (pfd.revents & POLLIN)
        {
            sockaddr other_addr;
            socklen_t other_len = sizeof(other_addr);
            int other = ::accept(socket.fd(), &other_addr, &other_len);
            if (other < 0)
            {
                return;
            }

            fds.push_back(other);
        }
    }

    /**
     * Removes a fd from memory.
     */
    static void remove(int fd)
    {
        std::remove(fds.begin(), fds.end(), fd);
    }

    void send(module::ModuleId id, const message::Message &message)
    {
        if (stopped)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(socket_mutex);
        auto it = module_fds.find(id.raw());
        if (it != module_fds.end())
        {
            std::string toml = message.to_toml();
            if (::write(it->second, toml.c_str(), toml.length() + 1) <= 0)
            {
                remove(it->second);
                std::cout << "Closed connection with " << std::to_string(it->second) << std::endl;
            }
        }
    }

    /**
     * Parses a buffer to messages.
     */
    static void parse_messages(int module_fd, char *buff)
    {
        char *msg = buff;
        while (true)
        {
            char *end = strchr(msg, '\0');
            if (end == NULL || end == msg)
            {
                break;
            }

            {
                const std::optional<message::Init> intro = message::Init::from_toml(msg);
                if (intro)
                {
                    const message::Init message = intro.value();
                    const module::ModuleId id = message.id();

                    std::lock_guard<std::mutex> lock(socket_mutex);
                    module_fds[id.raw()] = module_fd;

                    std::cout << "Registered module " << std::to_string(id.raw()) << " at fd " << std::to_string(module_fd) << std::endl;

                    goto next;
                }
            }

            {
                const std::optional<message::InitFinish> init_finish = message::InitFinish::from_toml(msg);
                if (init_finish)
                {
                    const std::optional<std::shared_ptr<module::Module>> ptr = module::get_module(init_finish.value().id());
                    if (ptr)
                    {
                        ptr.value()->set_initialization_finished(true);
                    }

                    goto next;
                }
            }

            {
                const std::optional<message::Update> update = message::Update::from_toml(msg);
                if (update)
                {
                    const message::Update message = update.value();
                    const std::optional<std::shared_ptr<module::Module>> ptr = module::get_module(message.module());
                    if (ptr)
                    {
                        ptr.value()->update(message);
                    }

                    goto next;
                }
            }

            {
                const std::optional<message::Packet> sent = message::Packet::from_toml(msg);
                if (sent)
                {
                    const message::Packet message = sent.value();
                    send(message.module(), message);
                    goto next;
                }
            }

        next:
            msg += end - msg + 1;
        }
    }

    /**
     * Handles incoming packets.
     */
    static void handle_packets()
    {
        std::vector<struct pollfd> pfds(fds.size());

        // populate pfds
        for (std::size_t i = 0; i < fds.size(); i++)
        {
            struct pollfd pollfd;
            pollfd.fd = static_cast<int>(fds.at(i));
            pollfd.events = POLLIN | POLLERR | POLLNVAL | POLLHUP;
            pollfd.revents = 0;

            pfds[i] = pollfd;
        }

        int selected = ::poll(pfds.data(), pfds.size(), 0);
        if (selected <= 0)
        {
            return;
        }

        for (size_t i = 0; i < pfds.size(); i++)
        {
            const unsigned int fd = pfds.at(i).fd;

            if (pfds[i].revents == 0)
            {
                continue;
            }
            if (pfds[i].revents & (POLLERR | POLLNVAL | POLLHUP))
            {
                // invalid fd
                remove(fd);
                std::cout << "Closed connection with " << std::to_string(fd) << std::endl;
                continue;
            }
            if (pfds[i].revents & POLLIN)
            {
                // there is a value to be read, yay
                char buff[MAX_TCP_BYTES_SIZE] = {0}; // avoid leftover buff from previous loop
                const ssize_t size = ::recv(fd, buff, MAX_TCP_BYTES_SIZE - 1, MSG_DONTWAIT);
                if (size <= 0)
                {
                    remove(fd);
                    std::cout << "Closed connection with " << std::to_string(fd) << std::endl;
                    continue;
                }

                parse_messages(fd, buff);
            }
        }
    }

    static void run_()
    {
        const Socket socket;
        if (!socket.is_valid())
        {
            stop();
            throw std::runtime_error("Failed to initialize socket");
        }
        setup_port(socket);

        fds.reserve(MAX_MODULES);

        std::cout << "Server waiting for connections" << std::endl;

        while (!stopped)
        {
            if (settings::get_setting<double>("simulation_speed") == 0.0)
            {
                std::this_thread::sleep_for(TIMEOUT);
                continue;
            }

            handle_connection_requests(socket);

            handle_packets();

            std::this_thread::sleep_for(TIMEOUT);
        }
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

    void join()
    {
        if (server_future_.valid())
        {
            server_future_.wait();
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
