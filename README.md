# QuantumSync Local V2

Standalone music player for Raspberry Pi. Plays local music via MPD with a
web GUI on port **1706** for volume, playback and music-folder control.

## What's new in V2

- **Network share** — the music folder is shared on the LAN via Samba as
  `\\<pi-ip>\music`. You pick the login username during install (defaults to
  `pi` when that user exists) plus a share password. Copy music straight in
  from any PC on the network.
- **Folder selection** — make subfolders inside the music folder (e.g.
  `Day to Day`, `Christmas`) and pick which one plays from a dropdown in the
  web GUI. "All Music" plays everything. The choice survives reboots.
- **Library rescan button** in the GUI to pick up newly copied files on demand.
- Fixed systemd dependency (`quantumsync-local.service` now waits for the
  correct MPD unit) and the logs page now shows the right MPD unit's journal.

## Install

```bash
git clone https://github.com/quantum-forge-consulting-za/quantum_sync_local_v2.git
cd quantum_sync_local_v2
sudo ./install.sh
```

The installer cleans up any old QuantumSync (V1 / streaming client) install,
builds the C++ server, configures MPD and Samba, and starts everything.

## Using it

- Web GUI: `http://<pi-ip>:1706/`
- Add music: open `\\<pi-ip>\music` in File Explorer, log in with the share
  username and password you chose at install, and copy files/folders in.
- Group music by making subfolders — each top-level subfolder becomes a
  selectable "Music Folder" in the GUI. Files in the root of the share belong
  to "All Music" only.
- Logs: `http://<pi-ip>:1706/logs`

## Components

| Path | Purpose |
| --- | --- |
| `src/` | C++17 HTTP server (Boost.Asio) with embedded web GUI |
| `config/mpd.conf` | Dedicated MPD instance (localhost:6600, ALSA headphone jack) |
| `systemd/` | `quantumsync-local-mpd` (MPD) and `quantumsync-local` (GUI) services |
| `scripts/quantumsync-restore-queue` | Restores the selected folder's queue at boot |
| `install.sh` | Full installer (deps, build, MPD, Samba, services) |

## API

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/api/status` | GET | Track, state, volume, folder, system stats |
| `/api/volume` | POST | `{"volume": 0-100}` |
| `/api/mute` | POST | `{"muted": true/false}` |
| `/api/playback` | POST | `{"action": "play\|pause\|toggle\|next\|prev"}` |
| `/api/folders` | GET | List top-level music folders + current selection |
| `/api/folder` | POST | `{"folder": "Christmas"}` (empty string = All Music) |
| `/api/update` | POST | Rescan the music library |
| `/api/journal` | GET | Last 100 journal lines |
