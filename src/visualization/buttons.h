#ifndef BUTTONS_H
#define BUTTONS_H

#include <SDL.h>
#include <atomic>
#include <memory>
#include <vector>

#include "../synchronization/virtual_clock.h"
#include "visualization.h"

namespace visualization::buttons
{

    enum class ScreenAnchor
    {
        TOP_LEFT,
        TOP_RIGHT,
        BOTTOM_LEFT,
        BOTTOM_RIGHT
    };

    class Button
    {
    protected:
        ScreenAnchor anchor_;

        SDL_Rect rect_ = {0, 0, 0, 0};
        SDL_Rect projected_ = {0, 0, 0, 0};
        SDL_Color base_color_;
        SDL_Color hover_color_;
        SDL_Color active_color_;

    private:
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
            case ScreenAnchor::TOP_LEFT:
                render_x = rect_.x;
                render_y = rect_.y;
                break;
            case ScreenAnchor::TOP_RIGHT:
                render_x = w - rect_.x - rect_.w;
                render_y = rect_.y;
                break;
            case ScreenAnchor::BOTTOM_LEFT:
                render_x = rect_.x;
                render_y = h - rect_.y - rect_.h;
                break;
            case ScreenAnchor::BOTTOM_RIGHT:
                render_x = w - rect_.x - rect_.w;
                render_y = h - rect_.y - rect_.h;
                break;
            default:
                return;
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

        void handle(const SDL_Event &event, virtual_clock::ISimulationClock &vclock) noexcept;
        void render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept;

        virtual ~Button() = default;

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept = 0;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept = 0;
    };

    class PauseButton : public Button
    {
    private:
        double pre_pause_simulation_speed_ = 0;

    public:
        PauseButton()
            : Button(ScreenAnchor::BOTTOM_LEFT, 100, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{230, 230, 230, 120},
                     SDL_Color{150, 150, 150, 255})
        {
        }

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class SlowSlowerButton : public Button
    {

    public:
        SlowSlowerButton()
            : Button(ScreenAnchor::BOTTOM_LEFT, 20, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{230, 230, 230, 120},
                     SDL_Color{150, 150, 150, 255})
        {
        }

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class SlowerButton : public Button
    {

    public:
        SlowerButton()
            : Button(ScreenAnchor::BOTTOM_LEFT, 60, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{230, 230, 230, 120},
                     SDL_Color{150, 150, 150, 255})
        {
        }

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class FastFasterButton : public Button
    {

    public:
        FastFasterButton()
            : Button(ScreenAnchor::BOTTOM_LEFT, 180, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{230, 230, 230, 120},
                     SDL_Color{150, 150, 150, 255})
        {
        }

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class FasterButton : public Button
    {

    public:
        FasterButton()
            : Button(ScreenAnchor::BOTTOM_LEFT, 140, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{230, 230, 230, 120},
                     SDL_Color{150, 150, 150, 255})
        {
        }

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class SpeedButton : public Button
    {

    public:
        SpeedButton()
            : Button(ScreenAnchor::BOTTOM_LEFT, 230, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0})
        {
        }

    protected:
        virtual void on_click([[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept override {};
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class TimeButton : public Button
    {

    public:
        TimeButton()
            : Button(ScreenAnchor::BOTTOM_LEFT, 300, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0})
        {
        }

    protected:
        virtual void on_click([[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept override {};
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class ZoomInButton : public Button
    {

    public:
        ZoomInButton()
            : Button(ScreenAnchor::BOTTOM_RIGHT, 80, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{230, 230, 230, 120},
                     SDL_Color{150, 150, 150, 255})
        {
        }

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class ZoomOutButton : public Button
    {

    public:
        ZoomOutButton()
            : Button(ScreenAnchor::BOTTOM_RIGHT, 120, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{230, 230, 230, 120},
                     SDL_Color{150, 150, 150, 255})
        {
        }

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class ZoomButton : public Button
    {

    public:
        ZoomButton()
            : Button(ScreenAnchor::BOTTOM_RIGHT, 20, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0})
        {
        }

    protected:
        virtual void on_click([[maybe_unused]] virtual_clock::ISimulationClock &vclock) noexcept override {};
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };

    class DbgButton : public Button
    {

    public:
        DbgButton()
            : Button(ScreenAnchor::BOTTOM_RIGHT, 180, 20, 40, 40,
                     SDL_Color{0, 0, 0, 0},
                     SDL_Color{230, 230, 230, 120},
                     SDL_Color{150, 150, 150, 255})
        {
        }

    protected:
        virtual void on_click(virtual_clock::ISimulationClock &vclock) noexcept override;
        virtual void on_render(SDL_Renderer *renderer, virtual_clock::ISimulationClock &vclock) noexcept override;
    };
}

#endif