# mm-autorestart

A **Metamod:Source plugin** (CS:GO, MM:S 1.12 and 2.0) that auto-restarts
a server once it has been up for a set time and is empty.

## Behaviour

Every 10 seconds the plugin checks whether a restart is warranted:

- **Uptime restart** - the plugin has been loaded for `autorestart_uptime`
  minutes. Uptime counts from when the plugin loads, which is when the
  server starts, or a `meta load` or `meta reload` of this plugin. Map
  changes and `meta refresh` don't reset it.

When a restart is warranted:

- If there are **no players**, it runs `quit` after `autorestart_grace`
  seconds.
- If a player disconnects after the restart comes due, they have
  `autorestart_grace` seconds to rejoin before the restart goes through.
- While the server **hibernates** (`sv_hibernate_when_empty`), `GameFrame`
  stops running, so a background thread polls every 30 seconds and sends the
  process `SIGTERM` if a restart is pending or due, once `autorestart_grace`
  seconds have passed since the last player left.
- If the plugin is loaded while the server is already hibernating, it can't
  tell, so a restart that comes due waits until the server next wakes up.
- If shutdown hangs for 60 seconds, the process is force-exited.

## Configuration

Server convars:

| Cvar                 | Required | Description                                                                                                                                                                 |
| -------------------- | -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `autorestart_uptime` | no       | Minutes of uptime (0 to 525600, default 0 = off) after which a restart comes due. Changing it cancels a pending restart, which comes due again if the new limit is reached. |
| `autorestart_grace`  | no       | Seconds (0 to 86400, default 30) an empty server waits before a due restart, so players can rejoin. A restart already counting down keeps its deadline.                     |

## Building

MM:S 2.0 replaced SourceHook with KHook, so there is one build per
Metamod:Source version. The source is shared; `METAMOD_PLAPI_VERSION` picks
the hook API. Linux x86 only.

```sh
git submodule update --init --recursive
make
```

`make dist` also zips each package into `dist/`, `make lint` runs
clang-tidy, and `make check` does both lint and build.

Result:

```text
output/mm-1.12/addons/autorestart/bin/autorestart.so
output/mm-1.12/addons/metamod/autorestart.vdf
output/mm-2.0/addons/autorestart/bin/autorestart.so
output/mm-2.0/addons/metamod/autorestart.vdf
```

Use the package matching the server's Metamod:Source version and drop its
`addons/` into `csgo/addons/`.
