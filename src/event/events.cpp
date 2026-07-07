#include "events.h"
#include "../packet/packet.h"
#include "../module/module.h"
#include "../tub/tub.h"

#include <memory>
#include <iostream>

namespace events
{

    void Event::fire() const
    {
    }

    void fire_events(virtual_clock::VirtualClock &vclock, std::list<Event> &events)
    {
        int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(vclock.get_virtual_time()).count();

        events.remove_if([seconds](const auto &event)
                         {
                             if (seconds >= event.time())
                             {
                                 event.fire();
                                 std::cout << "Fired at vtime " << std::to_string(seconds) << std::endl;
                                 return true;
                             }
                             return false; });
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

        std::cout << "Fired plane event at " << std::to_string(_time) << std::endl;
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
        tub::add_tub(std::make_unique<tub::Tub>(std::move(tub)));

        std::cout << "Fired tub event at " << std::to_string(_time) << std::endl;
    }
}