#ifndef MESSAGE_H
#define MESSAGE_H

#include <string>
#include <cstdint>
#include <optional>
#include <memory>

namespace message
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
        virtual bool is_valid() const noexcept;

        /**
         * @return Converts the data in this class to TOML.
         */
        virtual std::string to_toml() const noexcept;

        /**
         * @return A heap-allocated copy of this message preserving dynamic type.
         */
        virtual std::unique_ptr<Message> clone() const;

        virtual ~Message() = default;
    };

    /**
     * Thrown when a message that is attempting to be parsed is deformed.
     */
    class DeformedPacketException : public std::exception
    {
    private:
        std::string message;

    public:
        DeformedPacketException(const char *msg) : message(msg) {}

        const char *what() const noexcept
        {
            return message.c_str();
        }
    };

    class Init : public Message
    {
    private:
        uint8_t id_;

    public:
        Init(uint8_t id)
        {
            id_ = id;
        }

        /**
         * @return The id of the module.
         */
        uint8_t id() const noexcept
        {
            return id_;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Message> clone() const override;

        /**
         * Creates a new instance on this class based on the passed TOML info.
         * @param toml The TOML info.
         * @return A new Init instance.
         */
        static std::optional<Init> from_toml(std::string_view toml);
    };

    class InitFinish : public Message
    {
    private:
        uint8_t id_;

    public:
        InitFinish(uint8_t id)
        {
            id_ = id;
        }

        /**
         * @return The id of the module.
         */
        uint8_t id() const noexcept
        {
            return id_;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Message> clone() const override;

        /**
         * Creates a new instance on this class based on the passed TOML info.
         * @param toml The TOML info.
         * @return A new Init instance.
         */
        static std::optional<InitFinish> from_toml(std::string_view toml);
    };

    class Update : public Message
    {
    private:
        uint8_t module_;
        std::string device_;
        int value_;

    public:
        Update(uint8_t module, std::string device, int value)
        {
            module_ = module;
            device_ = device;
            value_ = value;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return module_;
        }

        /**
         * @return The device that had its value updated.
         */
        std::string device() const noexcept
        {
            return device_;
        }

        /**
         * @return The updated value.
         */
        int value() const noexcept
        {
            return value_;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Message> clone() const override;

        /**
         * Creates a new instance on this class based on the passed TOML info.
         * @param toml The TOML info.
         * @return A new Update instance.
         */
        static std::optional<Update> from_toml(std::string_view toml);
    };

    class UpdateRfid : public Message
    {
    private:
        uint8_t module_;
        std::string uid_;
        std::string data_;

    public:
        UpdateRfid(uint8_t module, std::string uid, std::string data)
        {
            module_ = module;
            uid_ = uid;
            data_ = data;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Message> clone() const override;
    };

    class Packet : public Message
    {
    private:
        uint8_t module_;
        std::size_t length_;
        std::string content_;

    public:
        Packet(uint8_t module, std::size_t length, std::string content)
        {
            module_ = module;
            length_ = length;
            content_ = content;
        }

        /**
         * @return The id of the target.
         */
        uint8_t module() const noexcept
        {
            return module_;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Message> clone() const override;

        /**
         * Creates a new instance on this class based on the passed TOML info.
         * @param toml The TOML info.
         * @return A new Packet instance.
         */
        static std::optional<Packet> from_toml(std::string_view toml);
    };

    class TubSchedule : public Message
    {
    private:
        uint8_t id_;
        uint8_t module_;
        uint8_t destination_;
        bool safe_;
        bool needs_security_;

    public:
        TubSchedule(uint8_t id, uint8_t module, uint8_t destination, bool safe, bool needs_security)
        {
            id_ = id;
            module_ = module;
            destination_ = destination;
            safe_ = safe;
            needs_security_ = needs_security;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Message> clone() const override;
    };

    // class TubArrival : public Message
    // {
    // private:
    //     uint8_t id_;
    //     uint8_t module_;

    // public:
    //     TubArrival(uint8_t id, uint8_t module)
    //     {
    //         id_ = id;
    //         module_ = module;
    //     }

    //     bool is_valid() const noexcept override;
    //     std::string to_toml() const noexcept override;
    //     std::unique_ptr<Message> clone() const override;
    // };

    class TubDeparture : public Message
    {
    private:
        uint8_t id_;
        uint8_t module_;

    public:
        TubDeparture(uint8_t id, uint8_t module)
        {
            id_ = id;
            module_ = module;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Message> clone() const override;
    };

    class PlaneArrival : public Message
    {
    private:
        uint8_t id_;
        uint8_t module_;
        unsigned int leaves_in_;

    public:
        PlaneArrival(uint8_t id, uint8_t module, unsigned int leaves_in)
        {
            id_ = id;
            module_ = module;
            leaves_in_ = leaves_in;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
    };

    class PlaneDeparture : public Message
    {
    private:
        uint8_t id_;
        uint8_t module_;

    public:
        PlaneDeparture(uint8_t id, uint8_t module)
        {
            id_ = id;
            module_ = module;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
    };

};

#endif // MESSAGE_H