#ifndef TUB_H
#define TUB_H

namespace tub {

    class Tub 
    {
    private:
        uint8_t id_;
        uint8_t module_;
        ModuleLocation location_;
        uint8_t location_progress_;

    public:
        Tub(uint8_t id) : id_(id)
        {

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

        /**
         * @returns A number between 0 and 100 indicating how far along the module location this tub is.
         * Measured from the center to the outskirts. 
         */
        uint8_t module_location_progress() const noexcept
        {
            return location_progress_;
        }
    };
}

#endif