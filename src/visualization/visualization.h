// Visualization code written by Ali, adapted to code standards

#ifndef VISUALIZATION_H
#define VISUALIZATION_H

#include <SDL.h>
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

    enum class GlyphId : uint8_t
    {
        ZERO,
        ONE,
        TWO,
        THREE,
        FOUR,
        FIVE,
        SIX,
        SEVEN,
        EIGHT,
        NINE,
        DOT,
        MINUS,
        X,
        LEFT,
        RIGHT,
        PAUSE,
        COUNT
    };

    void draw_char(SDL_Renderer *renderer, int x, int y, GlyphId glyph, int scale);
}

#endif
