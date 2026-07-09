#ifndef EVENTS_H
#define EVENTS_H

#include <list>
#include <cstdint>
#include <memory>
#include <string_view>
#include "../synchronization/virtual_clock.h"

namespace events
{

    /**
     * Generic event class.
     */
    class Event
    {
    protected:
        unsigned int _time;
        unsigned int id_;

    public:
        virtual void fire() const = 0;

        /**
         * Returns the arrival time of the object.
         */
        unsigned int time() const noexcept
        {
            return _time;
        }

        /**
         * Returns the id of the object.
         */
        unsigned int id() const noexcept
        {
            return id_;
        }

        bool operator==(const Event& other) const {
            return this->id() == other.id(); 
        }

        virtual ~Event() = default;
    };

    /**
     * Contains the data required for a plane.
     */
    class PlaneEvent : public Event
    {
        uint8_t module_;
        unsigned int leaves_in_;

    public:
        PlaneEvent(uint8_t time, uint8_t id, uint8_t module, unsigned int leaves_in)
        {
            _time = time;
            id_ = id;
            module_ = module;
            leaves_in_ = leaves_in;
        }

        /**
         * Returns the module that this plane is at.
         */
        uint8_t module() const noexcept
        {
            return module_;
        }

        /**
         * The time until leaving for this plane.
         */
        unsigned int leaves_in() const noexcept
        {
            return leaves_in_;
        }

        virtual void fire() const override;
    };

    enum LocationType
    {
        MODULE,
        PLANE
    };

    struct TubData
    {
        LocationType type;
        int id;
    };

    /**
     * Contains the data required for a tub.
     */
    class TubEvent : public Event
    {
        bool safe_;
        TubData _origin;
        TubData _destination;

    public:
        TubEvent(unsigned int time, uint8_t id, bool safe, TubData origin, TubData destination)
        {
            _time = time;
            id_ = id;
            safe_ = safe;
            _origin = origin;
            _destination = destination;
        }

        /**
         * Returns whether this tub is safe.
         */
        bool safe() const noexcept
        {
            return safe_;
        }

        /**
         * Returns the origin information for this tub.
         */
        TubData origin() const noexcept
        {
            return _origin;
        }

        /**
         * Returns the destination information for this tub.
         */
        TubData destination() const noexcept
        {
            return _destination;
        }

        virtual void fire() const override;
    };

    /**
     * Checks which events to fire based on the time and list of remaining events.
     * @param vclock The current clock.
     * @param events The remaining events.
     */
    void fire_events(virtual_clock::VirtualClock &vclock, std::list<std::unique_ptr<events::Event>> &events);

};

#endif // EVENTS_H
