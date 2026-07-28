#ifndef TUB_H
#define TUB_H

#include <cstdint>
#include <memory>
#include <thread>
#include <optional>
#include <vector>
#include <future>
#include <array>

#include "types.h"
#include "../defs.h"
#include "../module/types.h"

namespace tub
{

    class Tub
    {
    private:
        TubId id_;
        module::ModuleId module_;
        ModuleLocation location_ = ModuleLocation::MIDDLE;

        module::ModuleId target_module_;
        ModuleLocation target_module_location_;

        bool in_laser_ = false;
        bool in_rfid_ = false;
        LocationProgress location_progress_{0.5};

        std::future<void> tub_future_;
        std::atomic_bool stopped_ = true;
        std::array<int8_t, RFID_UID_SIZE_BYTES> rfid_uid_;
        std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid_;

    public:
        Tub(TubId id, module::ModuleId module, ModuleLocation location, module::ModuleId target_module,
            ModuleLocation target_module_location, std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid)
            : id_(id), module_(module), location_(location), target_module_(target_module), target_module_location_(target_module_location), rfid_(rfid)
        {
            rfid_uid_ = {static_cast<int8_t>(id.raw()), 0, 0, 0, 0, 0, 0};
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

        [[nodiscard]] TubId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] module::ModuleId module() const noexcept
        {
            return module_;
        }

        /**
         * @returns The module location of the current tub.
         */
        [[nodiscard]] ModuleLocation module_location() const noexcept
        {
            return location_;
        }

        [[nodiscard]] module::ModuleId target_module() const noexcept
        {
            return target_module_;
        }

        /**
         * @returns The target module location of the current tub.
         */
        [[nodiscard]] ModuleLocation target_module_location() const noexcept
        {
            return target_module_location_;
        }

        /**
         * @returns A number between 0 and 1 indicating how far along the module location this tub is.
         * 0 = at center, 1 = at outskirts.
         */
        [[nodiscard]] LocationProgress module_location_progress() const noexcept
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
     * Adds a tub.
     * @param ptr A unique_ptr to the tub.
     */
    void add_tub(std::unique_ptr<Tub> ptr);

    /**
     * Gets an optional tub from its id.
     * @param id The id.
     * @returns The tub instance from the id.
     */
    std::optional<std::shared_ptr<Tub>> get_tub(TubId id);

    /**
     * @returns All tubs.
     */
    std::vector<std::shared_ptr<Tub>> get_tubs();

    void check();

}

#endif