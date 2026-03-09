#ifndef CLIENT_H
#define CLIENT_H

#include <unistd.h>
#include <memory>
#include <queue>
#include <thread>

// only used for testing
namespace client
{
    class Client
    {
    private:
        int fd_ = -1;
        unsigned int id_ = 0;
        unsigned int port_ = 0;
        std::queue<std::unique_ptr<packet::Packet>> queue_;
        std::unique_ptr<std::thread> thread_ptr_;

    public:
        Client() = default;

        Client(unsigned int id, unsigned int port) : id_(id), port_(port)
        {
        }

        ~Client()
        {
            ::close(fd_);
            if (thread_ptr_ && thread_ptr_->joinable())
            {
                thread_ptr_->join();
            }
        }

        unsigned int id() const noexcept
        {
            return id_;
        }

        unsigned int port() const noexcept
        {
            return port_;
        }

        int fd() const noexcept
        {
            return fd_;
        }

        void set_fd(int fd) noexcept
        {
            fd_ = fd;
        }

        bool is_valid() const noexcept
        {
            return fd_ >= 0;
        }

        void queue(const packet::Packet &packet) noexcept
        {
            queue_.push(packet.clone());
        }

    private:
        void _run();

        void _stop();

    public:
        void run();
    };

    /**
     * Adds a module.
     * @param ptr A unique_ptr to the client.
     */
    void add_client(std::unique_ptr<client::Client> ptr);

    /**
     * Gets an optional client from its id.
     * @param id The id.
     * @returns The client instance from the id.
     */
    std::optional<std::shared_ptr<client::Client>> get_client(uint8_t id);

    /**
     * Closes all clients.
     */
    void close_clients();

}

#endif // CLIENT_H