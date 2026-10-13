# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-10-13

### Added
- Live real-time dashboard (stays open, flicker-free) listing listening TCP/UDP
  ports with owning process and its working directory (`Dir`).
- Windows support (GetExtendedTcpTable + remote PEB CWD) and Linux support
  (`/proc/net/tcp[6]` + `/proc/<pid>/cwd`).
- CLI output formats: `--json`, `--yaml`, `--csv`, `--once`/`--static`, `-w` watch.
- `kill <port> [-f]` to kill the process listening on a port.
- `wait <port> [--timeout N]` to block until a port comes up.
- `find [start] [--count N]` to print free port(s).
- Smart fuzzy search (`nxport <text>`) by process name or Dir.
- Port / range filtering (`nxport 3000`, `3000-3010`, `3000..3010`).
- Health check (`-p/--ping`, `[p]` in dashboard): HTTP status / TCP probe.
- Dashboard: `[k]` kill, `[Enter]`/`[o]` open in browser, `[s]` settings,
  `[r]` refresh, `/` search, `c` sort cycling, watch-diff `[NEW]`/`[GONE]` markers.
- Settings (persisted): hidden-folder blacklist, Server History with
  start/stop commands and auto-detection (`package.json`, `manage.py`,
  `artisan`, `Cargo.toml`, `go.mod`, `mix.exs`), UDP toggle, all-states
  toggle, refresh interval, reset.
- NxCode IDE integration: resolves ports served in-process by the IDE to the
  real project folder (QSettings `server/entries`).
- VS Code / NxCode extension (`vscode-nxport/`): ports tree view, status bar,
  open / kill / copy actions.
- Tech-stack sniffing (`[Express]`, `[Next]`, `[Docker]`, `[WSL]`, `[PUB!]`…).
- `--color` / `--no-color`, `-V/--version`, `-h/--help`.
