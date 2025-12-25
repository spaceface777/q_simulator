#include <string>
#include <string_view>

#include "packet.h"

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

    bool Update::is_valid() const noexcept
    {
        if (_device.empty())
        {
            return false;
        }

        return true;
    }

    std::string Update::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "update");
        append_value(buf, "id", _id);
        append_value(buf, "device", _device);
        append_value(buf, "value", _value);
        trim_trailing_nl(buf);
        return buf;
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

    bool ShutdownConfirm::is_valid() const noexcept
    {
        return true;
    }

    std::string ShutdownConfirm::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "shutdown.confirm");
        append_value(buf, "id", _id);
        trim_trailing_nl(buf);
        return buf;
    }

}