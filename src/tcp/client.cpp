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

#include "../message/message.h"
#include "../settings.h"
#include "../util.h"
#include "../defs.h"
#include "socket.h"
#include "client.h"

namespace client
{

    static std::map<uint8_t, std::shared_ptr<Client>> map;

    void stop()
    {
        for (auto &[key, val] : map)
        {
            val->stop();
        }
        map.clear();
    }

    static bool setup_connection(int fd, unsigned int port, unsigned int id)
    {
        struct sockaddr_in addr_in;
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = ::htons(port);
        addr_in.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

        std::cout << "Connecting to module " << std::to_string(id) << " at port " << std::to_string(port) << " and fd " << std::to_string(fd) << std::endl;
        if (::connect(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) < 0)
        {
            return false;
        }
        return true;
    }

    void Client::run_()
    {
        while (!stopped_)
        {
            const Socket socket;
            if (!socket.is_valid())
            {
                stop();
                throw std::runtime_error("Failed to bind socket");
            }

            if (!setup_connection(socket.fd(), port_, id_))
            {
                std::this_thread::sleep_for(TIMEOUT);
                continue;
            }

            fd_ = socket.fd();
            std::cout << "Connected to module " << std::to_string(id_) << std::endl;

            while (!stopped_)
            {
                if (settings::get_setting<double>("simulation_speed") == 0.0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                std::unique_ptr<message::Message> message;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!queue_.empty())
                    {
                        message = std::move(queue_.front());
                        queue_.pop();
                    }
                }

                if (!message)
                {
                    std::this_thread::sleep_for(TIMEOUT);
                    continue;
                }

                const std::string toml = message->to_toml();
                if (::write(socket.fd(), toml.c_str(), toml.length() + 1) < 0) // include '\0'
                {
                    break;
                }
            }

            close(fd_);
        }
    }

    void Client::run()
    {
        util::require(!stopped_, "Cannot run the same client twice");

        client_future_ = std::async(std::launch::async, [this]
                                    { run_(); });
    }

    void Client::check()
    {
        if (!client_future_.valid())
        {
            return;
        }

        auto status = client_future_.wait_for(std::chrono::seconds(0));
        if (status == std::future_status::ready)
        {
            client_future_.get();
        }
    }

    void Client::stop()
    {
        stopped_ = true;
    }

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

    void check()
    {
        for (auto &[key, val] : map)
        {
            val->check();
        }
    }

}