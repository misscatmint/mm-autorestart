# mm-autorestart

A **Metamod:Source plugin** (CS:GO, MM:S 1.12 and 2.0) that auto-restarts
a server at a configured daily time.

## Behaviour

Every 10 seconds the plugin checks whether a restart is warranted:

- **Daily restart** - the current UTC time has passed `autorestart_time`
  (once per day).

When a restart is warranted:

- If there are **no players**, it runs `quit`.
- If a player disconnects after the daily restart is triggered, they have 30
  seconds to rejoin before the restart goes through.
- While the server **hibernates** (`sv_hibernate_when_empty`), `GameFrame`
  stops running, so a background thread polls every 30 seconds and sends the
  process `SIGTERM` if a restart is pending or due.
- If shutdown hangs for 60 seconds, the process is force-exited.

## Configuration

Server convars:

| Env var              | Required | Description                                                        |
| -------------------- | -------- | ------------------------------------------------------------------ |
| `autorestart_time` | no       | UTC time `HH:mm` (or `HH:mm:ss`) for a daily restart.              |

## Building

MM:S 2.0 replaced SourceHook with KHook, so there is one build per
Metamod:Source version. The source is shared; `METAMOD_PLAPI_VERSION` picks
the hook API. Linux x86 only.

```sh
git submodule update --init --recursive
docker compose up --build
```

Result:

```text
output/mm-1.12/addons/autorestart/bin/autorestart.so
output/mm-1.12/addons/metamod/autorestart.vdf
output/mm-2.0/addons/autorestart/bin/autorestart.so
output/mm-2.0/addons/metamod/autorestart.vdf
```

Use the package matching the server's Metamod:Source version and drop its
`addons/` into `csgo/addons/`.
