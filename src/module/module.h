#ifndef MODULE_H
#define MODULE_H

#include <pthread.h>
#include <cstdint>

class Module
{
private:
    uint8_t _id;
    pid_t pid;

public:
    /**
     * @return The id of this module.
     */
    uint8_t id() {
        return _id;
    }

    void run();
};

#endif // MODULE_H