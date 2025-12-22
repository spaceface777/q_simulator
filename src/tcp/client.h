#ifndef CLIENT_H
#define CLIENT_H

namespace client
{
    /**
     * Signals the client thread to stop execution.
     */
    void stop();

    /**
     * Runs a TCP client on the designated port.
     *
     * @param port The port to run the client on.
     */
    void run(unsigned long port);

}

#endif // CLIENT_H