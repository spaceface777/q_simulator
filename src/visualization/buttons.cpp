#include "buttons.h"
#include "../defs.h"
#include "../settings.h"
#include "../synchronization/virtual_clock.h"

namespace visualization::buttons
{

    void Button::handle(const SDL_Event &event, virtual_clock::VirtualClock &vclock) noexcept
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

    void Button::render(SDL_Renderer *renderer) noexcept
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
    }

    void PauseButton::on_click(virtual_clock::VirtualClock &vclock) noexcept
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

    void PauseButton::on_render(SDL_Renderer *renderer) noexcept
    {
        if (settings::get_setting<double>("simulation_speed") == 0)
        {
        }
    }

}
