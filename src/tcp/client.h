#ifndef CLIENT_H
#define CLIENT_H

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

        void stop();

        void run();
    };

}

#endif // CLIENT_H