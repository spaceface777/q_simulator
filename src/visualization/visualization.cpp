// Visualization code written by Ali, adapted to code standards

#include "visualization.h"

#include <atomic>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../defs.h"
#include "../module/module.h"
#include "../module/types.h"
#include "../settings.h"
#include "../synchronization/virtual_clock.h"
#include "buttons.h"

namespace visualization {

	static constexpr float SCALING_FACTOR = 0.7;

	static constexpr float BELT_FACTOR = 0.4;

	static float scale = 1.0;

	// Scales a constant by the current scale setting.
	float scale_within_module(float x) {
		return x * SCALING_FACTOR * scale;
	}

	static std::atomic<bool> running_;

	static SDL_Window*   window_   = nullptr;
	static SDL_Renderer* renderer_ = nullptr;

	static double belt_offset_ = 0.0;

	struct Glyph {
		GlyphId  id;
		uint16_t bitmap;
	};

	class PixelFont {
	public:
		static constexpr uint8_t WIDTH  = 3;
		static constexpr uint8_t HEIGHT = 5;

		static constexpr std::array<Glyph, static_cast<size_t>(GlyphId::COUNT)> DATA = {
			{{GlyphId::ZERO, 0x7B6F},  // 111, 101, 101, 101, 111
			 {GlyphId::ONE, 0x2492},   // 010, 010, 010, 010, 010
			 {GlyphId::TWO, 0x73E7},   // 111, 001, 111, 100, 111
			 {GlyphId::THREE, 0x73CF}, // 111, 001, 111, 001, 111
			 {GlyphId::FOUR, 0x5BC9},  // 101, 101, 111, 001, 001
			 {GlyphId::FIVE, 0x79CF},  // 111, 100, 111, 001, 111
			 {GlyphId::SIX, 0x79EF},   // 111, 100, 111, 101, 111
			 {GlyphId::SEVEN, 0x7292}, // 111, 001, 010, 010, 010
			 {GlyphId::EIGHT, 0x7BEF}, // 111, 101, 111, 101, 111
			 {GlyphId::NINE, 0x7BCF},  // 111, 101, 111, 001, 111
			 {GlyphId::D, 0b110101101101110},
			 {GlyphId::B, 0b110101110101110},
			 {GlyphId::G, 0b111100101101111},
			 {GlyphId::Q, 0b111101101111001},
			 {GlyphId::S, 0b111100111001111},
			 {GlyphId::E, 0b111100111100111},
			 {GlyphId::C, 0b111100100100111},
			 {GlyphId::T, 0b111010010010010},
			 {GlyphId::R, 0b110101110101101},
			 {GlyphId::DOT, 0b000000000000010},
			 {GlyphId::MINUS, 0b000000111000000},
			 {GlyphId::PLUS, 0b000010111010000},
			 {GlyphId::X, 0b000101010101000},
			 {GlyphId::LEFT, 0b001010100010001},
			 {GlyphId::RIGHT, 0b100010001010100},
			 {GlyphId::PAUSE, 0b101101101101101}}};

		static constexpr uint16_t get_bitmap(GlyphId id) {
			size_t index = static_cast<size_t>(id);
			if (index < DATA.size()) return DATA[index].bitmap;
			throw std::invalid_argument("Invalid glyph id");
		}

		static constexpr uint16_t get_bitmap_from_char(char c) {
			if (c >= '0' && c <= '9') return get_bitmap(static_cast<GlyphId>(c - '0'));
			else if (c == 'd') return get_bitmap(GlyphId::D);
			else if (c == 'b') return get_bitmap(GlyphId::B);
			else if (c == 'g') return get_bitmap(GlyphId::G);
			else if (c == 'x') return get_bitmap(GlyphId::X);
			else if (c == 'q') return get_bitmap(GlyphId::Q);
			else if (c == 's') return get_bitmap(GlyphId::S);
			else if (c == 'e') return get_bitmap(GlyphId::E);
			else if (c == 'c') return get_bitmap(GlyphId::C);
			else if (c == 't') return get_bitmap(GlyphId::T);
			else if (c == 'r') return get_bitmap(GlyphId::R);
			else if (c == '.') return get_bitmap(GlyphId::DOT);
			else if (c == '-') return get_bitmap(GlyphId::MINUS);
			else if (c == '+') return get_bitmap(GlyphId::PLUS);
			else if (c == '<') return get_bitmap(GlyphId::LEFT);
			else if (c == '>') return get_bitmap(GlyphId::RIGHT);

			throw std::invalid_argument("Invalid char");
		}
	};

	void draw_char(SDL_Renderer* renderer, int x, int y, char digit, int scale) {
		uint16_t bits = PixelFont::get_bitmap_from_char(digit);
		for (int row = 0; row < PixelFont::HEIGHT; ++row) {
			for (int col = 0; col < PixelFont::WIDTH; ++col) {
				if (bits & (1 << (14 - (row * PixelFont::WIDTH + col)))) {
					const SDL_FRect r = {static_cast<float>(x + col * scale), static_cast<float>(y + row * scale),
										 static_cast<float>(scale), static_cast<float>(scale)};
					SDL_RenderFillRect(renderer, &r);
				}
			}
		}
	}

	void draw_char(SDL_Renderer* renderer, int x, int y, GlyphId glyph, int scale) {
		uint16_t bits = PixelFont::get_bitmap(glyph);
		for (int row = 0; row < PixelFont::HEIGHT; ++row) {
			for (int col = 0; col < PixelFont::WIDTH; ++col) {
				if (bits & (1 << (14 - (row * PixelFont::WIDTH + col)))) {
					const SDL_FRect r = {static_cast<float>(x + col * scale), static_cast<float>(y + row * scale),
										 static_cast<float>(scale), static_cast<float>(scale)};
					SDL_RenderFillRect(renderer, &r);
				}
			}
		}
	}

	void draw_string(SDL_Renderer* renderer, int x, int y, std::string str, int scale) {
		for (size_t i = 0; i < str.length(); ++i) {
			draw_char(renderer, x + i * 4 * scale, y, str[i], scale);
		}
	}

	static void draw_number(SDL_Renderer* renderer, int x, int y, int number, int scale) {
		draw_string(renderer, x, y, std::to_string(number), scale);
	}

	static void draw_number(SDL_Renderer* renderer, int x, int y, double number, int scale) {
		std::stringstream ss;
		ss << std::fixed << std::setprecision(1) << number;
		draw_string(renderer, x, y, ss.str(), scale);
	}

	static void get_module_coords(module::ModuleId id, float& x, float& y) {
		const float                    margin   = scale_within_module(20);
		const settings::ModuleSettings settings = settings::get_module_settings(id);

		x = margin + settings.x * (scale_within_module(180) + margin);
		y = margin + settings.y * (scale_within_module(180) + margin);
	}

	static void render_led(int module_x, int module_y, module::LedColor color, settings::ModuleAngle rotation) {
		int led_rel_x, led_rel_y;
		int offset = scale_within_module(20);
		switch (rotation) {
		case settings::ModuleAngle::NORMAL:
			led_rel_x = scale_within_module(90) + offset;
			led_rel_y = scale_within_module(90) + offset;
			break;
		case settings::ModuleAngle::ROTATED_90:
			led_rel_x = scale_within_module(90) - offset;
			led_rel_y = scale_within_module(90) + offset;
			break;
		case settings::ModuleAngle::UPSIDE_DOWN:
			led_rel_x = scale_within_module(90) - offset;
			led_rel_y = scale_within_module(90) - offset;
			break;
		case settings::ModuleAngle::ROTATED_270:
			led_rel_x = scale_within_module(90) + offset;
			led_rel_y = scale_within_module(90) - offset;
			break;
		default: return;
		}

		int x = module_x + led_rel_x;
		int y = module_y + led_rel_y;

		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(renderer_, color.r(), color.g(), color.b(), color.a() == 0 ? 255 : color.a());

		int radius = 6;
		for (int w = -radius; w <= radius; w++) {
			for (int h = -radius; h <= radius; h++) {
				if (w * w + h * h <= radius * radius) {
					SDL_RenderPoint(renderer_, x + w, y + h);
				}
			}
		}

		if (settings::get_setting<bool>("debug")) {
			draw_number(renderer_, x + 5, y + 10, color.r(), 2);
			draw_number(renderer_, x + 5, y + 25, color.g(), 2);
			draw_number(renderer_, x + 5, y + 40, color.b(), 2);
		}

		SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 150);
		SDL_RenderPoint(renderer_, x - 2, y - 2);
	}

	static void render_rfid(int module_x, int module_y, int rfid_state, const world::Calibration& calibration,
							settings::ModuleAngle rotation) {
		float        rfid_rel_x, rfid_rel_y;
		const double progress  = static_cast<double>(calibration.hardware.rfid_position_um) /
								 static_cast<double>(calibration.endpoint_to_junction_um);
		const float  half_size = scale_within_module(75.0 * world::RFID_HALF_SMALL_BELT_FRACTION);
		switch (rotation) {
		case settings::ModuleAngle::NORMAL:
			rfid_rel_x = scale_within_module(90);
			rfid_rel_y = scale_within_module(105 + 75 * progress);
			break;
		case settings::ModuleAngle::ROTATED_90:
			rfid_rel_x = scale_within_module(75 - 75 * progress);
			rfid_rel_y = scale_within_module(90);
			break;
		case settings::ModuleAngle::UPSIDE_DOWN:
			rfid_rel_x = scale_within_module(90);
			rfid_rel_y = scale_within_module(75 - 75 * progress);
			break;
		case settings::ModuleAngle::ROTATED_270:
			rfid_rel_x = scale_within_module(105 + 75 * progress);
			rfid_rel_y = scale_within_module(90);
			break;
		default: return;
		}

		SDL_FRect rfid_rect = {module_x + rfid_rel_x - half_size, module_y + rfid_rel_y - half_size, 2 * half_size,
							   2 * half_size};
		// The physical reader remains visible when idle. An active tag alternates red and grey so the
		// existing blink indication does not hide where the randomized reader actually is.
		const bool active_blink = rfid_state != 0 && (SDL_GetTicks() / 500) % 2 != 0;
		if (active_blink) {
			SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255);
		} else {
			SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
		}
		SDL_RenderFillRect(renderer_, &rfid_rect);
	}

	static void render_big_belt(int module_x, int module_y, module::BeltSpeed speed, bool is_running,
								settings::ModuleAngle rotation) {
		float width, height, belt_x, belt_y;

		switch (rotation) {
		case settings::ModuleAngle::NORMAL:
		case settings::ModuleAngle::UPSIDE_DOWN:
			width  = scale_within_module(180);
			height = scale_within_module(30);
			belt_x = module_x;
			belt_y = module_y + scale_within_module(75);
			break;
		case settings::ModuleAngle::ROTATED_90:
		case settings::ModuleAngle::ROTATED_270:
			width  = scale_within_module(30);
			height = scale_within_module(180);
			belt_x = module_x + scale_within_module(75);
			belt_y = module_y;
			break;
		default: return;
		};

		SDL_FRect belt = {belt_x, belt_y, width, height};

		if (is_running) {
			SDL_SetRenderDrawColor(renderer_, 34, 139, 34, 255);
		} else {
			SDL_SetRenderDrawColor(renderer_, 40, 40, 40, 255);
		}
		SDL_RenderFillRect(renderer_, &belt);

		if (is_running) {
			SDL_SetRenderDrawColor(renderer_, 50, 205, 50, 255);
		} else {
			SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
		}
		SDL_RenderRect(renderer_, &belt);

		if (is_running && speed.raw() != 0) {
			if (settings::get_setting<bool>("debug")) draw_number(renderer_, belt_x, belt_y, speed.raw(), 2);

			SDL_SetRenderDrawColor(renderer_, 144, 238, 144, 150);
			double step             = 20.0;
			double offset           = std::fmod(belt_offset_ * (-speed.raw() / 100.0), step);
			int    longer_dimension = height > width ? height : width;
			for (double l = -step; l < longer_dimension + step; l += step) {
				switch (rotation) {
				case settings::ModuleAngle::NORMAL:
				case settings::ModuleAngle::UPSIDE_DOWN: {
					float draw_x =
						belt_x + (rotation == settings::ModuleAngle::NORMAL ? (l + offset) : width - (l + offset));
					if (draw_x >= belt_x && draw_x < belt_x + width) {
						SDL_RenderLine(renderer_, draw_x, belt_y, draw_x, belt_y + height - 1);
					};
					break;
				}
				case settings::ModuleAngle::ROTATED_90:
				case settings::ModuleAngle::ROTATED_270: {
					float draw_y =
						belt_y + (rotation == settings::ModuleAngle::ROTATED_90 ? (l + offset) : height - (l + offset));
					if (draw_y >= belt_y && draw_y < belt_y + height) {
						SDL_RenderLine(renderer_, belt_x, draw_y, belt_x + width - 1, draw_y);
					};
					break;
				}
				}
			}
		}
	}

	static void render_small_belt(float module_x, float module_y, module::BeltSpeed speed, bool is_running,
								  settings::ModuleAngle rotation) {
		float width, height, belt_x, belt_y;

		switch (rotation) {
		case settings::ModuleAngle::NORMAL:
			belt_x = module_x + scale_within_module(75);
			belt_y = module_y + scale_within_module(105);
			width  = scale_within_module(30);
			height = scale_within_module(75);
			break;
		case settings::ModuleAngle::ROTATED_90:
			belt_x = module_x;
			belt_y = module_y + scale_within_module(75);
			width  = scale_within_module(75);
			height = scale_within_module(30);
			break;
		case settings::ModuleAngle::UPSIDE_DOWN:
			belt_x = module_x + scale_within_module(75);
			belt_y = module_y;
			width  = scale_within_module(30);
			height = scale_within_module(75);
			break;
		case settings::ModuleAngle::ROTATED_270:
			belt_x = module_x + scale_within_module(105);
			belt_y = module_y + scale_within_module(75);
			width  = scale_within_module(75);
			height = scale_within_module(30);
			break;
		default: return;
		}

		SDL_FRect belt = {belt_x, belt_y, width, height};

		if (is_running) {
			SDL_SetRenderDrawColor(renderer_, 34, 139, 34, 255);
		} else {
			SDL_SetRenderDrawColor(renderer_, 40, 40, 40, 255);
		}
		SDL_RenderFillRect(renderer_, &belt);

		if (is_running) {
			SDL_SetRenderDrawColor(renderer_, 50, 205, 50, 255);
		} else {
			SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
		}
		SDL_RenderRect(renderer_, &belt);

		// Animation lines
		if (is_running && speed.raw() != 0) {
			if (settings::get_setting<bool>("debug")) draw_number(renderer_, belt_x, belt_y, speed.raw(), 2);

			SDL_SetRenderDrawColor(renderer_, 144, 238, 144, 150);
			double step             = 20.0;
			double offset           = -std::fmod(belt_offset_ * (-speed.raw() / 100.0), step);
			int    longer_dimension = height > width ? height : width;
			for (double l = -step; l < longer_dimension + step; l += step) {
				switch (rotation) {
				case settings::ModuleAngle::NORMAL:
				case settings::ModuleAngle::UPSIDE_DOWN: {
					float draw_y =
						belt_y + (rotation == settings::ModuleAngle::NORMAL ? (l + offset) : height - (l + offset));
					if (draw_y >= belt_y && draw_y < belt_y + height) {
						SDL_RenderLine(renderer_, belt_x, draw_y, belt_x + width - 1, draw_y);
					}
					break;
				}
				case settings::ModuleAngle::ROTATED_90:
				case settings::ModuleAngle::ROTATED_270: {
					float draw_x =
						belt_x + (rotation == settings::ModuleAngle::ROTATED_270 ? (l + offset) : width - (l + offset));
					if (draw_x >= belt_x && draw_x < belt_x + width) {
						SDL_RenderLine(renderer_, draw_x, belt_y, draw_x, belt_y + height - 1);
					}
					break;
				}
				}
			}
		}
	}

	static void render_laser(float module_x, float module_y, bool power, int detection, int64_t position_um,
							 int64_t leg_length_um, settings::ModuleAngle rotation) {
		float       x1, y1, x2, y2;
		const float position =
			scale_within_module(90.0 * static_cast<double>(position_um) / static_cast<double>(leg_length_um));

		switch (rotation) {
		case settings::ModuleAngle::NORMAL:
			x1 = x2 = module_x + scale_within_module(180) - position;
			y1      = module_y + scale_within_module(75);
			y2      = module_y + scale_within_module(105);
			break;
		case settings::ModuleAngle::ROTATED_90:
			x1 = module_x + scale_within_module(75);
			x2 = module_x + scale_within_module(105);
			y1 = y2 = module_y + scale_within_module(180) - position;
			break;
		case settings::ModuleAngle::UPSIDE_DOWN:
			x1 = x2 = module_x + position;
			y1      = module_y + scale_within_module(75);
			y2      = module_y + scale_within_module(105);
			break;
		case settings::ModuleAngle::ROTATED_270:
			x1 = module_x + scale_within_module(75);
			x2 = module_x + scale_within_module(105);
			y1 = y2 = module_y + position;
			break;
		default: return;
		}

		// The emitter and detector are physical hardware, so their dots remain visible while powered
		// down. The beam retains the previous red blink while a tub blocks it.
		const bool beam_visible = power && (detection == 0 || (SDL_GetTicks() / 100) % 2 != 0);
		if (beam_visible) {
			SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255);
			float     thickness = 2;
			SDL_FRect beam;
			if (x1 == x2) {
				beam = {x1 - thickness / 2, y1, thickness, y2 - y1};
			} else {
				beam = {x1, y1 - thickness / 2, x2 - x1, thickness};
			}
			SDL_RenderFillRect(renderer_, &beam);
		}

		SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
		SDL_FRect dot1 = {x1 - 2.f, y1 - 2.f, 4, 4};
		SDL_FRect dot2 = {x2 - 2.f, y2 - 2.f, 4, 4};
		SDL_RenderFillRect(renderer_, &dot1);
		SDL_RenderFillRect(renderer_, &dot2);
	}

	static void render_servo(float module_x, float module_y, module::ServoAngle angle, settings::ModuleAngle rotation) {
		// Pivot at (90, 75) relative to module (top-middle of big belt)
		double pivot_rel_x = scale_within_module(90.0);
		double pivot_rel_y = scale_within_module(75.0);

		// Base angle depends on rotation
		double base_angle_rad = 0.0;
		switch (rotation) {
		case settings::ModuleAngle::NORMAL: base_angle_rad = 0.0; break;
		case settings::ModuleAngle::ROTATED_90:
			base_angle_rad = M_PI / 2.0;
			pivot_rel_x    = scale_within_module(105.0);
			pivot_rel_y    = scale_within_module(90.0);
			break;
		case settings::ModuleAngle::UPSIDE_DOWN:
			base_angle_rad = M_PI;
			pivot_rel_x    = scale_within_module(90.0);
			pivot_rel_y    = scale_within_module(105.0);
			break;
		case settings::ModuleAngle::ROTATED_270:
			base_angle_rad = 3.0 * M_PI / 2.0;
			pivot_rel_x    = scale_within_module(75.0);
			pivot_rel_y    = scale_within_module(90.0);
			break;
		}

		// angle 0: parallel to big belt (right), 90: perpendicular (towards small belt)
		double servo_angle_rad = ((180.0 - angle.raw()) / 180.0) * M_PI;
		double total_angle_rad = base_angle_rad + servo_angle_rad;

		double length    = scale_within_module(35.0);
		double end_rel_x = pivot_rel_x + length * cos(total_angle_rad);
		double end_rel_y = pivot_rel_y + length * sin(total_angle_rad);

		SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
		float x1 = module_x + pivot_rel_x;
		float y1 = module_y + pivot_rel_y;
		float x2 = module_x + end_rel_x;
		float y2 = module_y + end_rel_y;

		// Draw thick line (width 4)
		for (int i = -2; i <= 1; ++i) {
			if (abs(x2 - x1) > abs(y2 - y1)) {
				SDL_RenderLine(renderer_, x1, y1 + i, x2, y2 + i);
			} else {
				SDL_RenderLine(renderer_, x1 + i, y1, x2 + i, y2);
			}
		}

		SDL_SetRenderDrawColor(renderer_, 50, 50, 50, 255);
		SDL_FRect pivot_rect = {x1 - 4.f, y1 - 4.f, 8, 8};
		SDL_RenderFillRect(renderer_, &pivot_rect);
		SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
		if (settings::get_setting<bool>("debug")) draw_number(renderer_, x1 - 12, y1 - 8, angle.raw(), 2);
	}

	struct RenderedTub {
		module::ModuleId module;
		world::Belt      belt;
		int64_t          position_um;
	};

	struct GhostTub {
		uint8_t     id;
		RenderedTub tub;
	};

	static std::map<uint8_t, RenderedTub>                         last_tubs_;
	static std::map<std::pair<uint8_t, ModuleLocation>, GhostTub> ghost_tubs_;

	static ModuleLocation tub_location(const RenderedTub& tub, int64_t leg_length_um) {
		if (tub.belt == world::Belt::SMALL) return ModuleLocation::MIDDLE;
		return tub.position_um >= leg_length_um ? ModuleLocation::LEFT : ModuleLocation::RIGHT;
	}

	static void render_tub(uint8_t id, const RenderedTub& tub, bool ghost) {
		const int64_t leg_length_um = world::calibration_for(tub.module).endpoint_to_junction_um;
		if (leg_length_um <= 0) return;

		// The renderer measures a tub's progress outward from the junction, not on the belt axis.
		const ModuleLocation location = tub_location(tub, leg_length_um);
		double               progress = 0.0;
		if (tub.belt == world::Belt::SMALL) {
			progress = static_cast<double>(tub.position_um) / static_cast<double>(leg_length_um);
		} else if (tub.position_um >= leg_length_um) {
			progress = static_cast<double>(tub.position_um - leg_length_um) / static_cast<double>(leg_length_um);
		} else {
			progress = static_cast<double>(leg_length_um - tub.position_um) / static_cast<double>(leg_length_um);
		}
		progress = std::clamp(progress, 0.0, 1.0);
		if (ghost) progress += 20.0 / 90.0;

		const settings::ModuleSettings module_settings = settings::get_module_settings(tub.module);

		float module_x, module_y;
		get_module_coords(tub.module, module_x, module_y);

		float rel_x = 0, rel_y = 0;
		if (location == ModuleLocation::LEFT) {
			switch (module_settings.angle) {
			case settings::ModuleAngle::NORMAL:
				rel_x = (1.0 - progress) * scale_within_module(90.0);
				rel_y = scale_within_module(90.0);
				break;
			case settings::ModuleAngle::ROTATED_90:
				rel_x = scale_within_module(90.0);
				rel_y = (1.0 - progress) * scale_within_module(90.0);
				break;
			case settings::ModuleAngle::UPSIDE_DOWN:
				rel_x = scale_within_module(90) + progress * scale_within_module(90.0);
				rel_y = scale_within_module(90.0);
				break;
			case settings::ModuleAngle::ROTATED_270:
				rel_x = scale_within_module(90.0);
				rel_y = scale_within_module(90) + progress * scale_within_module(90.0);
				break;

			default:
				throw std::invalid_argument(
					"Invalid module angle " +
					std::to_string(
						static_cast<std::underlying_type<settings::ModuleAngle>::type>(module_settings.angle)));
			}
		} else if (location == ModuleLocation::RIGHT) {
			switch (module_settings.angle) {
			case settings::ModuleAngle::NORMAL:
				rel_x = scale_within_module(90) + progress * scale_within_module(90.0);
				rel_y = scale_within_module(90.0);
				break;
			case settings::ModuleAngle::ROTATED_90:
				rel_x = scale_within_module(90.0);
				rel_y = scale_within_module(90) + progress * scale_within_module(90.0);
				break;
			case settings::ModuleAngle::UPSIDE_DOWN:
				rel_x = (1.0 - progress) * scale_within_module(90.0);
				rel_y = scale_within_module(90.0);
				break;
			case settings::ModuleAngle::ROTATED_270:
				rel_x = scale_within_module(90.0);
				rel_y = (1.0 - progress) * scale_within_module(90.0);
				break;

			default:
				throw std::invalid_argument(
					"Invalid module angle " +
					std::to_string(
						static_cast<std::underlying_type<settings::ModuleAngle>::type>(module_settings.angle)));
			}
		} else {
			switch (module_settings.angle) {
			case settings::ModuleAngle::NORMAL:
				rel_x = scale_within_module(90.0);
				rel_y = scale_within_module(105.0) + progress * scale_within_module(75.0);
				break;
			case settings::ModuleAngle::ROTATED_90:
				rel_x = scale_within_module(75.0) - progress * scale_within_module(75.0);
				rel_y = scale_within_module(90.0);
				break;
			case settings::ModuleAngle::UPSIDE_DOWN:
				rel_x = scale_within_module(90.0);
				rel_y = scale_within_module(75.0) - progress * scale_within_module(75.0);
				break;
			case settings::ModuleAngle::ROTATED_270:
				rel_x = scale_within_module(105.0) + progress * scale_within_module(75.0);
				rel_y = scale_within_module(90.0);
				break;

			default:
				throw std::invalid_argument(
					"Invalid module angle " +
					std::to_string(
						static_cast<std::underlying_type<settings::ModuleAngle>::type>(module_settings.angle)));
			}
		}

		const float x = module_x + rel_x;
		const float y = module_y + rel_y;

		if (ghost) SDL_SetRenderDrawColor(renderer_, 150, 150, 150, 255);
		else SDL_SetRenderDrawColor(renderer_, 255, 192, 203, 255);
		const int radius =
			static_cast<int>(std::lround(scale_within_module(180.0 * world::TUB_RADIUS_BIG_BELT_FRACTION)));
		for (int w = -radius; w <= radius; w++) {
			for (int h = -radius; h <= radius; h++) {
				if (w * w + h * h <= radius * radius) SDL_RenderPoint(renderer_, x + w, y + h);
			}
		}

		SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
		draw_number(renderer_, x - 4, y - 6, id, 2);
		if (settings::get_setting<bool>("debug")) draw_number(renderer_, x - 4, y + 5, progress, 1);
	}

	static void render_tubs(const world::State& world) {
		for (const auto& [id, tub] : last_tubs_) {
			if (!world.tubs().contains(id) && world.removal_reason(id) == world::TubRemovalReason::YEETED) {
				const int64_t leg_length_um = world::calibration_for(tub.module).endpoint_to_junction_um;
				ghost_tubs_.insert_or_assign(std::make_pair(tub.module.raw(), tub_location(tub, leg_length_um)),
											 GhostTub{id, tub});
			}
		}

		last_tubs_.clear();
		for (const auto& [id, tub] : world.tubs()) {
			last_tubs_.emplace(id, RenderedTub{tub.module, tub.belt, tub.position_um});
		}

		for (const auto& [endpoint, ghost] : ghost_tubs_) {
			(void)endpoint;
			render_tub(ghost.id, ghost.tub, true);
		}
		for (const auto& [id, tub] : last_tubs_) render_tub(id, tub, false);
	}

	static void render_module(const world::State& world, const platform::Manager& platform, module::ModuleId module_id,
							  float x, float y) {
		const module::Module* module = world.module(module_id);
		if (module == nullptr) {
			return;
		}

		const settings::ModuleSettings module_settings = settings::get_module_settings(module_id);

		SDL_FRect bg = {x, y, scale_within_module(180), scale_within_module(180)};
		SDL_SetRenderDrawColor(renderer_, 60, 60, 60, 255);
		SDL_RenderFillRect(renderer_, &bg);

		SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
		SDL_RenderRect(renderer_, &bg);

		SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
		draw_number(renderer_, x + 5, y + 5, module_id.raw(), 2);

		if (module_settings.type == "security") {
			draw_string(renderer_, x + 5, y + bg.h - 15, "sec", 2);
		} else if (module_settings.type == "storage") {
			draw_string(renderer_, x + 5, y + bg.h - 15, "str", 2);
		} else if (module_settings.type == "quarantine") {
			draw_string(renderer_, x + 5, y + bg.h - 15, "qr", 2);
		}

		const module::BeltSpeed big_belt_speed   = module->big_belt_speed();
		const module::BeltSpeed small_belt_speed = module->small_belt_speed();
		render_big_belt(x, y, big_belt_speed, big_belt_speed.raw() != 0, module_settings.angle);
		render_small_belt(x, y, small_belt_speed, small_belt_speed.raw() != 0, module_settings.angle);
		render_led(x, y, module->led_color(), module_settings.angle);
		const world::Calibration& calibration = world::calibration_for(module_id);
		render_rfid(x, y, module->is_rfid_active(), calibration, module_settings.angle);
		render_servo(x, y, module->servo_actual_angle(), module_settings.angle);
		render_laser(x, y, module->is_left_laser_enabled(), module->is_left_laser_blocked(),
					 calibration.hardware.left_laser_position_um, calibration.endpoint_to_junction_um,
					 module_settings.angle);
		render_laser(x, y, module->is_right_laser_enabled(), module->is_right_laser_blocked(),
					 calibration.hardware.right_laser_position_um, calibration.endpoint_to_junction_um,
					 module_settings.angle);

		if (platform.lifecycle(module_id) == platform::Lifecycle::STOPPED) {
			SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(renderer_, 180, 0, 0, 80);
			SDL_RenderFillRect(renderer_, &bg);
			SDL_SetRenderDrawColor(renderer_, 255, 64, 64, 255);
			const float far_x = x + bg.w;
			const float far_y = y + bg.h;
			for (int offset = -2; offset <= 2; offset++) {
				SDL_RenderLine(renderer_, x, y + offset, far_x, far_y + offset);
				SDL_RenderLine(renderer_, far_x, y + offset, x, far_y + offset);
			}
		}
	}

	void stop() {
		running_ = false;

		SDL_DestroyRenderer(renderer_);
		SDL_DestroyWindow(window_);
		SDL_Quit();
	}

	static std::vector<std::unique_ptr<buttons::Button>> buttons_;

	void init() {
		if (running_) return;
		running_ = true;
		last_tubs_.clear();
		ghost_tubs_.clear();

		if (!SDL_Init(SDL_INIT_VIDEO)) {
			std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
			running_ = false;
			return;
		}

		window_ = SDL_CreateWindow("Quercus Airport Visualization", 800, 800, SDL_WINDOW_RESIZABLE);
		if (!window_) {
			std::cerr << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
			SDL_Quit();
			running_ = false;
			return;
		}

		renderer_ = SDL_CreateRenderer(window_, nullptr);
		if (!renderer_) {
			std::cerr << "Renderer could not be created! SDL_Error: " << SDL_GetError() << std::endl;
			stop();
			return;
		}

		buttons_.push_back(std::make_unique<buttons::PauseButton>(buttons::PauseButton()));
		buttons_.push_back(std::make_unique<buttons::SlowerButton>(buttons::SlowerButton()));
		buttons_.push_back(std::make_unique<buttons::SlowSlowerButton>(buttons::SlowSlowerButton()));
		buttons_.push_back(std::make_unique<buttons::FasterButton>(buttons::FasterButton()));
		buttons_.push_back(std::make_unique<buttons::FastFasterButton>(buttons::FastFasterButton()));
		buttons_.push_back(std::make_unique<buttons::SpeedButton>(buttons::SpeedButton()));
		buttons_.push_back(std::make_unique<buttons::TimeButton>(buttons::TimeButton()));
		buttons_.push_back(std::make_unique<buttons::ZoomInButton>(buttons::ZoomInButton()));
		buttons_.push_back(std::make_unique<buttons::ZoomOutButton>(buttons::ZoomOutButton()));
		buttons_.push_back(std::make_unique<buttons::ZoomButton>(buttons::ZoomButton()));
		buttons_.push_back(std::make_unique<buttons::DbgButton>(buttons::DbgButton()));
	}

	static std::chrono::milliseconds last_time = std::chrono::milliseconds(0);

	void update(const world::State& world, const platform::Manager& platform, virtual_clock::ISimulationClock& vclock) {
		if (!running_) {
			return;
		}

		scale = settings::get_setting<double>("scale");

		SDL_Event event;
		while (SDL_PollEvent(&event) != 0) {
			if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
				stop();
				return;
			}

			if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
				const bool* keyboard = SDL_GetKeyboardState(nullptr);

				if (keyboard[SDL_SCANCODE_F4]) {
					settings::set_setting("debug", !settings::get_setting<bool>("debug"));
				}
				if (keyboard[SDL_SCANCODE_UP]) {
					double new_value = settings::get_setting<double>("simulation_speed") + 0.1;
					settings::set_setting("simulation_speed", new_value);
					vclock.set_simulation_speed(new_value);
				}
				if (keyboard[SDL_SCANCODE_DOWN]) {
					double new_value = fmax(0.0, settings::get_setting<double>("simulation_speed") - 0.1);
					settings::set_setting("simulation_speed", new_value);
					vclock.set_simulation_speed(new_value);
				}
				if (keyboard[SDL_SCANCODE_LCTRL] && keyboard[SDL_SCANCODE_MINUS]) {
					double new_value = fmax(0.0, settings::get_setting<double>("scale") - 0.1);
					settings::set_setting("scale", new_value);
				}
				if (keyboard[SDL_SCANCODE_LCTRL] && keyboard[SDL_SCANCODE_EQUALS]) {
					double new_value = fmin(5.0, settings::get_setting<double>("scale") + 0.1);
					settings::set_setting("scale", new_value);
				}
			}

			for (auto& button : buttons_) {
				button->handle(event, vclock);
			}
		}

		if (last_time.count() == 0) {
			last_time = vclock.get_virtual_time();
		}

		const std::chrono::milliseconds current_time = vclock.get_virtual_time();
		const std::chrono::milliseconds dt           = current_time - last_time;
		last_time                                    = current_time;

		SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 255);
		SDL_RenderClear(renderer_);

		for (uint8_t i = 1; i < MAX_MODULES; ++i) {
			const auto mid = module::ModuleId{i};
			if (!settings::get_module_settings(mid).active) continue;

			float x, y;
			get_module_coords(mid, x, y);
			render_module(world, platform, mid, x, y);
		}

		render_tubs(world);
		for (auto& button : buttons_) {
			button->render(renderer_, vclock);
		}
		belt_offset_ += (dt.count() / 16.0) * BELT_FACTOR;

		SDL_RenderPresent(renderer_);
	}

	bool is_running() {
		return running_;
	}
} // namespace visualization
