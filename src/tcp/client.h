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
        int _fd = -1;
        unsigned int _id = 0;
        unsigned int _port = 0;
        std::queue<std::unique_ptr<packet::Packet>> _queue;
        std::unique_ptr<std::thread> _thread_ptr;

    public:
        Client() = default;

        Client(unsigned int id, unsigned int port)
        {
            _id = id;
            _port = port;
        }

        ~Client()
        {
            ::close(_fd);
            if (_thread_ptr && _thread_ptr->joinable())
            {
                _thread_ptr->join();
            }
        }

        unsigned int id() const noexcept
        {
            return _id;
        }

        unsigned int port() const noexcept
        {
            return _port;
        }

        int fd() const noexcept
        {
            return _fd;
        }

        void set_fd(int fd) noexcept
        {
            _fd = fd;
        }

        bool is_valid() const noexcept
        {
            return _fd >= 0;
        }

        void queue(const packet::Packet &packet) noexcept
        {
            _queue.push(packet.clone());
        }

        void pop() noexcept
        {
            _queue.pop();
        }

        bool empty() const noexcept
        {
            return _queue.empty();
        }

        std::size_t size() const noexcept
        {
            return _queue.size();
        }

        const packet::Packet &next() const noexcept
        {
            return *(_queue.front());
        }

        void run();

        void stop();
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

}

#endif // CLIENT_H