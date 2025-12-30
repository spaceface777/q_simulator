#ifndef SOCKET_H
#define SOCKET_H

#include <stdlib.h>
#include <stdexcept>
#include <unistd.h>

/**
 * Socket wrapper for RAII.
 */
class Socket
{
private:
    int _fd = -1;

public:
    Socket()
    {
        _fd = ::socket(AF_INET, SOCK_STREAM, 0);
    }

    Socket(int fd)
    {
        _fd = fd;
    }

    ~Socket()
    {
        std::cout << "Closed socket" << std::endl;
        if (is_valid())
        {
            ::close(_fd);
        }
    }

    /**
     * Returns whether this socket has a valid file descriptor.
     */
    bool is_valid() const noexcept
    {
        return _fd >= 0;
    }

    /**
     * Returns the file descriptor described by this socket.
     */
    int fd() const noexcept
    {
        return _fd;
    }

    /**
     * Marks this socket as unused.
     */
    int release() noexcept
    {
        int _ = _fd;
        _fd = -1;
        return _;
    }

    // disallow reassigning
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    // disallow transfer
    Socket(Socket &&other) noexcept
    {
        _fd = other.release();
    }
    Socket &operator=(Socket &&other) noexcept
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

#endif // SOCKET_H