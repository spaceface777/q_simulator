#ifndef CLIENT_H
#define CLIENT_H

#include <unistd.h>
#include <memory>
#include <queue>
#include <thread>
#include <mutex>
#include <future>

// only used for testing
namespace client
{
    class Client
    {
    private:
        int fd_ = -1;
        unsigned int id_ = 0;
        unsigned int port_ = 0;
        std::queue<std::unique_ptr<message::Message>> queue_;
        std::mutex mutex_;

        std::future<void> client_future_;
        std::atomic_bool stopped_ = false;

    public:
        Client() = default;

        Client(unsigned int id, unsigned int port) : id_(id), port_(port)
        {
        }

        ~Client()
        {
            ::close(fd_);
        }

        unsigned int id() const noexcept
        {
            return id_;
        }

        unsigned int port() const noexcept
        {
            return port_;
        }

        bool is_valid() const noexcept
        {
            return fd_ >= 0;
        }

        void queue(const message::Message &message) noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(message.clone());
        }

    private:
        void run_();

    public:
        void run();

        void check();

        void stop();
    };

    /**
     * Adds a module.
     * @param ptr A shared_ptr to the client.
     */
    void add_client(std::shared_ptr<client::Client> ptr);

    /**
     * Gets an optional client from its id.
     * @param id The id.
     * @returns The client instance from the id.
     */
    std::optional<std::shared_ptr<client::Client>> get_client(uint8_t id);

    void stop();

    void check();
}

#endif // CLIENT_H