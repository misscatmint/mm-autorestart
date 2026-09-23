#include "autorestart.h"

#include <convar.h>
#include <inetchannelinfo.h>
#include "tier0/dbg.h"
#include "tier0/platform.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

AutoRestartPlugin g_AutoRestartPlugin;

IVEngineServer *engine = nullptr;
IServerGameDLL *server = nullptr;
IServerGameClients *gameclients = nullptr;
CGlobalVars *gpGlobals = nullptr;

static void OnAutoRestartTimeChanged(IConVar *var, const char *pOldValue, float flOldValue);

ConVar autorestart_time("autorestart_time", "", FCVAR_NONE, "daily restart time in UTC (HH:mm or HH:mm:ss)", OnAutoRestartTimeChanged);

PLUGIN_EXPOSE(AutoRestartPlugin, g_AutoRestartPlugin);

#ifndef AUTORESTART_KHOOK
SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, edict_t *);
SH_DECL_HOOK1_void(IServerGameDLL, ServerHibernationUpdate, SH_NOATTRIB, 0, bool);
#endif

static const int kWatcherIntervalSeconds = 30;    // background poll cadence while hibernating
static const int kQuitTimeoutSeconds = 60;

// Engine shutdown can wedge after plugins unload (another plugin's thread, Steam, etc.),
// leaving a dead server. Force exit 0 so its loop restarts us.
// The thread only touches its own stack, and the .so is linked nodelete, so plugin unload is safe.
static void ArmQuitWatchdog()
{
	static std::atomic<bool> armed {false};
	if (armed.exchange(true))
	{
		return;
	}
	std::thread(
		[]
		{
			std::this_thread::sleep_for(std::chrono::seconds(kQuitTimeoutSeconds));
			static const char msg[] = "[autorestart] shutdown stalled, forcing exit\n";
			std::fwrite(msg, 1, sizeof(msg) - 1, stderr);
			std::fflush(stderr);
			std::_Exit(0);
		})
		.detach();
}

static std::string Trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
	{
		return "";
	}
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

static void OnAutoRestartTimeChanged(IConVar *var, const char *pOldValue, float flOldValue)
{
	g_AutoRestartPlugin.UpdateRestartTime();
}

AutoRestartPlugin::AutoRestartPlugin()
#ifdef AUTORESTART_KHOOK
	: m_GameFrame(&IServerGameDLL::GameFrame, this, nullptr, &AutoRestartPlugin::Hook_GameFrame),
	  m_ClientDisconnect(&IServerGameClients::ClientDisconnect, this, nullptr, &AutoRestartPlugin::Hook_ClientDisconnect),
	  m_ServerHibernationUpdate(&IServerGameDLL::ServerHibernationUpdate, this, nullptr, &AutoRestartPlugin::Hook_ServerHibernationUpdate)
#endif
{
}

bool AutoRestartPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_ANY(GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	gpGlobals = ismm->GetCGlobals();

	META_REGCVAR(&autorestart_time);

	UpdateRestartTime();

#ifdef AUTORESTART_KHOOK
	m_GameFrame.Add(server);
	m_ClientDisconnect.Add(gameclients);
	m_ServerHibernationUpdate.Add(server);
#else
	SH_ADD_HOOK(IServerGameDLL, GameFrame, server, SH_MEMBER(this, &AutoRestartPlugin::Hook_GameFrame), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, gameclients, SH_MEMBER(this, &AutoRestartPlugin::Hook_ClientDisconnect), true);
	SH_ADD_HOOK(IServerGameDLL, ServerHibernationUpdate, server, SH_MEMBER(this, &AutoRestartPlugin::Hook_ServerHibernationUpdate), true);
#endif

	// 0.0 forces a check on the first frame
	m_lastCheckTime = 0.0;

	// Watch for updates while the server hibernates, where GameFrame is frozen.
	m_watcherThread = std::thread(&AutoRestartPlugin::WatcherLoop, this);

	Msg("[autorestart] loaded\n");

	return true;
}

bool AutoRestartPlugin::Unload(char *error, size_t maxlen)
{
	// Stop the watcher thread before tearing down hooks.
	{
		std::lock_guard<std::mutex> lock(m_watcherMutex);
		m_stopWatcher = true;
	}
	m_watcherCv.notify_all();
	if (m_watcherThread.joinable())
	{
		m_watcherThread.join();
	}

#ifdef AUTORESTART_KHOOK
	m_GameFrame.Remove(server);
	m_ClientDisconnect.Remove(gameclients);
	m_ServerHibernationUpdate.Remove(server);
#else
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, server, SH_MEMBER(this, &AutoRestartPlugin::Hook_GameFrame), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, gameclients, SH_MEMBER(this, &AutoRestartPlugin::Hook_ClientDisconnect), true);
	SH_REMOVE_HOOK(IServerGameDLL, ServerHibernationUpdate, server, SH_MEMBER(this, &AutoRestartPlugin::Hook_ServerHibernationUpdate), true);
#endif
	return true;
}

void AutoRestartPlugin::UpdateRestartTime()
{
	const char *dailyStr = autorestart_time.GetString();
	if (!dailyStr || !*dailyStr)
	{
		m_hasDailyRestart = false;
		return;
	}

	std::string trimmed = Trim(dailyStr);
	int hh = 0, mm = 0, ss = 0;
	int n = std::sscanf(trimmed.c_str(), "%d:%d:%d", &hh, &mm, &ss);
	if (n >= 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss < 60)
	{
		m_dailyRestartSeconds = hh * 3600 + mm * 60 + ss;
		m_hasDailyRestart = true;

		time_t now = time(nullptr);
		int today = static_cast<int>(now / 86400);
		int secOfDay = static_cast<int>(now % 86400);
		m_lastDailyRestartDay = (secOfDay >= m_dailyRestartSeconds) ? today : today - 1;
		Msg("[autorestart] daily restart time set to %s\n", trimmed.c_str());
	}
	else
	{
		m_hasDailyRestart = false;
	}
}

bool AutoRestartPlugin::CheckDailyRestart() const
{
	if (!m_hasDailyRestart || m_scheduledRestartNeeded)
	{
		return false;
	}

	time_t now = time(nullptr);
	int today = static_cast<int>(now / 86400);    // days since epoch (UTC)
	int secOfDay = static_cast<int>(now % 86400); // seconds since UTC midnight
	
	return today > m_lastDailyRestartDay && secOfDay >= m_dailyRestartSeconds;
}

int AutoRestartPlugin::CountHumanPlayers() const
{
	// Bots have no net channel, so only real (incl. still-connecting) clients count.
	int count = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (engine->GetPlayerNetInfo(i) != nullptr)
		{
			count++;
		}
	}
	return count;
}

void AutoRestartPlugin::QuitNow()
{
	Msg("[autorestart] restarting server\n");
	ArmQuitWatchdog();
	engine->ServerCommand("quit\n");
}

void AutoRestartPlugin::CheckAndRestart()
{
	bool isDailyRestartDue = CheckDailyRestart();

	if (!(isDailyRestartDue || m_scheduledRestartNeeded))
	{
		return;
	}

	if (isDailyRestartDue && !m_scheduledRestartNeeded)
	{
		m_scheduledRestartNeeded = true;
		m_lastDailyRestartDay = static_cast<int>(time(nullptr) / 86400);
	}

	int numPlayers = CountHumanPlayers();
	if (numPlayers == 0)
	{
		if (!m_quitPending)
		{
			m_quitAtTime = Plat_FloatTime() + 30.0;
			m_quitPending = true;
			Msg("[autorestart] server empty, restarting in 30 seconds\n");
		}
	}
	else
	{
		// If someone rejoined during the grace period, cancel any pending quit
		m_quitPending = false;
		if (!m_restartNeeded)
		{
			m_restartNeeded = true;
			Msg("[autorestart] %d player(s) online, will restart once empty\n", numPlayers);
		}
	}
}

void AutoRestartPlugin::OnGameFrame()
{
	double now = Plat_FloatTime();

	if (m_quitPending && now >= m_quitAtTime)
	{
		CheckAndRestart();
		if (m_quitPending)
		{
			QuitNow();
			return;
		}
		else
		{
			m_lastCheckTime = now;
			return;
		}
	}

	if (now - m_lastCheckTime < 10.0)
	{
		return;
	}

	m_lastCheckTime = now;
	CheckAndRestart();
}

void AutoRestartPlugin::OnClientDisconnect(edict_t *pEntity)
{
	if (!(m_restartNeeded || m_scheduledRestartNeeded))
	{
		return;
	}

	int leaving = pEntity ? static_cast<int>(pEntity - gpGlobals->pEdicts) : -1;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (i != leaving && engine->GetPlayerNetInfo(i) != nullptr)
		{
			return;
		}
	}

	if (!m_quitPending)
	{
		m_quitAtTime = Plat_FloatTime() + 30.0;
		m_quitPending = true;
		Msg("[autorestart] last player left, restarting if they don't rejoin in 30 seconds\n");
	}
}

#ifdef AUTORESTART_KHOOK
KHook::Return<void> AutoRestartPlugin::Hook_GameFrame(IServerGameDLL *, bool simulating)
{
	OnGameFrame();
	return {KHook::Action::Ignore};
}

KHook::Return<void> AutoRestartPlugin::Hook_ClientDisconnect(IServerGameClients *, edict_t *pEntity)
{
	OnClientDisconnect(pEntity);
	return {KHook::Action::Ignore};
}

KHook::Return<void> AutoRestartPlugin::Hook_ServerHibernationUpdate(IServerGameDLL *, bool bHibernating)
{
	m_hibernating = bHibernating;
	return {KHook::Action::Ignore};
}
#else
void AutoRestartPlugin::Hook_GameFrame(bool simulating)
{
	OnGameFrame();
	RETURN_META(MRES_IGNORED);
}

void AutoRestartPlugin::Hook_ClientDisconnect(edict_t *pEntity)
{
	OnClientDisconnect(pEntity);
	RETURN_META(MRES_IGNORED);
}

void AutoRestartPlugin::Hook_ServerHibernationUpdate(bool bHibernating)
{
	m_hibernating = bHibernating;
	RETURN_META(MRES_IGNORED);
}
#endif

void AutoRestartPlugin::WatcherLoop()
{
	for (;;)
	{
		{
			std::unique_lock<std::mutex> lock(m_watcherMutex);
			m_watcherCv.wait_for(lock, std::chrono::seconds(kWatcherIntervalSeconds), [this] { return m_stopWatcher.load(); });
			if (m_stopWatcher.load())
			{
				return;
			}
		}

		// A hibernating server is empty, so any pending or due restart can happen right away.
		if (m_hibernating && (m_quitPending || m_scheduledRestartNeeded || CheckDailyRestart()))
		{
			double now = Plat_FloatTime();
			if (now >= m_quitAtTime)
			{
				Msg("[autorestart] restarting hibernating server\n");
				ArmQuitWatchdog();
				kill(getpid(), SIGTERM);
				return;
			}
		}
	}
}
