#include <string>
#include <string_view>
#include <stdexcept>

#include "packet.h"
#include "../util.h"
#include "../defs.h"
#include <optional>

namespace packet
{
    /**
     * @return A buffer to create a TOML representation.
     */
    std::string create_buffer()
    {
        std::string buf;
        buf.reserve(64);
        return buf;
    }

    /**
     * Appends a TOML header to the buffer.
     * @param buf The buffer.
     * @param header The header to add.
     */
    void append_header(std::string &buf, std::string header)
    {
        buf += "[" + header + "]\n";
    }

    /**
     * Appends a value to the buffer.
     * @param buf The buffer.
     * @param attr The attribute.
     * @param value The value.
     */
    void append_value(std::string &buf, std::string_view attr, int value)
    {
        buf += std::string(attr) + " = " + std::to_string(value) + "\n";
    }

    /**
     * Appends a value to the buffer.
     * @param buf The buffer.
     * @param attr The attribute.
     * @param value The value.
     */
    void append_value(std::string &buf, std::string_view attr, std::string_view value)
    {
        buf += std::string(attr) + " = \"" + std::string(value) + "\"\n";
    }

    /**
     * Removes a trailing nl.
     * @param buf The buffer.
     */
    void trim_trailing_nl(std::string &buf)
    {
        if (buf[buf.size() - 1] == '\n')
        {
            buf.erase(buf.size() - 1, 1);
        }
    }

    bool Packet::is_valid() const noexcept
    {
        return false;
    }

    std::string Packet::to_toml() const noexcept
    {
        return std::string();
    }

    std::unique_ptr<Packet> Packet::clone() const
    {
        return std::make_unique<Packet>(*this);
    }

    bool Introduction::is_valid() const noexcept
    {
        return true;
    }

    std::string Introduction::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "introduction");
        append_value(buf, "id", _id);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Packet> Introduction::clone() const
    {
        return std::make_unique<Introduction>(*this);
    }

    std::optional<Introduction> Introduction::from_toml(std::string_view toml)
    {
        const std::vector<std::string_view> lines = util::split(toml, "\n");

        int stage = 0;
        int id = -1;
        for (const std::string_view line : lines)
        {
            const std::string_view trimmed = util::trim(line);

            if (trimmed.empty())
            {
                continue;
            }

            if (trimmed == "[introduction]")
            {
                stage = 1;
                continue;
            }

            const std::vector<std::string_view> parts = util::split(trimmed, "=");

            if (parts.size() != 2)
            {
                return {};
            }

            const std::string_view left = util::trim(parts[0]);
            const std::string right = std::string(util::trim(parts[1]));

            if (left == "id" && stage == 1)
            {
                id = std::stoi(right);
                continue;
            }

            return {};
        }

        return Introduction(id);
    }

    bool ModuleUpdate::is_valid() const noexcept
    {
        if (_device.empty())
        {
            return false;
        }

        return true;
    }

    std::string ModuleUpdate::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "update");
        append_value(buf, "id", _id);
        append_value(buf, "device", _device);
        append_value(buf, "value", _value);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Packet> ModuleUpdate::clone() const
    {
        return std::make_unique<ModuleUpdate>(*this);
    }

    std::optional<ModuleUpdate> ModuleUpdate::from_toml(std::string_view toml)
    {
        const std::vector<std::string_view> lines = util::split(toml, "\n");

        int stage = 0;
        int module = -1;
        std::string device;
        int value = -1;
        for (const std::string_view line : lines)
        {
            const std::string_view trimmed = util::trim(line);

            if (trimmed.empty())
            {
                continue;
            }

            if (trimmed == "[update]")
            {
                stage = 1;
                continue;
            }

            const std::vector<std::string_view> parts = util::split(trimmed, "=");

            if (parts.size() != 2)
            {
                return {};
            }

            const std::string_view left = util::trim(parts[0]);
            const std::string right = std::string(util::trim(parts[1]));

            if (left == "module" && stage == 1)
            {
                module = std::stoi(right);
                continue;
            }
            else if (left == "device" && stage == 1)
            {
                device = right;
                continue;
            }
            else if (left == "value" && stage == 1)
            {
                value = std::stoi(right);
                continue;
            }

            return {};
        }

        return ModuleUpdate(module, device, value);
    }

    bool ModulePacket::is_valid() const noexcept
    {
        if (_content.length() != _length)
        {
            return false;
        }

        return true;
    }

    std::string ModulePacket::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "packet");
        append_value(buf, "module", _module);
        append_value(buf, "length", _length);
        append_value(buf, "content", _content);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Packet> ModulePacket::clone() const
    {
        return std::make_unique<ModulePacket>(*this);
    }

    std::optional<ModulePacket> ModulePacket::from_toml(std::string_view toml)
    {
        const std::vector<std::string_view> lines = util::split(toml, "\n");

        int stage = 0;
        int module = -1;
        std::size_t length;
        std::string content;
        for (const std::string_view line : lines)
        {
            const std::string_view trimmed = util::trim(line);

            if (trimmed.empty())
            {
                continue;
            }

            if (trimmed == "[packet]")
            {
                stage = 1;
                continue;
            }

            const std::vector<std::string_view> parts = util::split(trimmed, "=");

            if (parts.size() != 2)
            {
                return {};
            }

            const std::string_view left = util::trim(parts[0]);
            const std::string right = std::string(util::trim(parts[1]));

            if (left == "module" && stage == 1)
            {
                module = std::stoi(right);
                continue;
            }
            else if (left == "length" && stage == 1)
            {
                length = std::stoi(right);
                continue;
            }
            else if (left == "content" && stage == 1)
            {
                content = right;
                continue;
            }

            return {};
        }

        return ModulePacket(module, length, content);
    }

    bool TubArrival::is_valid() const noexcept
    {
        return true;
    }

    std::string TubArrival::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "tub.arrival");
        append_value(buf, "id", _id);
        append_value(buf, "module", _module);
        append_value(buf, "destination", _destination);
        append_value(buf, "safe", _safe);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Packet> TubArrival::clone() const
    {
        return std::make_unique<TubArrival>(*this);
    }

    bool TubDeparture::is_valid() const noexcept
    {
        return true;
    }

    std::string TubDeparture::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "tub.departure");
        append_value(buf, "id", _id);
        append_value(buf, "module", _module);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Packet> TubDeparture::clone() const
    {
        return std::make_unique<TubDeparture>(*this);
    }

    bool PlaneArrival::is_valid() const noexcept
    {
        return true;
    }

    std::string PlaneArrival::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "plane.arrival");
        append_value(buf, "id", _id);
        append_value(buf, "module", _module);
        append_value(buf, "leaves_in", _leaves_in);
        trim_trailing_nl(buf);
        return buf;
    }

    bool PlaneDeparture::is_valid() const noexcept
    {
        return true;
    }

    std::string PlaneDeparture::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "plane.departure");
        append_value(buf, "id", _id);
        append_value(buf, "module", _module);
        trim_trailing_nl(buf);
        return buf;
    }

    bool Shutdown::is_valid() const noexcept
    {
        return true;
    }

    std::string Shutdown::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "shutdown");
        trim_trailing_nl(buf);
        return buf;
    }

}