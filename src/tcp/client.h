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
        std::queue<packet::Packet> _queue;
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
            if (is_valid())
            {
                ::close(_fd);
            }
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

        bool is_valid() const noexcept
        {
            return _fd >= 0;
        }

        void queue(packet::Packet packet) noexcept
        {
            _queue.push(packet);
        }

        void pop() noexcept
        {
            _queue.pop();
        }

        bool empty() const noexcept
        {
            return _queue.empty();
        }

        packet::Packet next() const noexcept
        {
            return _queue.front();
        }

        void run();

        void stop();
    };

}

#endif // CLIENT_H