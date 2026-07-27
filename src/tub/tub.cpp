#include "tub.h"
#include "../module/module.h"
#include "../settings.h"
#include "../message/message.h"
#include "../util.h"

#include <thread>
#include <atomic>
#include <iostream>
#include <string>
#include <cstdint>
#include <cstddef>
#include <map>

namespace tub
{

    static constexpr double RFID_LOCATION = 0.5;
    static constexpr double RFID_RANGE = 0.1;

    static constexpr double LASER_LOCATION = 0.75;
    static constexpr double LASER_RANGE = 0.1;

    static constexpr double MOVEMENT_FACTOR = 1.0 / 100.0 / 60.0 / 2.0;

    static std::string bytes_to_hex(const int8_t *data, std::size_t len)
    {
        if (data == nullptr || len == 0)
        {
            return "";
        }

        static constexpr char hex_digits[] = "0123456789ABCDEF";

        std::string out;
        out.reserve(len * 2);

        for (std::size_t i = 0; i < len; ++i)
        {
            out.push_back(hex_digits[(data[i] >> 4) & 0x0F]);
            out.push_back(hex_digits[data[i] & 0x0F]);
        }

        return out;
    }

    void Tub::run_()
    {
        while (!stopped_)
        {
            // assume if module is in T shape
            // then servo is aimed at right by default
            // angle > 90 = left
            // angle <= 90 = right

            const std::optional<std::shared_ptr<module::Module>> module_opt = module::get_module(module_);
            if (!module_opt)
            {
                stop();
                throw std::runtime_error("Tub " + std::to_string(id_.raw()) + " is owned by invalid module " + std::to_string(module_.raw()));
            }
            const std::shared_ptr<module::Module> module = module_opt.value();
            const settings::ModuleSettings settings = settings::get_module_settings(module_);

            if (location_progress_.is_at_end())
            {
                const module::ModuleId old_module = module_;
                std::optional<module::ModuleId> new_module;
                switch (location_)
                {
                case ModuleLocation::MIDDLE:
                    new_module = settings.middle;
                    break;
                case ModuleLocation::LEFT:
                    new_module = settings.left;
                    break;
                case ModuleLocation::RIGHT:
                    new_module = settings.right;
                    break;
                }

                if (!new_module)
                    return;

                const settings::ModuleSettings new_settings = settings::get_module_settings(*new_module);
                if (new_settings.middle == old_module)
                {
                    location_ = ModuleLocation::MIDDLE;
                }
                else if (new_settings.left == old_module)
                {
                    location_ = ModuleLocation::LEFT;
                }
                else if (new_settings.right == old_module)
                {
                    location_ = ModuleLocation::RIGHT;
                }
                else
                {
                    stop();
                    throw std::invalid_argument("Module " + std::to_string(old_module.raw()) + " and " + std::to_string(new_module->raw()) + " are not connected in settings.toml");
                }

                location_progress_ = LocationProgress{1.0};
                module_ = *new_module;
            }
            else if (location_progress_.is_at_begin())
            {
                switch (location_)
                {
                case ModuleLocation::MIDDLE:
                    if (module->servo_angle() > 90)
                    {
                        location_ = ModuleLocation::RIGHT;
                    }
                    else
                    {
                        location_ = ModuleLocation::LEFT;
                    }
                    break;
                case ModuleLocation::LEFT:
                    if (module->servo_angle() < 10 || module->servo_angle() > 170)
                    {
                        location_ = ModuleLocation::RIGHT;
                    }
                    else
                    {
                        location_ = ModuleLocation::MIDDLE;
                    }
                    break;
                case ModuleLocation::RIGHT:
                    if (module->servo_angle() > 170 || module->servo_angle() < 10)
                    {
                        location_ = ModuleLocation::LEFT;
                    }
                    else
                    {
                        location_ = ModuleLocation::MIDDLE;
                    }
                    break;
                }
                location_progress_ = LocationProgress{0};
            }
            else
            {
                const double simulation_speed = settings::get_setting<double>("simulation_speed");

                switch (location_)
                {
                case ModuleLocation::MIDDLE:
                    location_progress_ += simulation_speed * module->small_belt_speed().raw() * MOVEMENT_FACTOR * (TIMEOUT.count() / 16.0);

                    if (in_rfid_ && (location_progress_ < RFID_LOCATION - RFID_RANGE || location_progress_ > RFID_LOCATION + RFID_RANGE))
                    {
                        module->send(message::UpdateRfid(module_, "", ""));
                        module->set_rfid_active(false);
                        in_rfid_ = false;
                    }
                    else if (!in_rfid_ && location_progress_ > RFID_LOCATION - RFID_RANGE && location_progress_ < RFID_LOCATION + RFID_RANGE)
                    {
                        module->send(message::UpdateRfid(module_,
                                                         bytes_to_hex(reinterpret_cast<int8_t *>(rfid_uid_.data()), rfid_uid_.size()),
                                                         bytes_to_hex(reinterpret_cast<int8_t *>(rfid_.data()), rfid_.size())));
                        module->set_rfid_active(true);
                        in_rfid_ = true;
                    }
                    break;
                case ModuleLocation::LEFT:
                    location_progress_ += simulation_speed * module->big_belt_speed().raw() * MOVEMENT_FACTOR * (TIMEOUT.count() / 16.0);

                    if (in_laser_ && (location_progress_ < LASER_LOCATION - LASER_RANGE || location_progress_ > LASER_LOCATION + LASER_RANGE))
                    {
                        module->send(message::Update(module_, "left laser", false));
                        module->set_left_laser_active(false);
                        in_laser_ = false;
                    }
                    else if (!in_laser_ && location_progress_ > LASER_LOCATION - LASER_RANGE && location_progress_ < LASER_LOCATION + LASER_RANGE)
                    {

                        module->send(message::Update(module_, "left laser", true));
                        module->set_left_laser_active(true);
                        in_laser_ = true;
                    }
                    break;
                case ModuleLocation::RIGHT:
                    location_progress_ += simulation_speed * -module->big_belt_speed().raw() * MOVEMENT_FACTOR * (TIMEOUT.count() / 16.0);

                    if (in_laser_ && (location_progress_ < LASER_LOCATION - LASER_RANGE || location_progress_ > LASER_LOCATION + LASER_RANGE))
                    {
                        module->send(message::Update(module_, "right laser", false));
                        module->set_right_laser_active(false);
                        in_laser_ = false;
                    }
                    else if (!in_laser_ && location_progress_ > LASER_LOCATION - LASER_RANGE && location_progress_ < LASER_LOCATION + LASER_RANGE)
                    {
                        module->send(message::Update(module_, "right laser", true));
                        module->set_right_laser_active(true);
                        in_laser_ = true;
                    }
                    break;
                }
            }

            std::this_thread::sleep_for(TIMEOUT);
        }
    }

    void Tub::run()
    {
        util::require(stopped_, "Cannot run the same tub twice");

        stopped_ = false;

        tub_future_ = std::async(std::launch::async, [this]
                                 { run_(); });
    }

    void Tub::check()
    {
        if (!tub_future_.valid())
        {
            return;
        }

        auto status = tub_future_.wait_for(std::chrono::seconds(0));
        if (status == std::future_status::ready)
        {
            tub_future_.get();
        }
    }

    void Tub::stop()
    {
        stopped_ = true;
    }

    void Tub::join()
    {
        if (tub_future_.valid())
        {
            tub_future_.wait();
        }
    }

    static std::map<uint8_t, std::shared_ptr<tub::Tub>> map;

    void stop()
    {
        for (auto &tub : get_tubs())
        {
            tub->stop();
            tub->join();
        }
        map.clear();
    }

    void add_tub(std::unique_ptr<tub::Tub> ptr)
    {
        if (map.count(ptr->id().raw()))
        {
            stop();
            throw std::invalid_argument("Attempted to double register tubs");
        }
        map[ptr->id().raw()] = std::move(ptr);
    }

    std::optional<std::shared_ptr<tub::Tub>> get_tub(TubId id)
    {
        if (map.count(id.raw()))
        {
            return map.at(id.raw());
        }
        return {};
    }

    std::vector<std::shared_ptr<tub::Tub>> get_tubs()
    {
        std::vector<std::shared_ptr<tub::Tub>> tubs;
        for (const auto &pair : map)
        {
            tubs.push_back(pair.second);
        }
        return tubs;
    }

    void check()
    {
        for (auto &tub : get_tubs())
        {
            tub->check();
        }
    }
}