# Hopline

![A selected latency spike in two synchronized timelines raises the average and loss in the corresponding hop rows.](docs/trace.png)

Hopline is a native Linux continuous traceroute client. It records IPv4 and IPv6 probes, plots latency and loss at each hop, and lets you investigate an earlier incident while collection continues.

## Build and run

Install a C++20 compiler, make, pkg-config, Qt 6 Base and Declarative development files (including qmake6, Quick Controls and Quick Dialogs), and the SQLite 3 development files. Build from the project directory.

```sh
bin/build
./build/hopline
```

Enter a hostname or IP address, select the address family and interval, and press **Start trace**. Auto uses a routable address returned by the system resolver. To compare IPv4 and IPv6 for a dual-stack hostname, start one trace for each family. Each tab collects independently. A link-local IPv6 target needs its interface, for example `fe80::1%eth0`.

```sh
./build/hopline --demo
./build/hopline --family 6 ::1
./build/hopline --report 127.0.0.1 --family 4 --interval 500 --seconds 10
```

The demo opens a disposable recording containing a brief outage. The headless report writes JSON to stdout. Duration controls how long new rounds start; in-flight rounds have time to receive replies or time out before the report prints.

On Arch Linux, `bin/package` builds a pacman package and a source package in `pkgbuild/`. Installing the binary package adds Hopline to the desktop launcher.

## Investigate a trace

- **Read across a hop row.** The horizontal span is the lowest to highest RTT in the focus period. The connected dots show averages, and the cross marks the last sample when it received a reply. Sent and loss count timeouts. Latency statistics use replies only. Jitter is the mean absolute difference between consecutive successful RTTs.
- **Click a hop** to toggle its timeline. All visible timelines share a time axis. The destination timeline follows the target when its hop number changes. Per-hop timelines follow the selected hop number. A change of responding address is flagged in the focused hop table.
- **Click a spike** to inspect that point in history. The upper table recalculates over the focus window, using the responders observed then. Auto focus covers the visible timeline. All focus covers the recording. Drag across any timeline to fill all timelines with that time range and recompute the hop table for the selection. Shift-drag or use the arrow buttons to pan. Ctrl+wheel and the View picker change the displayed duration. The horizontal amber and red timeline bands mark RTTs above 100 and 200 ms, respectively. **Go live** returns to the current end of a running trace.
- **Read loss downstream.** A router can rate-limit its own replies. Loss at one intermediate hop with a healthy destination has a different meaning from loss that persists to the destination.
- **Right-click a timeline** to add a timestamped note. Right-click an existing marker to edit or remove it. **Save PNG** exports the visible graphs. Ctrl+L focuses the target field and Ctrl+Shift+S opens the image export dialog.
- **Resize the interface** with Ctrl++ (or Ctrl+=) and Ctrl+−. Ctrl+0 restores 100%. Hopline remembers the scale across launches.
- **Close a trace tab** with its × button or a middle-click. Closing stops collection and keeps the recording. Use the closed-traces menu to reopen it, including after restarting Hopline.
- **Pause** stops collection for that tab. Resume appends to the same session. Switching tabs preserves the current focus and pinned hops for the duration of the app session. Closing the app stops all collectors; reopening it restores the recordings for review or manual resume.

## Measurement and storage

The collector uses nonblocking Linux sockets on a dedicated worker thread. It prefers unprivileged ICMP echo sockets and falls back to UDP traceroute if the kernel disallows them. Each round sends increasing IPv4 TTLs or IPv6 hop limits toward the target and correlates replies with individual probes. The path limit is 30 hops and the reply timeout is 1.5 seconds. The collector uses kernel receive timestamps when available and coherent with the send clock.

History lives in `${XDG_DATA_HOME:-~/.local/share}/hopline/history.sqlite`. Use `--database PATH` for a separate recording collection. Raw data is kept until the database is removed. To archive a database, close Hopline first or use SQLite's backup facility.

A writer batches samples into SQLite WAL transactions. An independent reader combines raw samples with 10-second, minute, and hour summaries. Zoomed-out pixels retain extrema and loss, and focus statistics include the raw edges of the selected interval. The Qt Quick renderer receives only the displayed rows and timeline buckets. Hardware rendering uses scene-graph geometry; a software rendering path is also provided.

## Verify

```sh
bin/test
tests/probe-netns.sh
build-tests/history/history_test --benchmark
QT_QPA_PLATFORMTHEME=generic QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  HOPLINE_LIVE_TEST=1 build-tests/integration/integration_test replayWhileProbing
```

The regular suite checks historical statistics, reroutes, notes, session state and the real Qt window. The namespace test sets up an isolated two-hop network and checks IPv4, IPv6, both probe transports, link-local addressing and timeout accounting through the CLI. It requires user/network namespaces, iproute2, util-linux and Python 3. The last test replays 25 hours of recorded observations with 24 hours visible, repeatedly changes historical focus, captures rendered frames, and measures the spacing of two simultaneous live loopback traces.
