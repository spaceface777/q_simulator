#ifndef SERVER_H
#define SERVER_H

#include "../message/message.h"
#include "../module/types.h"

namespace server
{

    /**
     * Sends a message to module id.
     *
     * @param id The module id.
     * @param message The message to send.
     */
    void send(module::ModuleId id, const message::Message &message);

    /**
     * Runs the server.
     */
    void run();

    /**
     * Signals the server thread to stop execution.
     */
    void stop();

    /**
     * Waits for the server thread to finish. Call stop() first.
     */
    void join();

    /**
     * @return The amount of connections this server is handling.
     */
    std::size_t size();

    void check();

}

#endif // SERVER_H