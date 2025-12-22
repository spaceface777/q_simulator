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
    Socket(int fd)
    {
        _fd = fd;
    }

    ~Socket()
    {
        if (valid())
        {
            ::close(_fd);
        }
    }

    /**
     * Returns whether this socket has a valid file descriptor.
     */
    bool valid()
    {
        return _fd >= 0;
    }

    /**
     * Returns the file descriptor described by this socket.
     */
    int fd() const
    {
        return _fd;
    }

    /**
     * Marks this socket as unused.
     */
    int release()
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
            if (valid())
            {
                ::close(_fd);
            }
            _fd = other.release();
        }
        return *this;
    }
};

#endif // SOCKET_H