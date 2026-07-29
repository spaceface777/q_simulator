#ifndef MESSAGE_H
#define MESSAGE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "../module/types.h"

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

class Init : public Message
{
  private:
    module::ModuleId id_;

  public:
    Init(module::ModuleId id) { id_ = id; }

    /**
     * @return The id of the module.
     */
    module::ModuleId id() const noexcept { return id_; }

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
    module::ModuleId id_;

  public:
    InitFinish(module::ModuleId id) { id_ = id; }

    /**
     * @return The id of the module.
     */
    module::ModuleId id() const noexcept { return id_; }

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
    module::ModuleId module_;
    std::string device_;
    double value_;

  public:
    Update(module::ModuleId module, std::string device, double value)
    {
        module_ = module;
        device_ = device;
        value_ = value;
    }

    /**
     * @return The id of the module.
     */
    module::ModuleId module() const noexcept { return module_; }

    /**
     * @return The device that had its value updated.
     */
    std::string device() const noexcept { return device_; }

    /**
     * @return The updated value.
     */
    double value() const noexcept { return value_; }

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
    module::ModuleId module_;
    std::string uid_;
    std::string data_;

  public:
    UpdateRfid(module::ModuleId module, std::string uid, std::string data)
    {
        module_ = module;
        uid_ = uid;
        data_ = data;
    }

    bool is_valid() const noexcept override;
    std::string to_toml() const noexcept override;
    std::unique_ptr<Message> clone() const override;
};

class UpdateGraph : public Message
{
  private:
    module::ModuleId module_;
    std::size_t size_;
    std::string data_;

  public:
    UpdateGraph(module::ModuleId module, std::size_t size, std::string data)
    {
        module_ = module;
        size_ = size;
        data_ = data;
    }

    bool is_valid() const noexcept override;
    std::string to_toml() const noexcept override;
    std::unique_ptr<Message> clone() const override;
};

class Packet : public Message
{
  private:
    module::ModuleId module_;
    std::string data_;

  public:
    Packet(module::ModuleId module, std::string data)
    {
        module_ = module;
        data_ = data;
    }

    /**
     * @return The id of the target.
     */
    module::ModuleId module() const noexcept { return module_; }

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
    module::ModuleId module_;
    module::ModuleId destination_;
    bool safe_;
    bool needs_security_;

  public:
    TubSchedule(uint8_t id, module::ModuleId module, module::ModuleId destination, bool safe, bool needs_security)
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

// class TubDeparture : public Message
// {
// private:
//     uint8_t id_;
//     uint8_t module_;

// public:
//     TubDeparture(uint8_t id, uint8_t module)
//     {
//         id_ = id;
//         module_ = module;
//     }

//     bool is_valid() const noexcept override;
//     std::string to_toml() const noexcept override;
//     std::unique_ptr<Message> clone() const override;
// };

class PlaneArrival : public Message
{
  private:
    uint8_t id_;
    module::ModuleId module_;
    unsigned int leaves_in_;

  public:
    PlaneArrival(uint8_t id, module::ModuleId module, unsigned int leaves_in)
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
    module::ModuleId module_;

  public:
    PlaneDeparture(uint8_t id, module::ModuleId module)
    {
        id_ = id;
        module_ = module;
    }

    bool is_valid() const noexcept override;
    std::string to_toml() const noexcept override;
};

}; // namespace message

#endif // MESSAGE_H