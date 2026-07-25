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
		pid_t pid = ::fork();
		util::require(pid > -1, "Failed to fork: " + std::string(std::strerror(errno)));

		const bool child = pid == 0;
		if (child)
		{
			::prctl(PR_SET_PDEATHSIG, SIGKILL);

			std::cout << "Starting process " << path << " at " << ::getpid() << std::endl;

			// file for id

			const std::string id_path = "/tmp/qaid";
			std::ofstream id_of(id_path);
			util::require(id_of.is_open(), "Failed to create ofstream for " + std::to_string(id_.raw()));

			id_of << std::to_string(id_.raw());
			id_of.flush();
			id_of.close();

			util::require(std::filesystem::exists(id_path), "Failed to create file for " + std::to_string(id_.raw()));

			// exec

			const std::string module_name = "Module " + std::to_string(id_.raw());
			running_ = true;
			std::cerr << "Starting module " << std::to_string(id_.raw()) << std::endl;

			const int success = ::execl(path.c_str(), module_name.c_str(), (char *)0);

			util::require(success >= 0, "Failed to execl: " + std::string(::strerror(errno)));
		}
		else
		{
			pid_ = pid;
		}
	}

	void Module::send(const message::Message &message)
	{
		server::send(id_, message);
	}

	void Module::update(const message::Update message)
	{
		if (message.device() == "big belt")
		{
			big_speed_ = BeltSpeed(message.value());
		}
		else if (message.device() == "small belt")
		{
			small_speed_ = BeltSpeed(message.value());
		}
		else if (message.device() == "servo")
		{
			servo_degrees_ = ServoAngle(message.value());
		}
		else if (message.device() == "led")
		{
			led_color_ = LedColor(message.value());
		}
	}

	static std::map<uint8_t, std::shared_ptr<module::Module>> map;
	static std::mutex map_mutex;

	void add_module(std::unique_ptr<module::Module> ptr)
	{
		std::lock_guard<std::mutex> lock(map_mutex);
		util::require(!map.count(ptr->id().raw()), "Attempted to double register modules");

		uint8_t id = ptr->id().raw();
		map[id] = std::move(ptr);
	}

	std::optional<std::shared_ptr<module::Module>> get_module(ModuleId id)
	{
		std::lock_guard<std::mutex> lock(map_mutex);
		if (map.count(id.raw()))
		{
			return map.at(id.raw());
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