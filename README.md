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
./build/hopline --report example.net --family 6 --protocol tcp --port 443 --interface eth0
./build/hopline --report 127.0.0.1 --family 4 --interval 500 --seconds 10
```

The demo opens two simultaneous recordings. Compare the IPv6 service with the IPv4 gateway during the upstream latency spike. The headless report writes JSON to stdout. Duration controls how long new rounds start; in-flight rounds have time to receive replies or time out before the report prints.

On Arch Linux, `bin/package` builds a pacman package and a source package in `pkgbuild/`. Installing the binary package adds Hopline to the desktop launcher.

The **Probes** menu selects ICMP, UDP, or TCP and an outgoing interface for the new trace. Auto tries ICMP first and uses UDP if the kernel disallows ping sockets. Pick UDP or TCP explicitly to test the path to a particular port. The default TCP port is 443 and the default UDP port is 33434. Hover over a recording’s tab to see its saved probe settings. A resumed recording uses those settings.

## Investigate a trace

- **Read across a hop row.** The horizontal span is the lowest to highest RTT in the focus period. The connected dots show averages, and the cross marks the last sample when it received a reply. Sent and loss count timeouts. Latency statistics use replies only. Jitter is the mean absolute difference between consecutive successful RTTs.
- **Click a hop** to toggle its timeline. All visible timelines share a time axis. The destination timeline follows the target when its hop number changes. Per-hop timelines follow the selected hop number. A change of responding address is flagged in the focused hop table.
- **Click a spike** to inspect that point in history. The upper table recalculates over the focus window, using the responders observed then. Auto focus covers the visible timeline. All focus covers the recording. Drag across any timeline to fill all timelines with that time range and recompute the hop table for the selection. Shift-drag or use the arrow buttons to pan. Ctrl+wheel and the View picker change the displayed duration. The horizontal amber and red timeline bands mark RTTs above 100 and 200 ms, respectively. **Go live** returns to the current end of a running trace.
- **Read loss downstream.** A router can rate-limit its own replies. Loss at one intermediate hop with a healthy destination has a different meaning from loss that persists to the destination.
- **Right-click a timeline** to add a timestamped note. Right-click an existing marker to edit or remove it. **Trace actions → Save graph as PNG** exports the visible graphs. Ctrl+L focuses the target field and Ctrl+Shift+S opens the image export dialog.
- **Resize the interface** with Ctrl++ (or Ctrl+=) and Ctrl+−. Ctrl+0 restores 100%. Hopline remembers the scale across launches.
- **Close a trace tab** with its × button or a middle-click. Closing stops collection and keeps the recording. Use the closed-traces menu to reopen it, including after restarting Hopline.
- **Pause** stops collection for that tab. Resume appends to the same session. Switching tabs preserves the current focus and pinned hops for the duration of the app session. By default, closing the window stops collection. Reopen Hopline to review the recording or resume a trace.

## Compare destinations

Open at least two traces, then choose **Compare…** above the timelines. Select the other destinations to display beneath the active trace. All tracks share the same time axis and RTT scale. Hover over a spike to read latency and loss at the same instant for every destination. Drag across any track to zoom them together. The route profile belongs to the active tab. A blank stretch means that recording has no samples for that period. A timeout is a recorded probe and contributes to loss.

For example, monitor your gateway, a public endpoint, and the service you are troubleshooting. A spike that begins at the same time on all three points you toward the shared part of the path. A spike seen only on the service gives you a different set of hops to investigate. To compare address families, trace the same hostname twice and select IPv4 on one trace and IPv6 on the other.

## Share an incident

Drag across the incident, then choose **Trace actions → Export focus as CSV** or **Save replayable incident**. Both commands capture the active target and the time range used for the hop table. **Focus → All** exports its whole recording. CSV contains exact UTC timestamps, per-hop RTTs or timeouts, responder addresses, destination flags, annotations and probe settings. Each actual probe appears once.

Open a portable recording with **Trace actions → Open incident**, the empty window’s **Open incident** button, or the command below. A separate offline window opens at the saved interval with the original settings and annotations. Its temporary database is discarded when you close it.

```sh
./build/hopline --replay outage.hopline
```

## Route history

Open **Trace actions → Route history** to review sustained changes in the selected recording, including older recordings. The picker lists when a new responder or destination hop first appeared. Select a change to see when it was confirmed, compare the paths by TTL, or choose **Show on timeline** to inspect the six rounds surrounding the transition. Refresh includes rounds recorded since the dialog opened.

Hopline establishes a baseline and confirms a replacement after three consecutive matching rounds at a hop. Missing replies break the confirmation streak and leave the last confirmed address intact. A gap longer than four preceding sampling intervals, with a five-second minimum, restarts confirmation and the RTT comparison window. Alternating per-round responders alone do not confirm a change. Longer runs of replies from one load-balanced path can still look like a routing change from a single vantage point. A shorter path requires the destination itself to answer at its new hop. The destination RTT summaries average successful replies in the three rounds immediately before the change and the three confirming rounds. The counts show how much evidence each average uses.

## Unattended recording and destination alerts

Open **Trace actions → Recording and alerts** and enable **Keep recording after closing the window**. Closing the window leaves its collectors running in the desktop session. Launch Hopline again to see the same traces. **Pause** and closing a trace tab both stop that trace. **Quit Hopline** flushes pending samples and exits. When unattended recording is enabled, the next launch resumes the traces that were running at exit.

The same controls are available in a desktop terminal. Use the same `--database` path on each command when working with a separate recording collection.

```sh
hopline --background --family 4 127.0.0.1 &
hopline
hopline --quit
```

Configure a mean RTT threshold, a packet-loss percentage, or both for the selected destination. A zero threshold disables that metric. Hopline raises a desktop notification when the same threshold is reached in two consecutive complete windows and issues one recovery after two healthy windows. Pauses and collection gaps break the confirmation period. **Recent events** keeps the latest 200 transitions across traces available in the window. Click an event or its desktop notification to inspect the interval, including after archiving a trace. Desktop notifications use the session’s freedesktop notification service. All transitions are stored with the recording.

## Measurement and storage

The collector uses nonblocking Linux sockets on a dedicated worker thread. ICMP uses Linux ping sockets. TCP uses nonblocking connects and the Linux SYN error queue for intermediate routers. An accepted connection or a TCP reset proves that the destination responded. UDP keeps the same source and destination ports and checksum across hops. When a router quotes only the UDP header or an application sends an untagged reply, subsequent UDP probes use separate source ports for reliable attribution. In-flight probes are retried once when switching source ports. None of these methods require raw sockets. Each round sends increasing IPv4 TTLs or IPv6 hop limits toward the target and correlates replies with individual probes. The path limit is 30 hops and the reply timeout is 1.5 seconds. The collector uses kernel receive timestamps when available and coherent with the send clock.

History lives in `${XDG_DATA_HOME:-~/.local/share}/hopline/history.sqlite`. Use `--database PATH` for a separate recording collection. Raw data is kept until the database is removed. To archive a database, close Hopline first or use SQLite's backup facility.

A writer batches samples into SQLite WAL transactions. An independent reader combines raw samples with 10-second, minute, and hour summaries. Zoomed-out pixels retain extrema and loss, and focus statistics include the raw edges of the selected interval. The Qt Quick renderer receives only the displayed rows and timeline buckets. Hardware rendering uses scene-graph geometry; a software rendering path is also provided.

## Verify

```sh
bin/test
python3 tests/replay-cli.py
tests/probe-netns.sh
python3 tests/background-recorder.py
build-tests/history/history_test --benchmark
QT_QPA_PLATFORMTHEME=generic QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  HOPLINE_LIVE_TEST=1 build-tests/integration/integration_test replayWhileProbing
```

The recorder test checks background close, relaunch and restart against loopback. Add `--notifications` to test delivery through a private D-Bus daemon with dbus-python and PyGObject installed.

The regular suite checks historical statistics, reroutes, notes, session state and the real Qt window. The namespace test sets up an isolated two-hop network and checks IPv4, IPv6, ICMP, fixed-port UDP, open and refused TCP connections, interface selection, link-local addressing and timeout accounting through the CLI. It requires user/network namespaces, iproute2, util-linux and Python 3. The last test replays 25 hours of recorded observations with 24 hours visible, repeatedly changes historical focus, captures rendered frames, and measures the spacing of two simultaneous live loopback traces.
