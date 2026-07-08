// Visualization code written by Ali, adapted to code standards

#ifndef VISUALIZATION_H
#define VISUALIZATION_H

#include "../synchronization/virtual_clock.h"

namespace visualization
{

    /**
     * Initializes the visualization.
     */
    void init();

    /**
     * Updates the visualization.
     */
    void update(virtual_clock::VirtualClock &vclock);

    /**
     * Signals the visualization loop to stop.
     */
    void stop();

    /**
     * @return Whether the visualization is still running or whether it has been closed/finished.
     */
    bool is_running();

    void rethrow();

}

#endif
