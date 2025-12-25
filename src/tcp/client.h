#ifndef CLIENT_H
#define CLIENT_H

#include <unistd.h>

// only used for testing
namespace client
{
    class Client
    {
    private:
        int _fd;
        unsigned int _port;

    public:
        Client(unsigned int port)
        {
            _port = port;
        }

        ~Client()
        {
            if (is_valid())
            {
                ::close(_fd);
            }
        }

        int fd() const noexcept
        {
            return _fd;
        }

        bool is_valid() const noexcept
        {
            return _fd >= 0;
        }

        void stop();

        void run();

        int release() noexcept
        {
            int _ = _fd;
            _fd = -1;
            return _;
        }

        // disallow reassigning
        Client(const Client &) = delete;
        Client &operator=(const Client &) = delete;

        // disallow transfer
        Client(Client &&other) noexcept
        {
            _fd = other.release();
        }
        Client &operator=(Client &&other) noexcept
        {
            if (this != &other)
            {
                if (is_valid())
                {
                    ::close(_fd);
                }
                _fd = other.release();
            }
            return *this;
        }
    };

}

#endif // CLIENT_H