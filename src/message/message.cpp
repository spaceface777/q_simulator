#include "message.h"

#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../defs.h"
#include "../util.h"

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
    append_value(buf, "id", id_.raw());
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
            const auto parsed_id = util::parse_byte(right);
            if (!parsed_id.has_value())
            {
                return {};
            }

            id = parsed_id.value();
            continue;
        }

        return {};
    }

    return Init(module::ModuleId{static_cast<uint8_t>(id)});
}

bool InitFinish::is_valid() const noexcept
{
    return true;
}

std::string InitFinish::to_toml() const noexcept
{
    std::string buf = create_buffer();
    append_header(buf, "init.finish");
    append_value(buf, "id", id_.raw());
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
            const auto parsed_id = util::parse_byte(right);
            if (!parsed_id.has_value())
            {
                return {};
            }

            id = parsed_id.value();
            continue;
        }

        return {};
    }

    return InitFinish(module::ModuleId{static_cast<uint8_t>(id)});
}

bool Update::is_valid() const noexcept
{
    return !device_.empty();
}

std::string Update::to_toml() const noexcept
{
    std::string buf = create_buffer();
    append_header(buf, "update");
    append_value(buf, "module", module_.raw());
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
            const auto parsed_module = util::parse_byte(right);
            if (!parsed_module.has_value())
            {
                return {};
            }

            module = parsed_module.value();
            continue;
        }
        else if (left == "device" && stage == 1)
        {
            const auto parsed_device = util::parse_quoted_string(right);
            if (!parsed_device.has_value())
            {
                return {};
            }

            device = parsed_device.value();
            continue;
        }
        else if (left == "value" && stage == 1)
        {
            const auto parsed_value = util::parse_int(right);
            if (!parsed_value.has_value())
            {
                return {};
            }

            value = parsed_value.value();
            continue;
        }

        return {};
    }

    return Update(module::ModuleId{static_cast<uint8_t>(module)}, device, value);
}

bool UpdateRfid::is_valid() const noexcept
{
    return !uid_.empty() && !data_.empty();
}

std::string UpdateRfid::to_toml() const noexcept
{
    std::string buf = create_buffer();
    append_header(buf, "update.rfid");
    append_value(buf, "module", module_.raw());
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
    append_value(buf, "module", module_.raw());
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
    return !data_.empty();
}

std::string Packet::to_toml() const noexcept
{
    std::string buf = create_buffer();
    append_header(buf, "packet");
    append_value(buf, "module", module_.raw());
    append_value(buf, "length", data_.length());
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
            const auto parsed_module = util::parse_byte(right);
            if (!parsed_module.has_value())
            {
                return {};
            }

            module = parsed_module.value();
            continue;
        }
        else if (left == "length" && stage == 1)
        {
            continue;
        }
        else if (left == "data" && stage == 1)
        {
            const auto parsed_data = util::parse_quoted_string(right);
            if (!parsed_data.has_value())
            {
                return {};
            }

            data = parsed_data.value();
            continue;
        }

        return {};
    }

    return Packet(module::ModuleId{static_cast<uint8_t>(module)}, data);
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
    append_value(buf, "module", module_.raw());
    append_value(buf, "destination", destination_.raw());
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
    append_value(buf, "module", module_.raw());
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
    append_value(buf, "module", module_.raw());
    trim_trailing_nl(buf);
    return buf;
}

} // namespace message