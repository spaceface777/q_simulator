#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <cstring>
#include <fstream>
#include <csignal>
#include <sys/prctl.h>
#include <mutex>

#include "module.h"
#include "../tcp/server.h"
#include "../util.h"
#include <map>

namespace module
{

	void Module::run(std::string path)
	{
		_pid = ::fork();
		util::require(_pid > -1, "Failed to fork: " + std::string(std::strerror(errno)));

		const bool child = _pid == 0;
		if (child)
		{
			::prctl(PR_SET_PDEATHSIG, SIGKILL);

			std::cout << "Starting process " << path << " at " << ::getpid() << std::endl;

			// file for id

			const std::string id_path = "/tmp/qaid";
			std::ofstream id_of(id_path);
			util::require(id_of.is_open(), "Failed to create ofstream for " + std::to_string(id_));

			id_of << std::to_string(id_);
			id_of.flush();
			id_of.close();

			util::require(std::filesystem::exists(id_path), "Failed to create file for " + std::to_string(id_));

			// exec

			const std::string module_name = "Module " + std::to_string(id_);
			running_ = true;
			std::cerr << "Starting module " << std::to_string(id_) << std::endl;

			const int success = ::execl(path.c_str(), module_name.c_str(), (char *)0);

			util::require(success >= 0, "Failed to execl: " + std::string(::strerror(errno)));
		}
	}

	void Module::send(const packet::Packet &packet)
	{
		server::send(id_, packet);
	}

	void Module::update(const packet::ModuleUpdate packet)
	{
		if (packet.device() == "big belt")
		{
			_big_speed = packet.value();
		}
		else if (packet.device() == "small belt")
		{
			_small_speed = packet.value();
		}
		else if (packet.device() == "servo")
		{
			_servo_degrees = packet.value();
		}
		else if (packet.device() == "led")
		{
			_led_color = packet.value();
		}
	}

	static std::map<uint8_t, std::shared_ptr<module::Module>> map;
	static std::mutex map_mutex;

	void add_module(std::unique_ptr<module::Module> ptr)
	{
		std::lock_guard<std::mutex> lock(map_mutex);
		util::require(!map.count(ptr->id()), "Attempted to double register modules");

		uint8_t id = ptr->id();
		map[id] = std::move(ptr);
	}

	std::optional<std::shared_ptr<module::Module>> get_module(uint8_t id)
	{
		std::lock_guard<std::mutex> lock(map_mutex);
		if (map.count(id))
		{
			return map.at(id);
		}
		return {};
	}

	std::vector<std::shared_ptr<module::Module>> get_modules()
	{
		std::lock_guard<std::mutex> lock(map_mutex);
		std::vector<std::shared_ptr<module::Module>> modules;
		for (const auto &pair : map)
		{
			modules.push_back(pair.second);
		}
		return modules;
	}

}