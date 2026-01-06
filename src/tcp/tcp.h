#ifndef SERVER_H
#define SERVER_H

#include "../packet/packet.h"

namespace server
{
    /**
     * Signals the server thread to stop execution.
     */
    void stop();

    /**
     * Sends a packet to module id.
     *
     * @param id The module id.
     * @param packet The packet to send.
     */
    void send(int id, packet::Packet packet);

    /**
     * Runs a TCP server on the designated port.
     *
     * @param port The port to run the server on.
     */
    void run(unsigned int port);

}

#endif // SERVER_H