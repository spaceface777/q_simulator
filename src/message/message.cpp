#include <string>
#include <string_view>
#include <stdexcept>
#include <iostream>
#include <optional>
#include <limits>

#include "message.h"
#include "../util.h"
#include "../defs.h"

namespace message
{
    /**
     * @return A buffer to create a TOML representation.
     */
    std::string create_buffer()
    {
        std::string buf;
        buf.reserve(256);
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

    static std::optional<uint8_t> parse_byte(std::string_view text)
    {
        try
        {
            std::size_t consumed = 0;
            const long long value = std::stoll(std::string(text), &consumed);
            if (consumed != text.size() || value < 0 || value > std::numeric_limits<uint8_t>::max())
            {
                return {};
            }

            return static_cast<uint8_t>(value);
        }
        catch (const std::invalid_argument &)
        {
            return {};
        }
        catch (const std::out_of_range &)
        {
            return {};
        }
    }

    static std::optional<int> parse_int(std::string_view text)
    {
        try
        {
            std::size_t consumed = 0;
            const long long value = std::stoll(std::string(text), &consumed);
            if (consumed != text.size() || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
            {
                return {};
            }

            return static_cast<int>(value);
        }
        catch (const std::invalid_argument &)
        {
            return {};
        }
        catch (const std::out_of_range &)
        {
            return {};
        }
    }

    static std::optional<std::size_t> parse_size(std::string_view text)
    {
        try
        {
            std::size_t consumed = 0;
            const long long value = std::stoll(std::string(text), &consumed);
            if (consumed != text.size() || value < 0)
            {
                return {};
            }

            return static_cast<std::size_t>(value);
        }
        catch (const std::invalid_argument &)
        {
            return {};
        }
        catch (const std::out_of_range &)
        {
            return {};
        }
    }

    static std::optional<std::string> parse_quoted_string(std::string_view text)
    {
        if (text.size() < 2 || text.front() != '"' || text.back() != '"')
        {
            return {};
        }

        return std::string(text.substr(1, text.size() - 2));
    }

    bool Message::is_valid() const noexcept
    {
        return false;
    }

    std::string Message::to_toml() const noexcept
    {
        return std::string();
    }

    std::unique_ptr<Message> Message::clone() const
    {
        return std::make_unique<Message>(*this);
    }

    bool Init::is_valid() const noexcept
    {
        return true;
    }

    std::string Init::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "init");
        append_value(buf, "id", id_);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Message> Init::clone() const
    {
        return std::make_unique<Init>(*this);
    }

    std::optional<Init> Init::from_toml(std::string_view toml)
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

            if (trimmed == "[init]")
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
                const auto parsed_id = parse_byte(right);
                if (!parsed_id.has_value())
                {
                    return {};
                }

                id = parsed_id.value();
                continue;
            }

            return {};
        }

        return Init(id);
    }

    bool InitFinish::is_valid() const noexcept
    {
        return true;
    }

    std::string InitFinish::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "init.finish");
        append_value(buf, "id", id_);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Message> InitFinish::clone() const
    {
        return std::make_unique<InitFinish>(*this);
    }

    std::optional<InitFinish> InitFinish::from_toml(std::string_view toml)
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

            if (trimmed == "[init.finish]")
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
                const auto parsed_id = parse_byte(right);
                if (!parsed_id.has_value())
                {
                    return {};
                }

                id = parsed_id.value();
                continue;
            }

            return {};
        }

        return InitFinish(id);
    }

    bool Update::is_valid() const noexcept
    {
        return !device_.empty();
    }

    std::string Update::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "update");
        append_value(buf, "module", module_);
        append_value(buf, "device", device_);
        append_value(buf, "value", value_);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Message> Update::clone() const
    {
        return std::make_unique<Update>(*this);
    }

    std::optional<Update> Update::from_toml(std::string_view toml)
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
                const auto parsed_module = parse_byte(right);
                if (!parsed_module.has_value())
                {
                    return {};
                }

                module = parsed_module.value();
                continue;
            }
            else if (left == "device" && stage == 1)
            {
                const auto parsed_device = parse_quoted_string(right);
                if (!parsed_device.has_value())
                {
                    return {};
                }

                device = parsed_device.value();
                continue;
            }
            else if (left == "value" && stage == 1)
            {
                const auto parsed_value = parse_int(right);
                if (!parsed_value.has_value())
                {
                    return {};
                }

                value = parsed_value.value();
                continue;
            }

            return {};
        }

        return Update(module, device, value);
    }

    bool UpdateRfid::is_valid() const noexcept
    {
        return !uid_.empty() && !data_.empty();
    }

    std::string UpdateRfid::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "update.rfid");
        append_value(buf, "module", module_);
        append_value(buf, "uid_length", uid_.length());
        append_value(buf, "uid", uid_);
        append_value(buf, "data_length", data_.length());
        append_value(buf, "data", data_);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Message> UpdateRfid::clone() const
    {
        return std::make_unique<UpdateRfid>(*this);
    }

    bool UpdateGraph::is_valid() const noexcept
    {
        return size_ > 0 && !data_.empty();
    }

    std::string UpdateGraph::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "update.graph");
        append_value(buf, "module", module_);
        append_value(buf, "size", size_);
        append_value(buf, "data_length", data_.length());
        append_value(buf, "data", data_);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Message> UpdateGraph::clone() const
    {
        return std::make_unique<UpdateGraph>(*this);
    }

    bool Packet::is_valid() const noexcept
    {
        if (data_.length() != length_)
        {
            return false;
        }

        return true;
    }

    std::string Packet::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "packet");
        append_value(buf, "module", module_);
        append_value(buf, "length", length_);
        append_value(buf, "data", data_);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Message> Packet::clone() const
    {
        return std::make_unique<Packet>(*this);
    }

    std::optional<Packet> Packet::from_toml(std::string_view toml)
    {
        const std::vector<std::string_view> lines = util::split(toml, "\n");

        int stage = 0;
        int module = -1;
        std::size_t length = 0;
        std::string data;
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
                const auto parsed_module = parse_byte(right);
                if (!parsed_module.has_value())
                {
                    return {};
                }

                module = parsed_module.value();
                continue;
            }
            else if (left == "length" && stage == 1)
            {
                const auto parsed_length = parse_size(right);
                if (!parsed_length.has_value())
                {
                    return {};
                }

                length = parsed_length.value();
                continue;
            }
            else if (left == "data" && stage == 1)
            {
                const auto parsed_data = parse_quoted_string(right);
                if (!parsed_data.has_value())
                {
                    return {};
                }

                data = parsed_data.value();
                continue;
            }

            return {};
        }

        return Packet(module, length, data);
    }

    bool TubSchedule::is_valid() const noexcept
    {
        return true;
    }

    std::string TubSchedule::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "tub.schedule");
        append_value(buf, "id", id_);
        append_value(buf, "module", module_);
        append_value(buf, "destination", destination_);
        append_value(buf, "safe", safe_);
        append_value(buf, "needs_security", needs_security_);
        trim_trailing_nl(buf);
        return buf;
    }

    std::unique_ptr<Message> TubSchedule::clone() const
    {
        return std::make_unique<TubSchedule>(*this);
    }

    // bool TubArrival::is_valid() const noexcept
    // {
    //     return true;
    // }

    // std::string TubArrival::to_toml() const noexcept
    // {
    //     std::string buf = create_buffer();
    //     append_header(buf, "tub.arrival");
    //     append_value(buf, "id", id_);
    //     append_value(buf, "module", module_);
    //     trim_trailing_nl(buf);
    //     return buf;
    // }

    // std::unique_ptr<Message> TubArrival::clone() const
    // {
    //     return std::make_unique<TubArrival>(*this);
    // }

    // bool TubDeparture::is_valid() const noexcept
    // {
    //     return true;
    // }

    // std::string TubDeparture::to_toml() const noexcept
    // {
    //     std::string buf = create_buffer();
    //     append_header(buf, "tub.departure");
    //     append_value(buf, "id", id_);
    //     append_value(buf, "module", module_);
    //     trim_trailing_nl(buf);
    //     return buf;
    // }

    // std::unique_ptr<Message> TubDeparture::clone() const
    // {
    //     return std::make_unique<TubDeparture>(*this);
    // }

    bool PlaneArrival::is_valid() const noexcept
    {
        return true;
    }

    std::string PlaneArrival::to_toml() const noexcept
    {
        std::string buf = create_buffer();
        append_header(buf, "plane.arrival");
        append_value(buf, "id", id_);
        append_value(buf, "module", module_);
        append_value(buf, "leaves_in", leaves_in_);
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
        append_value(buf, "id", id_);
        append_value(buf, "module", module_);
        trim_trailing_nl(buf);
        return buf;
    }

}