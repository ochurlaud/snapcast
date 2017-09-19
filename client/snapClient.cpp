/***
    This file is part of snapcast
    Copyright (C) 2014-2017  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include <iostream>
#include <sys/resource.h>

#include "popl.hpp"
#include "controller.h"
#include "browseZeroConf/browsemDNS.h"

#ifdef HAS_ALSA
#include "player/alsaPlayer.h"
#endif
#ifdef HAS_DAEMON
#include "common/daemon.h"
#endif
#include "aixlog.hpp"
#include "common/signalHandler.h"
#include "common/strCompat.h"
#include "common/utils.h"


using namespace std;
using namespace popl;

volatile sig_atomic_t g_terminated = false;

PcmDevice getPcmDevice(const std::string& soundcard)
{
#ifdef HAS_ALSA
	vector<PcmDevice> pcmDevices = AlsaPlayer::pcm_list();

	try
	{
		int soundcardIdx = cpt::stoi(soundcard);
		for (auto dev: pcmDevices)
			if (dev.idx == soundcardIdx)
				return dev;
	}
	catch(...)
	{
	}

	for (auto dev: pcmDevices)
		if (dev.name.find(soundcard) != string::npos)
			return dev;
#endif
	PcmDevice pcmDevice;
	return pcmDevice;
}


int main (int argc, char **argv)
{
#ifdef MACOS
#pragma message "Warning: the macOS support is experimental and might not be maintained"
#endif
	int exitcode = EXIT_SUCCESS;
	try
	{
		string soundcard("default");
		string host("");
		size_t port(1704);
		int latency(0);
		size_t instance(1);

		OptionParser op("Allowed options");
		auto helpSwitch =     op.add<Switch>("", "help", "produce help message");
		auto debugSwitch =    op.add<Switch, Attribute::hidden>("", "debug", "enable debug logging");
		auto versionSwitch =  op.add<Switch>("v", "version", "show version number");
#if defined(HAS_ALSA)
		auto listSwitch =     op.add<Switch>("l", "list", "list pcm devices");
		/*auto soundcardValue =*/ op.add<Value<string>>("s", "soundcard", "index or name of the soundcard", "default", &soundcard);
#endif
		/*auto hostValue =*/  op.add<Value<string>>("h", "host", "server hostname or ip address", "", &host);
		/*auto portValue =*/  op.add<Value<size_t>>("p", "port", "server port", 1704, &port);
#ifdef HAS_DAEMON
		int processPriority(-3);
		auto daemonOption =   op.add<Implicit<int>>("d", "daemon", "daemonize, optional process priority [-20..19]", -3, &processPriority);
		auto userValue =      op.add<Value<string>>("", "user", "the user[:group] to run snapclient as when daemonized");
#endif
		/*auto latencyValue =*/   op.add<Value<int>>("", "latency", "latency of the soundcard", 0, &latency);
		/*auto instanceValue =*/  op.add<Value<size_t>>("i", "instance", "instance id", 1, &instance);
		auto hostIdValue =    op.add<Value<string>>("", "hostID", "unique host id", "");

		try
		{
			op.parse(argc, argv);
		}
		catch (const std::invalid_argument& e)
		{
			cerr << "Exception: " << e.what() << std::endl;
			cout << "\n" << op << "\n";
			exit(EXIT_FAILURE);
		}

		if (versionSwitch->is_set())
		{
			cout << "snapclient v" << VERSION << "\n"
				<< "Copyright (C) 2014-2017 BadAix (snapcast@badaix.de).\n"
				<< "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n"
				<< "This is free software: you are free to change and redistribute it.\n"
				<< "There is NO WARRANTY, to the extent permitted by law.\n\n"
				<< "Written by Johannes M. Pohl.\n\n";
			exit(EXIT_SUCCESS);
		}

#ifdef HAS_ALSA
		if (listSwitch->is_set())
		{
			vector<PcmDevice> pcmDevices = AlsaPlayer::pcm_list();
			for (auto dev: pcmDevices)
			{
				cout << dev.idx << ": " << dev.name << "\n"
					<< dev.description << "\n\n";
			}
			exit(EXIT_SUCCESS);
		}
#endif

		if (helpSwitch->is_set())
		{
			cout << op << "\n";
			exit(EXIT_SUCCESS);
		}

		if (instance <= 0)
			std::invalid_argument("instance id must be >= 1");

		AixLog::Log::init(
			{
				make_shared<AixLog::SinkCout>(debugSwitch->is_set()?(AixLog::Severity::trace):(AixLog::Severity::info), AixLog::Type::all, debugSwitch->is_set()?"%Y-%m-%d %H-%M-%S.#ms [#severity] (#function)":"%Y-%m-%d %H-%M-%S [#severity]"),
				make_shared<AixLog::SinkNative>("snapclient", AixLog::Severity::trace, AixLog::Type::special)
			}
		);

		signal(SIGHUP, signal_handler);
		signal(SIGTERM, signal_handler);
		signal(SIGINT, signal_handler);

#ifdef HAS_DAEMON
		std::unique_ptr<Daemon> daemon;
		if (daemonOption->is_set())
		{
			string pidFile = "/var/run/snapclient/pid";
			if (instance != 1)
				pidFile += "." + cpt::to_string(instance);
			string user = "";
			string group = "";

			if (userValue->is_set())
			{
				if (userValue->value().empty())
					std::invalid_argument("user must not be empty");

				vector<string> user_group = utils::string::split(userValue->value(), ':');
				user = user_group[0];
				if (user_group.size() > 1)
					group = user_group[1];
			}
			daemon.reset(new Daemon(user, group, pidFile));
			daemon->daemonize();
			if (processPriority < -20)
				processPriority = -20;
			else if (processPriority > 19)
				processPriority = 19;
			if (processPriority != 0)
				setpriority(PRIO_PROCESS, 0, processPriority);
			SLOG(NOTICE) << "daemon started" << std::endl;
		}
#endif

		PcmDevice pcmDevice = getPcmDevice(soundcard);
#if defined(HAS_ALSA)
		if (pcmDevice.idx == -1)
		{
			cout << "soundcard \"" << soundcard << "\" not found\n";
//			exit(EXIT_FAILURE);
		}
#endif

		if (host.empty())
		{
#if defined(HAS_AVAHI) || defined(HAS_BONJOUR)
			BrowseZeroConf browser;
			mDNSResult avahiResult;
			while (!g_terminated)
			{
				try
				{
					if (browser.browse("_snapcast._tcp", avahiResult, 5000))
					{
						host = avahiResult.ip_;
						port = avahiResult.port_;
						LOG(INFO) << "Found server " << host << ":" << port << "\n";
						break;
					}
				}
				catch (const std::exception& e)
				{
					SLOG(ERROR) << "Exception: " << e.what() << std::endl;
				}
				chronos::sleep(500);
			}
#endif
		}

		std::unique_ptr<Controller> controller(new Controller(hostIdValue->value(), instance));
		if (!g_terminated)
		{
			LOG(INFO) << "Latency: " << latency << "\n";
			controller->start(pcmDevice, host, port, latency);
			while(!g_terminated)
				chronos::sleep(100);
			controller->stop();
		}
	}
	catch (const std::exception& e)
	{
		SLOG(ERROR) << "Exception: " << e.what() << std::endl;
		exitcode = EXIT_FAILURE;
	}

	SLOG(NOTICE) << "daemon terminated." << endl;
	exit(exitcode);
}


