#include "buttons.h"
#include "../defs.h"
#include "../settings.h"
#include "../synchronization/virtual_clock.h"
#include <chrono>

namespace visualization::buttons
{

    void Button::handle(const SDL_Event &event, virtual_clock::ISimulationClock &vclock) noexcept
    {
        if (event.type == SDL_MOUSEMOTION ||
            event.type == SDL_MOUSEBUTTONDOWN ||
            event.type == SDL_MOUSEBUTTONUP)
        {
            int mx, my;
            SDL_GetMouseState(&mx, &my);

            bool inside = true;
            if (mx < projected_.x)
                inside = false; // Left of button
            else if (mx > projected_.x + projected_.w)
                inside = false; // Right of button
            else if (my < projected_.y)
                inside = false; // Above button
            else if (my > projected_.y + projected_.h)
                inside = false; // Below button

            if (inside)
            {
                hovering_ = true;

                if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
                {
                    pressing_ = true;
                }
                else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
                {
                    if (pressing_)
                    {
                        on_click(vclock);
                    }
                    pressing_ = false;
                }
            }
            else
            {
                hovering_ = false;
                pressing_ = false;
            }
        }
    }

    void Button::render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_Color current = base_color_;

        if (pressing_)
        {
            current = active_color_;
        }
        else if (hovering_)
        {
            current = hover_color_;
        }

        project_(renderer);
        SDL_SetRenderDrawColor(renderer, current.r, current.g, current.b, current.a);
        SDL_RenderFillRect(renderer, &projected_);

        on_render(renderer, vclock);
    }

    void PauseButton::on_click(virtual_clock::ISimulationClock &vclock) noexcept
    {
        if (settings::get_setting<double>("simulation_speed") == 0)
        {
            settings::set_setting("simulation_speed", pre_pause_simulation_speed_);
            vclock.set_simulation_speed(pre_pause_simulation_speed_);
            pre_pause_simulation_speed_ = 0;
        }
        else
        {
            pre_pause_simulation_speed_ = settings::get_setting<double>("simulation_speed");
            settings::set_setting("simulation_speed", 0.0);
            vclock.set_simulation_speed(0.0);
        }
    }

    static constexpr int SCALE = 3;
    static constexpr int X_OFFSET = 15;
    static constexpr int Y_OFFSET = 11;
    static constexpr double LARGE_STEP = 1.0;
    static constexpr double SMALL_STEP = 0.1;

    void PauseButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        if (settings::get_setting<double>("simulation_speed") == 0)
            visualization::draw_char(renderer, projected_.x + X_OFFSET, projected_.y + Y_OFFSET, GlyphId::RIGHT, SCALE);
        else
            visualization::draw_char(renderer, projected_.x + X_OFFSET, projected_.y + Y_OFFSET, GlyphId::PAUSE, SCALE);
    }

    void SlowSlowerButton::on_click(virtual_clock::ISimulationClock &vclock) noexcept
    {
        double new_value = fmax(0.0, settings::get_setting<double>("simulation_speed") - LARGE_STEP);
        settings::set_setting("simulation_speed", new_value);
        vclock.set_simulation_speed(new_value);
    }

    void SlowSlowerButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        visualization::draw_char(renderer, projected_.x + 8, projected_.y + Y_OFFSET, GlyphId::LEFT, SCALE);
        visualization::draw_char(renderer, projected_.x + 8 + 4 * SCALE, projected_.y + Y_OFFSET, GlyphId::LEFT, SCALE);
    }

    void SlowerButton::on_click(virtual_clock::ISimulationClock &vclock) noexcept
    {
        double new_value = fmax(0.0, settings::get_setting<double>("simulation_speed") - SMALL_STEP);
        settings::set_setting("simulation_speed", new_value);
        vclock.set_simulation_speed(new_value);
    }

    void SlowerButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        visualization::draw_char(renderer, projected_.x + X_OFFSET, projected_.y + Y_OFFSET, GlyphId::LEFT, SCALE);
    }

    void FastFasterButton::on_click(virtual_clock::ISimulationClock &vclock) noexcept
    {
        double new_value = fmin(5.0, settings::get_setting<double>("simulation_speed") + LARGE_STEP);
        settings::set_setting("simulation_speed", new_value);
        vclock.set_simulation_speed(new_value);
    }

    void FastFasterButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        visualization::draw_char(renderer, projected_.x + 8, projected_.y + Y_OFFSET, GlyphId::RIGHT, SCALE);
        visualization::draw_char(renderer, projected_.x + 8 + 4 * SCALE, projected_.y + Y_OFFSET, GlyphId::RIGHT, SCALE);
    }

    void FasterButton::on_click(virtual_clock::ISimulationClock &vclock) noexcept
    {
        double new_value = fmin(5.0, settings::get_setting<double>("simulation_speed") + SMALL_STEP);
        settings::set_setting("simulation_speed", new_value);
        vclock.set_simulation_speed(new_value);
    }

    void FasterButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        visualization::draw_char(renderer, projected_.x + X_OFFSET, projected_.y + Y_OFFSET, GlyphId::RIGHT, SCALE);
    }

    void SpeedButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << settings::get_setting<double>("simulation_speed") << "x";
        visualization::draw_string(renderer, projected_.x, projected_.y + 12, ss.str(), 3);
    }

    void TimeButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << (vclock.get_virtual_time().count() / 1000.0);
        visualization::draw_string(renderer, projected_.x, projected_.y + 12, ss.str(), 3);
    }

    void ZoomInButton::on_click([[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        double new_value = fmin(5.0, settings::get_setting<double>("scale") + SMALL_STEP);
        settings::set_setting("scale", new_value);
    }

    void ZoomInButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        visualization::draw_char(renderer, projected_.x + X_OFFSET, projected_.y + Y_OFFSET, GlyphId::PLUS, SCALE);
    }

    void ZoomOutButton::on_click([[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        double new_value = fmax(0.0, settings::get_setting<double>("scale") - SMALL_STEP);
        settings::set_setting("scale", new_value);
    }

    void ZoomOutButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        visualization::draw_char(renderer, projected_.x + X_OFFSET, projected_.y + Y_OFFSET, GlyphId::MINUS, SCALE);
    }

    void ZoomButton::on_render(SDL_Renderer *renderer, [[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << settings::get_setting<double>("scale") << "x";
        visualization::draw_string(renderer, projected_.x - 3, projected_.y + 12, ss.str(), 3);
    }

}
