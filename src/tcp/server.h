#ifndef SERVER_H
#define SERVER_H

#include "../message/message.h"

namespace server
{

    /**
     * Sends a message to module id.
     *
     * @param id The module id.
     * @param message The message to send.
     */
    void send(unsigned int id, const message::Message &message);

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

    void check();

}

#endif // SERVER_H