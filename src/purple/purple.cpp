#include "purple.h"
#include <string>
#include <string_view>

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

bool purple::Message::is_valid() const
{
    return false;
}

std::string purple::Message::to_toml() const
{
    return std::string();
}

bool purple::Update::is_valid() const
{
    if (_device.empty())
    {
        return false;
    }

    return true;
}

std::string purple::Update::to_toml() const
{
    std::string buffer = create_buffer();
    append_header(buffer, "update");
    append_value(buffer, "id", _id);
    append_value(buffer, "device", _device);
    append_value(buffer, "value", _value);
    return buffer;
}

bool purple::Shutdown::is_valid() const
{
    return true;
}

std::string purple::Shutdown::to_toml() const
{
    std::string buffer = create_buffer();
    append_header(buffer, "shutdown");
    return buffer;
}

bool purple::ShutdownConfirm::is_valid() const
{
    return true;
}

std::string purple::ShutdownConfirm::to_toml() const
{
    std::string buffer = create_buffer();
    append_header(buffer, "shutdown.confirm");
    append_value(buffer, "id", _id);
    return buffer;
}
