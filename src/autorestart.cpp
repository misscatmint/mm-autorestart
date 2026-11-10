#include "autorestart.h"

#include <convar.h>
#include "tier0/dbg.h"
#include "tier0/platform.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

AutoRestartPlugin g_AutoRestartPlugin;

IVEngineServer *engine = nullptr;
IServerGameDLL *server = nullptr;
IServerGameClients *gameclients = nullptr;
CGlobalVars *gpGlobals = nullptr;

static void OnAutoRestartUptimeChanged(IConVar *var, const char *pOldValue, float flOldValue);

ConVar autorestart_uptime("autorestart_uptime", "0", FCVAR_NONE, "minutes of uptime after which the server restarts once empty (0 = off)", true, 0.0f, true, 525600.0f, OnAutoRestartUptimeChanged);
ConVar autorestart_grace("autorestart_grace", "30", FCVAR_NONE, "seconds a due restart waits on an empty server for players to rejoin", true, 0.0f, true, 86400.0f);

PLUGIN_EXPOSE(AutoRestartPlugin, g_AutoRestartPlugin);

#ifndef AUTORESTART_KHOOK
SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, edict_t *);
SH_DECL_HOOK1_void(IServerGameDLL, ServerHibernationUpdate, SH_NOATTRIB, 0, bool);
#endif

static const int kWatcherIntervalSeconds = 30;    // background poll cadence while hibernating
static const int kQuitTimeoutSeconds = 60;

// Engine shutdown can wedge after plugins unload (another plugin's thread, Steam, etc.),
// leaving a dead server. Force an exit so the container's restart policy relaunches us.
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

static void OnAutoRestartUptimeChanged(IConVar *var, const char *pOldValue, float flOldValue)
{
	// tier1 runs change callbacks on every set, and server.cfg sets this again on each map load.
	if (static_cast<int>(flOldValue) == autorestart_uptime.GetInt())
	{
		return;
	}
	g_AutoRestartPlugin.UpdateUptimeLimit();
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

	META_REGCVAR(&autorestart_uptime);
	META_REGCVAR(&autorestart_grace);

	// The .so is nodelete, so a reload reuses this object with whatever the last load left behind.
	m_hibernating = false;
	m_stopWatcher = false;
	m_lastCheckTime = 0.0;
	m_loadTime = std::chrono::steady_clock::now();
	UpdateUptimeLimit();

#ifdef AUTORESTART_KHOOK
	m_GameFrame.Add(server);
	m_ClientDisconnect.Add(gameclients);
	m_ServerHibernationUpdate.Add(server);
#else
	SH_ADD_HOOK(IServerGameDLL, GameFrame, server, SH_MEMBER(this, &AutoRestartPlugin::Hook_GameFrame), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, gameclients, SH_MEMBER(this, &AutoRestartPlugin::Hook_ClientDisconnect), true);
	SH_ADD_HOOK(IServerGameDLL, ServerHibernationUpdate, server, SH_MEMBER(this, &AutoRestartPlugin::Hook_ServerHibernationUpdate), true);
#endif

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

void AutoRestartPlugin::UpdateUptimeLimit()
{
	// A new limit cancels a restart that's already waiting; the next check makes it due again if the limit is still reached.
	m_scheduledRestartNeeded = false;
	m_restartNeeded = false;
	m_quitPending = false;

	int minutes = autorestart_uptime.GetInt();
	m_uptimeLimitSeconds = minutes * 60;
	if (minutes > 0)
	{
		Msg("[autorestart] restarting after %d minutes of uptime\n", minutes);
	}
}

bool AutoRestartPlugin::CheckUptimeRestart() const
{
	int limit = m_uptimeLimitSeconds;
	if (limit <= 0 || m_scheduledRestartNeeded)
	{
		return false;
	}
	return std::chrono::steady_clock::now() - m_loadTime >= std::chrono::seconds(limit);
}

int AutoRestartPlugin::CountHumanPlayers() const
{
	// Bots have no net channel (unless sv_stressbots gives them one), so only real (incl. still-connecting) clients count.
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
	m_quitIssued = true;
}

void AutoRestartPlugin::CheckAndRestart()
{
	if (CheckUptimeRestart())
	{
		m_scheduledRestartNeeded = true;
	}

	if (!m_scheduledRestartNeeded)
	{
		return;
	}

	int numPlayers = CountHumanPlayers();
	if (numPlayers == 0)
	{
		if (!m_quitPending)
		{
			int grace = autorestart_grace.GetInt();
			m_quitAtTime = static_cast<int>(Plat_FloatTime()) + grace;
			m_quitPending = true;
			Msg("[autorestart] server empty, restarting in %d seconds\n", grace);
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
	if (m_quitIssued)
	{
		return;
	}

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
	int leaving = pEntity ? static_cast<int>(pEntity - gpGlobals->pEdicts) : -1;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (i != leaving && engine->GetPlayerNetInfo(i) != nullptr)
		{
			return;
		}
	}

	if (m_quitPending)
	{
		return;
	}
	// Set even before a restart is due: if the server hibernates first, the watcher only has this deadline to go by.
	int grace = autorestart_grace.GetInt();
	m_quitAtTime = static_cast<int>(Plat_FloatTime()) + grace;
	if (m_scheduledRestartNeeded)
	{
		m_quitPending = true;
		Msg("[autorestart] last player left, restarting if they don't rejoin in %d seconds\n", grace);
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
		if (m_hibernating && (m_quitPending || m_scheduledRestartNeeded || CheckUptimeRestart()))
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
