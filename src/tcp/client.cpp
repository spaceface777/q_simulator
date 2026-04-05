#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <queue>
#include <map>

#include "../packet/packet.h"
#include "../util.h"
#include "../defs.h"
#include "socket.h"
#include "client.h"

namespace client
{
    static std::exception_ptr exception_ptr;
    static std::atomic_bool stopped = false;

    static void setup_connection(int fd, unsigned int port, unsigned int id)
    {
        const struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};

        std::cout << "Connecting to module " << std::to_string(id) << " at port " << std::to_string(port) << " and fd " << std::to_string(fd) << std::endl;
        if (::connect(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) < 0)
        {
            throw std::runtime_error("Failed to connect to socket");
        }
    }

    void Client::_run()
    {
        const Socket socket;
        if (!socket.is_valid())
        {
            throw std::runtime_error("Failed to bind socket");
        }

        setup_connection(socket.fd(), port_, id_);
        fd_ = socket.fd();

        std::cout << "Connected to module " << std::to_string(id_) << std::endl;

        while (!stopped)
        {
            std::unique_ptr<packet::Packet> packet;
            size_t current_size = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty())
                {
                    packet = std::move(queue_.front());
                    queue_.pop();
                    current_size = queue_.size();
                }
            }

            if (!packet)
            {
                std::this_thread::sleep_for(TIMEOUT);
                continue;
            }

            std::cout << "Size " << std::to_string(current_size) << std::endl;
            std::cout << "Sending " << packet->to_toml() << " to " << std::to_string(socket.fd()) << std::endl;
            const std::string toml = packet->to_toml();
            if (::write(socket.fd(), toml.c_str(), toml.length() + 1) < 0) // include '\0'
            {
                std::this_thread::sleep_for(TIMEOUT);
            }
        }
    }

    void Client::run()
    {
        if (thread_ptr_)
        {
            throw std::runtime_error("Double client instantiation");
        }

        thread_ptr_ = std::make_unique<std::thread>(
            [this]
            {
                try
                {
                    this->_run();
                }
                catch (const std::exception &ex)
                {
                    exception_ptr = std::current_exception();
                }
            });
    }

    void Client::_stop()
    {
        stopped = true;
    }

    static std::map<uint8_t, std::shared_ptr<Client>> map;

    void add_client(std::shared_ptr<Client> ptr)
    {
        if (map.count(ptr->id()))
        {
            throw std::invalid_argument("Attempted to double register clients");
        }
        map[ptr->id()] = std::move(ptr);
    }

    std::optional<std::shared_ptr<Client>> get_client(uint8_t id)
    {
        if (map.count(id))
        {
            return map.at(id);
        }
        return {};
    }

    void close_clients()
    {
        for (auto &[key, val] : map)
        {
            val->queue(packet::Shutdown());
        }
    }

    void rethrow()
    {
        if (exception_ptr)
        {
            fprintf(stderr, "Rethrowing exception from client thread\n");
            std::rethrow_exception(exception_ptr);
        }
    }

}