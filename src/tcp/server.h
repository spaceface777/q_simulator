#ifndef SERVER_H
#define SERVER_H

#include <unordered_map>

#include "../message/message.h"
#include "../module/types.h"

namespace server
{

    static constexpr int PORT = 7777;

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
     * @return The amount of connections this server is handling.
     */
    [[nodiscard]] std::size_t size();

    void check();

#ifdef TESTING
    [[nodiscard]] std::unordered_map<uint8_t, int> get_module_fds();
#endif

}

#endif // SERVER_H