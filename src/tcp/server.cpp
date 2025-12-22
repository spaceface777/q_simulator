#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <array>
#include <atomic>

#include "socket.h"
#include "server.h"

#define MAX_BACKLOG 10
#define MAX_TCP_BYTES_SIZE 512

std::atomic_bool stopped = false;

void server::stop() {
    stopped = true;
}

void setup_port(int fd, unsigned long port)
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

void server::run(unsigned long port)
{
    std::cerr << "Server init" << std::endl;

    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (!socket.is_valid())
    {
        throw std::runtime_error("Failed to initialize socket");
    }

    setup_port(socket.fd(), port);

    std::cerr << "Waiting to connect to peer..." << std::endl;

    struct sockaddr other_addr;
    socklen_t other_len;
    const int other_fd = accept(socket.fd(), &other_addr, &other_len);
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
}
