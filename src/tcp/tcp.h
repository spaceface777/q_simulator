#ifndef SERVER_H
#define SERVER_H

namespace server
{
    /**
     * Signals the server thread to stop execution.
     */
    void stop();

    /**
     * Runs a TCP server on the designated port.
     *
     * @param port The port to run the server on.
     */
    void run(unsigned int port);

}

#endif // SERVER_H