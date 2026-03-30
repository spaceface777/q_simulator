#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <cstring>
#include <fstream>

#include "module.h"
#include "../tcp/server.h"
#include <map>

namespace module
{

	void Module::run(std::filesystem::path path)
	{
		pid_t pid_ = ::fork();
		if (pid_ == -1)
		{
			throw std::runtime_error("Failed to fork: " + std::string(std::strerror(errno)));
		}

		const bool child = pid_ == 0;
		if (child)
		{
			std::cout << "Starting process " << path << " at " << ::getpid() << std::endl;

			// file for id

			const std::filesystem::path id_path = "/tmp/qaid";
			std::ofstream id_of(id_path);
			if (!id_of.is_open())
			{
				throw std::runtime_error("Failed to create file for " + std::to_string(id_));
			}
			id_of << std::to_string(id_);
			id_of.flush();
			id_of.close();

			// exec

			const std::string module_name = "Module " + std::to_string(id_);
			running_ = true;
			std::cerr << "Started module " << std::to_string(id_) << std::endl;

			const int success = ::execl(path.c_str(), module_name.c_str(), (char *)0);

			if (success < 0)
			{
				throw std::runtime_error("Failed to execl: " + std::string(::strerror(errno)));
			}
		}
	}

	void Module::send(const packet::Packet &packet)
	{
		server::send(id_, packet);
	}

	void Module::update(const packet::Update &packet)
	{
		if (packet.device() == "big belt")
		{
			big_speed_ = packet.value();
		}
		else if (packet.device() == "small belt")
		{
			small_speed_ = packet.value();
		}
		else if (packet.device() == "servo")
		{
			servo_degrees_ = packet.value();
		}
	}

	static std::map<uint8_t, std::unique_ptr<module::Module>> map;

	void add_module(std::unique_ptr<module::Module> ptr)
	{
		if (map.count(ptr->id()))
		{
			throw std::invalid_argument("Attempted to double register modules");
		}
		map[ptr->id()] = std::move(ptr);
	}

	std::optional<std::shared_ptr<module::Module>> get_module(uint8_t id)
	{
		if (map.count(id))
		{
			return std::move(map.at(id));
		}
		return {};
	}

}