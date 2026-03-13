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
        uint8_t _id;

    public:
        Introduction(uint8_t id)
        {
            _id = id;
        }

        /**
         * @return The id of the module.
         */
        uint8_t id() const noexcept
        {
            return _id;
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

    class Update : public Packet
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
        uint8_t id() const noexcept
        {
            return _id;
        }

        /**
         * @return The device that had its value updated.
         */
        std::string device() const noexcept
        {
            return _device;
        }

        /**
         * @return The updated value.
         */
        int value() const noexcept
        {
            return _value;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Packet> clone() const override;

        /**
         * Creates a new instance on this class based on the passed TOML info.
         * @param toml The TOML info.
         * @return A new Update instance.
         */
        static std::optional<Update> from_toml(std::string_view toml);
    };

    class PacketSend : public Packet
    {
    private:
        uint8_t _module;
        std::size_t _length;
        std::string _content;

    public:
        PacketSend(uint8_t module, std::size_t length, std::string content)
        {
            _module = module;
            _length = length;
            _content = content;
        }

        /**
         * @return The id of the target.
         */
        uint8_t module() const noexcept
        {
            return _module;
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
         * @return A new PacketSend instance.
         */
        static std::optional<PacketSend> from_toml(std::string_view toml);
    };

    class TubArrival : public Packet
    {
    private:
        uint8_t _id;
        uint8_t _module;
        uint8_t _destination;
        bool _safe;

    public:
        TubArrival(uint8_t id, uint8_t module, uint8_t destination, bool safe)
        {
            _id = id;
            _module = module;
            _destination = destination;
            _safe = safe;
        }

        /**
         * @return The id of the plane.
         */
        uint8_t id() const noexcept
        {
            return _id;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return _module;
        }

        /**
         * @return The amount of time until the plane leaves.
         */
        uint8_t destination() const noexcept
        {
            return _destination;
        }

        /**
         * @return Whether this tub is safe.
         */
        bool safe() const noexcept
        {
            return _safe;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Packet> clone() const override;
    };

    class TubDeparture : public Packet
    {
    private:
        uint8_t _id;
        uint8_t _module;

    public:
        TubDeparture(uint8_t id, uint8_t module)
        {
            _id = id;
            _module = module;
        }

        /**
         * @return The id of the plane.
         */
        uint8_t id() const noexcept
        {
            return _id;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return _module;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
        std::unique_ptr<Packet> clone() const override;
    };

    class PlaneArrival : public Packet
    {
    private:
        uint8_t _id;
        uint8_t _module;
        uint16_t _leaves_in;

    public:
        PlaneArrival(uint8_t id, uint8_t module, uint16_t leaves_in)
        {
            _id = id;
            _module = module;
            _leaves_in = leaves_in;
        }

        /**
         * @return The id of the plane.
         */
        uint8_t id() const noexcept
        {
            return _id;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return _module;
        }

        /**
         * @return The amount of time until the plane leaves.
         */
        uint16_t leaves_in() const noexcept
        {
            return _leaves_in;
        }

        bool is_valid() const noexcept override;
        std::string to_toml() const noexcept override;
    };

    class PlaneDeparture : public Packet
    {
    private:
        uint8_t _id;
        uint8_t _module;

    public:
        PlaneDeparture(uint8_t id, uint8_t module)
        {
            _id = id;
            _module = module;
        }

        /**
         * @return The id of the plane.
         */
        uint8_t id() const noexcept
        {
            return _id;
        }

        /**
         * @return The id of the module.
         */
        uint8_t module() const noexcept
        {
            return _module;
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