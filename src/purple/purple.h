#ifndef PURPLE_H
#define PURPLE_H

#include <string>
#include <cstdint>

namespace purple
{
    /**
     * Represents a generic PURPLE message.
     */
    class Message
    {
    public:
        /**
         * @return Whether the message with these values is valid.
         */
        virtual bool is_valid() const;

        /**
         * @return Converts the data in this class to TOML.
         */
        virtual std::string to_toml() const;

        virtual ~Message() = default;
    };

    class Update : public Message
    {
    private:
        uint8_t _id;
        std::string _device;
        int _value;

    public:
        Update(uint8_t id, std::string device, int value)
        {
            _id = id;
            _device = device;
            _value = value;
        }

        /**
         * @return The id of the module.
         */
        uint8_t id() const
        {
            return _id;
        }

        /**
         * @return The device that had its value updated.
         */
        std::string device() const
        {
            return _device;
        }

        /**
         * @return The updated value.
         */
        int value() const
        {
            return _value;
        }

        bool is_valid() const override;
        std::string to_toml() const override;
    };

    class Shutdown : public Message
    {
    public:
        Shutdown() {};

        bool is_valid() const override;
        std::string to_toml() const override;
    };

    class ShutdownConfirm : public Message
    {
    private:
        uint8_t _id;

    public:
        ShutdownConfirm(uint8_t id)
        {
            _id = id;
        }

        /**
         * @return The id of the module.
         */
        uint8_t id() const
        {
            return _id;
        }

        bool is_valid() const override;
        std::string to_toml() const override;
    };

};

#endif // PURPLE_H