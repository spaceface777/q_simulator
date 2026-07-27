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
    int fd_ = -1;

public:
    Socket()
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    Socket(int fd)
    {
        fd_ = fd;
    }

    ~Socket()
    {
        if (is_valid())
        {
            ::close(fd_);
        }
    }

    /**
     * Returns whether this socket has a valid file descriptor.
     */
    [[nodiscard]] bool is_valid() const noexcept
    {
        return fd_ >= 0;
    }

    /**
     * Returns the file descriptor described by this socket.
     */
    [[nodiscard]] int fd() const noexcept
    {
        return fd_;
    }

    /**
     * Marks this socket as unused.
     */
    int release() noexcept
    {
        int _ = fd_;
        fd_ = -1;
        return _;
    }

    // disallow reassigning
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    // disallow transfer
    Socket(Socket &&other) noexcept
    {
        fd_ = other.release();
    }
    Socket &operator=(Socket &&other) noexcept
    {
        if (this != &other)
        {
            if (is_valid())
            {
                ::close(fd_);
            }
            fd_ = other.release();
        }
        return *this;
    }
};

#endif // SOCKET_H
