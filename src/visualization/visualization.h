// Visualization code written by Ali, adapted to code standards

#ifndef VISUALIZATION_H
#define VISUALIZATION_H

#include <SDL3/SDL.h>

#include "../platform/platform.h"
#include "../synchronization/virtual_clock.h"
#include "../world/world.h"

namespace visualization {

	void init();

	void update(const world::State& world, const platform::Manager& platform, virtual_clock::ISimulationClock& vclock);

	// Signals the visualization loop to stop.
	void stop();

	[[nodiscard]] bool is_running();

	enum class GlyphId : uint8_t {
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
		D,
		B,
		G,
		DOT,
		MINUS,
		PLUS,
		X,
		LEFT,
		RIGHT,
		PAUSE,
		COUNT
	};

	void draw_char(SDL_Renderer* renderer, int x, int y, GlyphId glyph, int scale);
	void draw_string(SDL_Renderer* renderer, int x, int y, std::string str, int scale);
} // namespace visualization

#endif
