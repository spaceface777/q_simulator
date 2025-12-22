#ifndef MODULE_H
#define MODULE_H

#include <pthread.h>
#include <cstdint>
#include <filesystem>
#include "../packet/packet.h"

namespace module
{
    class Module
    {
    private:
        uint8_t _id;
        pid_t _pid = -1;
        bool _running = false;

    public:
        Module(uint8_t id)
        {
            _id = id;
        }

        /**
         * @return The id of this module.
         */
        uint8_t id() const
        {
            return _id;
        }

        /**
         * @return Whether this module's executable is running.
         */
        bool is_running() const
        {
            return _running;
        }

        /**
         * Run this module as the executable found at path.
         *
         * @param path The file to execute.
         */
        void run(std::filesystem::path path);

        /**
         * Sends a packet to this module.
         *
         * @param packet The packet to send.
         */
        void send(packet::Packet packet);
    };
}

#endif // MODULE_H