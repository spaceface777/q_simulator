#ifndef PACKET_H
#define PACKET_H

#include <string>
#include <cstdint>
#include <optional>
#include <memory>

namespace packet
{
    /**
     * Represents a generic PURPLE message.
     */
    class Packet
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
         * @return A heap-allocated copy of this packet preserving dynamic type.
         */
        virtual std::unique_ptr<Packet> clone() const;

        virtual ~Packet() = default;
    };

    /**
     * Thrown when a packet that is attempting to be parsed is deformed.
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

    class Introduction : public Packet
    {
    private:
        uint8_t id_;

    public:
        Introduction(uint8_t id)
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
        std::unique_ptr<Packet> clone() const override;

        /**
         * Creates a new instance on this class based on the passed TOML info.
         * @param toml The TOML info.
         * @return A new Introduction instance.
         */
        static std::optional<Introduction> from_toml(std::string_view toml);
    };

    class ModuleUpdate : public Packet
    {
    private:
        uint8_t id_;
        std::string device_;
        int value_;

    public:
        ModuleUpdate(uint8_t id, std::string device, int value)
        {
            id_ = id;
            device_ = device;
            value_ = value;
        }

        /**
         * @return The id of the module.
         */
        uint8_t id() const noexcept
        {
            return id_;
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
        std::unique_ptr<Packet> clone() const override;

        /**
         * Creates a new instance on this class based on the passed TOML info.
         * @param toml The TOML info.
         * @return A new Update instance.
         */
        static std::optional<ModuleUpdate> from_toml(std::string_view toml);
    };

    class SimulationUpdate : public Packet
    {
    private:
        uint8_t id_;
        std::string device_;
        int value_;

    public:
        SimulationUpdate(uint8_t id, std::string device, int value)
        {
            id_ = id;
            device_ = device;
            value_ = value;
        }

        /**
         * @return The id of the module.
         */
        uint8_t id() const noexcept
        {
            return id_;
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
        std::unique_ptr<Packet> clone() const override;
    };

    class ModulePacket : public Packet
    {
    private:
        uint8_t module_;
        std::size_t _length;
        std::string _content;

    public:
        ModulePacket(uint8_t module, std::size_t length, std::string content)
        {
            module_ = module;
            _length = length;
            _content = content;
        }

        /**
         * @return The id of the target.
         */
        uint8_t module() const noexcept
        {
            return module_;
        }

        /**
         * @return The length of the content.
         */
        std::size_t length() const noexcept
        {
            return _length;
        }

        /**
         * @return The content.
         */
        std::string content() const noexcept
        {
            return _content;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Packet> clone() const override;

        /**
         * Creates a new instance on this class based on the passed TOML info.
         * @param toml The TOML info.
         * @return A new ModulePacket instance.
         */
        static std::optional<ModulePacket> from_toml(std::string_view toml);
    };

    class TubArrival : public Packet
    {
    private:
        uint8_t id_;
        uint8_t module_;
        uint8_t destination_;
        bool safe_;

    public:
        TubArrival(uint8_t id, uint8_t module, uint8_t destination, bool safe)
        {
            id_ = id;
            module_ = module;
            destination_ = destination;
            safe_ = safe;
        }

        /**
         * @return The id of the plane.
         */
        uint8_t id() const noexcept
        {
            return id_;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return module_;
        }

        /**
         * @return The amount of time until the plane leaves.
         */
        uint8_t destination() const noexcept
        {
            return destination_;
        }

        /**
         * @return Whether this tub is safe.
         */
        bool safe() const noexcept
        {
            return safe_;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Packet> clone() const override;
    };

    class TubDeparture : public Packet
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

        /**
         * @return The id of the plane.
         */
        uint8_t id() const noexcept
        {
            return id_;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return module_;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Packet> clone() const override;
    };

    class PlaneArrival : public Packet
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

        /**
         * @return The id of the plane.
         */
        uint8_t id() const noexcept
        {
            return id_;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return module_;
        }

        /**
         * @return The amount of time until the plane leaves.
         */
        unsigned int leaves_in() const noexcept
        {
            return leaves_in_;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
    };

    class PlaneDeparture : public Packet
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

        /**
         * @return The id of the plane.
         */
        uint8_t id() const noexcept
        {
            return id_;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return module_;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
    };

    class Shutdown : public Packet
    {
    public:
        Shutdown() {};

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
    };

};

#endif // PACKET_H