#ifndef BUTTONS_H
#define BUTTONS_H

#include <SDL.h>
#include <atomic>
#include <memory>
#include <vector>

#include "../synchronization/virtual_clock.h"

namespace visualization::buttons
{

    enum ScreenAnchor
    {
        TOP_LEFT,
        TOP_RIGHT,
        BOTTOM_LEFT,
        BOTTOM_RIGHT
    };

    class Button
    {
    private:
        ScreenAnchor anchor_;

        SDL_Rect rect_;
        SDL_Rect projected_;
        SDL_Color base_color_;
        SDL_Color hover_color_;
        SDL_Color active_color_;

        bool hovering_ = false;
        bool pressing_ = false;

        void project_(SDL_Renderer *renderer) noexcept
        {

            int w = 0;
            int h = 0;
            if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0)
            {
                return;
            }

            int render_x, render_y;
            switch (anchor_)
            {
            case TOP_LEFT:
                render_x = rect_.x;
                render_y = rect_.y;
                break;
            case TOP_RIGHT:
                render_x = w - rect_.x - rect_.w;
                render_y = rect_.y;
                break;
            case BOTTOM_LEFT:
                render_x = rect_.x;
                render_y = h - rect_.y - rect_.h;
                break;
            case BOTTOM_RIGHT:
                render_x = w - rect_.x;
                render_y = h - rect_.y;
                break;
            }

            projected_ = {render_x, render_y, rect_.w, rect_.h};
        }

    public:
        Button(ScreenAnchor anchor, int x, int y, int w, int h,
               SDL_Color base_color, SDL_Color hover_color, SDL_Color active_color)
            : anchor_(anchor), rect_{x, y, w, h},
              base_color_(base_color), hover_color_(hover_color), active_color_(active_color)
        {
        }

        virtual void on_click([[maybe_unused]] virtual_clock::VirtualClock &vclock) noexcept {};
        virtual void on_render([[maybe_unused]] SDL_Renderer *renderer) noexcept {};

        void handle(const SDL_Event &event, virtual_clock::VirtualClock &vclock) noexcept;
        void render(SDL_Renderer *renderer) noexcept;

        virtual ~Button() = default;
    };

    class PauseButton : public Button
    {
    private:
        double pre_pause_simulation_speed_ = 0;

    public:
        PauseButton()
            : Button(ScreenAnchor::BOTTOM_LEFT, 20, 20, 40, 40,
                     SDL_Color{200, 200, 200, 255},
                     SDL_Color{230, 230, 230, 255},
                     SDL_Color{150, 150, 150, 255})
        {
        }

        virtual void on_click(virtual_clock::VirtualClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer) noexcept override;
    };
}

#endif