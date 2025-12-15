#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <array>

#define MAX_BACKLOG 10
#define MAX_TCP_BYTES_SIZE 512

long port = 8080;

void setup_port(int fd)
{
    const struct sockaddr_in addr_in = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)}};

    // assign fd to port
    if (bind(fd, (const struct sockaddr *)&addr_in, sizeof(addr_in)) != 0)
    {
        throw std::runtime_error("Failed to bind socket");
    }

    // enable listening
    if (listen(fd, MAX_BACKLOG) != 0)
    {
        throw std::runtime_error("Failed to set listening mode");
    }
}

int main()
{
    std::cerr << "Server init" << std::endl;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
    {
        throw std::runtime_error("Failed to initialize socket");
    }

    (void)setup_port(fd);

    std::cerr << "Waiting to connect to peer..." << std::endl;

    struct sockaddr other_addr;
    socklen_t other_len;
    const int other_fd = accept(fd, &other_addr, &other_len);
    if (other_fd == -1)
    {
        throw std::runtime_error("Failed to accept peer connection");
    }

    // while (true)
    {
        std::array<char, MAX_TCP_BYTES_SIZE> buff;
        const ssize_t count = read(other_fd, buff.data(), buff.size());

        std::cerr << "Read ";
        for (const char c : buff)
        {
            std::cerr << c;
        }
        std::cerr << std::endl;
        if (count <= 0)
        {
            throw std::runtime_error("Failed to read");
        }

        const std::string b = "OK";
        if (write(other_fd, b.c_str(), b.length()) <= 0)
        {
            throw std::runtime_error("Failed to write");
        }
    }

    close(fd);
    close(other_fd);
}
