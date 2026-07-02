#include "events.h"
#include "../packet/packet.h"
#include "../module/module.h"
#include "../tub/tub.h"

#include <unique>

namespace events 
{

    void fire_events(virtual_clock::VirtualClock &vclock, std::list<Event> &events)
    {
        int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(vclock.get_virtual_time()).count();

        for (const auto &event : events) 
        {
            if (seconds < event.time()) 
            {
                continue;
            }

            event.fire();
            events.remove(event);
        }
    }

    void PlaneEvent::fire() const
    {
        auto packet = packet::PlaneArrival(_id, _module, _leaves_in);
        
        for (const auto &module : module::get_modules())
        {
            if (!module) 
            {
                continue;
            }

            module->send(packet);
        }
    }

    void TubEvent::fire() const
    {
        auto packet = packet::TubArrival(_id, _origin.id, _destination.id, _safe);

        for (const auto &module : module::get_modules())
        {
            if (!module) 
            {
                continue;
            }

            module->send(packet);
        }

        tub::Tub tub = tub::Tub(_id, _origin.id, _destination.id, ModuleLocation::BOTTOM_BELT);
        tub.run();
        tub::add_tub(std::make_unique(tub));
    }
}