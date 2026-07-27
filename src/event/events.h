#ifndef EVENTS_H
#define EVENTS_H

#include <list>
#include <cstdint>
#include <memory>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <array>
#include "../synchronization/virtual_clock.h"
#include "../defs.h"
#include "../module/types.h"

namespace events
{

    namespace fs = std::filesystem;

    /**
     * Generic event class.
     */
    class Event
    {
    protected:
        double time_;
        unsigned int id_;

    public:
        virtual void fire() const = 0;

        /**
         * Returns the arrival time of the object.
         */
        [[nodiscard]] double time() const noexcept
        {
            return time_;
        }

        /**
         * Returns the id of the object.
         */
        [[nodiscard]] unsigned int id() const noexcept
        {
            return id_;
        }

        bool operator==(const Event &other) const
        {
            return this->id() == other.id();
        }

        virtual ~Event() = default;
    };

    /**
     * Contains the data required for a plane.
     */
    class PlaneEvent : public Event
    {
        module::ModuleId module_;
        unsigned int leaves_in_;
        std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid_;

    public:
        PlaneEvent(double time, uint8_t id, module::ModuleId module, unsigned int leaves_in,
                   std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid)
        {
            time_ = time;
            id_ = id;
            module_ = module;
            leaves_in_ = leaves_in;
            rfid_ = rfid;
        }

        /**
         * Returns the module that this plane is at.
         */
        module::ModuleId module() const noexcept
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

        std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid() const noexcept
        {
            return rfid_;
        }

        virtual void fire() const override;
    };

    enum class LocationType
    {
        MODULE,
        PLANE
    };

    struct TubData
    {
        LocationType type;
        module::ModuleId id;
    };

    /**
     * Contains the data required for a tub.
     */
    class TubEvent : public Event
    {
        TubData origin_;
        TubData destination_;
        bool safe_;
        bool needs_security_;
        std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid_;

    public:
        TubEvent(double time, uint8_t id, TubData origin, TubData destination,
                 bool safe, bool needs_security,
                 std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid)
        {
            time_ = time;
            id_ = id;
            safe_ = safe;
            origin_ = origin;
            destination_ = destination;
            needs_security_ = needs_security;
            rfid_ = rfid;
        }

        /**
         * Returns the origin information for this tub.
         */
        TubData origin() const noexcept
        {
            return origin_;
        }

        /**
         * Returns the destination information for this tub.
         */
        TubData destination() const noexcept
        {
            return destination_;
        }

        /**
         * Returns whether this tub is safe.
         */
        bool safe() const noexcept
        {
            return safe_;
        }

        std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid() const noexcept
        {
            return rfid_;
        }

        bool needs_security() const noexcept
        {
            return needs_security_;
        }

        virtual void fire() const override;
    };

    /**
     * Checks which events to fire based on the time and list of remaining events.
     * @param vclock The current clock.
     * @param events The remaining events.
     */
    void fire_events(virtual_clock::ISimulationClock &vclock, std::list<std::unique_ptr<events::Event>> &events);

    /**
     * Parses the events in the events.toml file.
     * @param events Vector to populate with parsed events.
     */
    void parse_events(fs::path file, std::list<std::unique_ptr<events::Event>> &events);

};

#endif // EVENTS_H
