/**
 * AutoRestart - Metamod:Source plugin (CS:GO, MM:S 1.12 and 2.0)
 *
 * Restarts the server daily
 */

#pragma once

#include <ISmmPlugin.h>
#include <eiface.h>

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>

// MM:S 2.0 (plugin API 18) replaced SourceHook with KHook; 1.12 is API 16.
#if METAMOD_PLAPI_VERSION >= 18
#define AUTORESTART_KHOOK
#endif

class AutoRestartPlugin : public ISmmPlugin
{
public:
	AutoRestartPlugin();

	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
	bool Unload(char *error, size_t maxlen) override;

public: // hooks
#ifdef AUTORESTART_KHOOK
	KHook::Return<void> Hook_GameFrame(IServerGameDLL *, bool simulating);
	KHook::Return<void> Hook_ClientDisconnect(IServerGameClients *, edict_t *pEntity);
	KHook::Return<void> Hook_ServerHibernationUpdate(IServerGameDLL *, bool bHibernating);
#else
	void Hook_GameFrame(bool simulating);
	void Hook_ClientDisconnect(edict_t *pEntity);
	void Hook_ServerHibernationUpdate(bool bHibernating);
#endif

public: // ISmmPlugin metadata
	const char *GetAuthor() override
	{
		return "jvnipers, catmint";
	}

	const char *GetName() override
	{
		return "autorestart";
	}

	const char *GetDescription() override
	{
		return "Auto-restart the server daily";
	}

	const char *GetURL() override
	{
		return "https://github.com/misscatmint/mm-autorestart";
	}

	const char *GetLicense() override
	{
		return "AGPL-3.0";
	}

	const char *GetVersion() override
	{
		return "3.0.1";
	}

	const char *GetDate() override
	{
		return __DATE__;
	}

	const char *GetLogTag() override
	{
		return "autorestart";
	}

	void UpdateRestartTime();
private:
	// Engine-agnostic hook bodies, called from the KHook/SourceHook wrappers.
	void OnGameFrame();
	void OnClientDisconnect(edict_t *pEntity);

	void CheckAndRestart();
	bool IsServerOutOfDate();
	bool CheckDailyRestart() const;
	int CountHumanPlayers() const;
	void PrintToChatAll(const char *msg);
	void QuitNow();

	std::map<std::string, std::string> ReadPluginVersions() const;

	// While the server hibernates GameFrame is frozen, so the normal restart path can't run.
	// This thread polls for a pending/due restart and, while hibernating,
	// signals the process to shut down for relaunch. It never calls into the engine.
	void WatcherLoop();

	bool m_restartNeeded = false;
	std::atomic<bool> m_scheduledRestartNeeded {false};

	bool m_hasDailyRestart = false;
	int m_dailyRestartSeconds = 0;               // seconds since UTC midnight
	std::atomic<int> m_lastDailyRestartDay {-1}; // days since unix epoch (UTC) of last daily restart

	double m_lastCheckTime = 0.0; // Plat_FloatTime() of last 10s tick
	std::atomic<bool> m_quitPending {false};
	double m_quitAtTime = 0.0; // Plat_FloatTime() at which to issue the deferred quit

	// Background watcher state. m_hibernating is the engine's hibernation signal (set from Hook_ServerHibernationUpdate);
	// the thread only acts while it's true.
	std::atomic<bool> m_hibernating {false};
	std::atomic<bool> m_stopWatcher {false};
	std::thread m_watcherThread;
	std::mutex m_watcherMutex;
	std::condition_variable m_watcherCv;

#ifdef AUTORESTART_KHOOK
	KHook::Virtual<IServerGameDLL, void, bool> m_GameFrame;
	KHook::Virtual<IServerGameClients, void, edict_t *> m_ClientDisconnect;
	KHook::Virtual<IServerGameDLL, void, bool> m_ServerHibernationUpdate;
#endif
};

extern AutoRestartPlugin g_AutoRestartPlugin;

PLUGIN_GLOBALVARS();
