#ifndef TUB_H
#define TUB_H

#include <cstdint>
#include <memory>
#include <thread>
#include <optional>
#include <vector>
#include <future>
#include <array>

#include "../defs.h"

namespace tub
{

    class Tub
    {
    private:
        uint8_t id_;
        uint8_t module_;
        ModuleLocation location_ = ModuleLocation::BOTTOM;

        uint8_t target_module_;
        ModuleLocation target_module_location_;

        bool in_laser_ = false;
        bool in_rfid_ = false;
        // 0 = middle, 1 = extremity
        double location_progress_ = 0.5;

        std::future<void> tub_future_;
        std::atomic_bool stopped_ = false;
        std::array<int8_t, RFID_UID_SIZE_BYTES> rfid_uid_;
        std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid_;

    public:
        Tub(uint8_t id, uint8_t module, uint8_t target_module,
            ModuleLocation target_module_location, std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid)
            : id_(id), module_(module), target_module_(target_module), target_module_location_(target_module_location), rfid_(rfid)
        {
            rfid_uid_ = {static_cast<int8_t>(id), 0, 0, 0, 0, 0, 0};
        }

        /**
         * Stops the worker and waits for it to finish, so a Tub can never be
         * destroyed while its worker thread is still running.
         */
        ~Tub()
        {
            stop();
            join();
        }

        uint8_t id() const noexcept
        {
            return id_;
        }

        uint8_t module() const noexcept
        {
            return module_;
        }

        /**
         * @returns The module location of the current tub.
         */
        ModuleLocation module_location() const noexcept
        {
            return location_;
        }

        uint8_t target_module() const noexcept
        {
            return target_module_;
        }

        /**
         * @returns The target module location of the current tub.
         */
        ModuleLocation target_module_location() const noexcept
        {
            return target_module_location_;
        }

        /**
         * @returns A number between 0 and 1 indicating how far along the module location this tub is.
         * 0 = at center, 1 = at outskirts.
         */
        double module_location_progress() const noexcept
        {
            return location_progress_;
        }

    private:
        void run_();

    public:
        /**
         * Starts the update process of this tub.
         */
        void run();

        void check();

        void stop();

        /**
         * Waits for this tub's worker thread to finish. Call stop() first,
         * otherwise this blocks until the worker stops on its own.
         */
        void join();

        Tub(Tub &&) noexcept = default;
        Tub &operator=(Tub &&) noexcept = default;

        Tub(const Tub &) = delete;
        Tub &operator=(const Tub &) = delete;
    };

    /**
     * Stops all tubs.
     */
    void stop();

    /**
     * Waits for all tub worker threads to finish. Call stop() first.
     * Must be called before main() returns: static destruction begins the
     * moment main returns, and a still-running worker would use freed
     * globals (e.g. the settings table).
     */
    void join();

    /**
     * Adds a tub.
     * @param ptr A unique_ptr to the tub.
     */
    void add_tub(std::unique_ptr<tub::Tub> ptr);

    /**
     * Gets an optional tub from its id.
     * @param id The id.
     * @returns The tub instance from the id.
     */
    std::optional<std::shared_ptr<tub::Tub>> get_tub(uint8_t id);

    /**
     * @returns All tubs.
     */
    std::vector<std::shared_ptr<tub::Tub>> get_tubs();

    void check();

}

#endif