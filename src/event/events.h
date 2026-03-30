#ifndef EVENTS_H
#define EVENTS_H

#include <cstdint>
#include <string_view>

namespace events
{
    /**
     * Generic event class.
     */
    class Event
    {
    protected:
        uint8_t _time;
        uint8_t _id;

    public:
        virtual ~Event() {}

        /**
         * Returns the arrival time of the object.
         */
        uint8_t time() const noexcept
        {
            return _time;
        }

        /**
         * Returns the id of the object.
         */
        uint8_t id() const noexcept
        {
            return _id;
        }
    };

    /**
     * Contains the data required for a plane.
     */
    class PlaneEvent : public Event
    {
        uint8_t _module;
        uint8_t _leaves_in;

    public:
        PlaneEvent(uint8_t time, uint8_t id, uint8_t module, uint8_t leaves_in)
        {
            _time = time;
            _id = id;
            _module = module;
            _leaves_in = leaves_in;
        }

        /**
         * Returns the module that this plane is at.
         */
        uint8_t module() const noexcept
        {
            return _module;
        }

        /**
         * The time until leaving for this plane.
         */
        uint8_t leaves_in() const noexcept
        {
            return _leaves_in;
        }
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
        bool _safe;
        TubData _origin;
        TubData _destination;

    public:
        TubEvent(uint8_t time, uint8_t id, bool safe, TubData origin, TubData destination)
        {
            _time = time;
            _id = id;
            _safe = safe;
            _origin = origin;
            _destination = destination;
        }

        /**
         * Returns whether this tub is safe.
         */
        bool safe() const noexcept
        {
            return _safe;
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
    };
};

#endif // EVENTS_H
