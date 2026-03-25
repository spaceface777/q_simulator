#ifndef SERVER_H
#define SERVER_H

#include "../packet/packet.h"

namespace server
{

    /**
     * Sends a packet to module id.
     *
     * @param id The module id.
     * @param packet The packet to send.
     */
    void send(unsigned int id, const packet::Packet &packet);

    /**
     * Runs the server.
     */
    void run();

    /**
     * Signals the server thread to stop execution.
     */
    void stop();

    /**
     * @return The amount of connections this server is handling.
     */
    std::size_t size();

    /**
     * Throws any exceptions that may have occurred during the execution of the thread.
     */
    void rethrow();

}

#endif // SERVER_H