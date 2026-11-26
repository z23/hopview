# Windows and macOS capture

Download the artifact for your operating system from a successful [Native desktop probes run on `main`](https://github.com/z23/hopview/actions/workflows/native.yml?query=branch%3Amain) and unzip it. On Windows x64, run `Hopview-Windows-x64.exe` to install Hopview for your user. On macOS, open `Hopview-macOS.dmg` and drag **Hopview** to **Applications**. The app includes Intel and Apple silicon binaries. Development builds use ad-hoc signing on macOS and an unsigned Windows installer.

Enter a hostname or IP address and choose **Start trace**. Windows and macOS use ordinary-user ICMP echo for the route and a separate destination probe. The desktop and the bundled headless collector use the same measurement engine. **Recording and alerts** can keep a desktop trace running after the window closes. **Export focus as CSV** and **Save replayable incident** use the same formats as Linux.

When you select a device on your LAN, macOS 15 and later may ask for local network access. You can review Hopview’s access in **System Settings → Privacy & Security → Local Network**. [Apple’s local network privacy guide](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)

| Local capture option | Windows | macOS |
| --- | --- | --- |
| IPv4 and IPv6, including scoped link-local destinations | Yes | Yes |
| Auto or ICMP with a configurable payload | Yes | Yes |
| Outgoing interface | Selected source address on the chosen interface | Bound interface |
| Require unfragmented probes | IPv4 | IPv4 |
| DSCP marking | Unavailable | Yes |
| UDP and TCP TTL sweeps | Linux collectors | Linux collectors |

Windows reports the ICMP status and whole-millisecond RTT supplied by IP Helper. A successful loopback probe can read 0 ms. macOS uses ICMP datagram sockets and decodes quoted echo requests from routers. Firewalls and routers can suppress ICMP replies. Live replies appear as they arrive, and loss counts update when each round finishes. Replay and remote recordings retain their original protocols on every desktop.

For a ten-second JSON capture in PowerShell:

```powershell
& "$env:LOCALAPPDATA\Programs\Hopview\hopview.exe" --report 127.0.0.1 --family 4 --seconds 10 | Out-String
```

On macOS:

```sh
/Applications/Hopview.app/Contents/MacOS/hopview --report ::1 --family 6 --seconds 10
```

Configured collectors can run `hopview-node collect COLLECTOR.json` on either platform. The helper sits next to the desktop binary, including inside the macOS app. See the [manual collector configuration](telemetry.md#keep-a-manually-configured-collector). The systemd deployment and `hopview-node join` command are for Linux VMs. Starting a local TLS receiver from the desktop also requires the OpenSSL command-line tool.

## Build an installer

Use a Qt 6 desktop SDK with Qt Quick, Qt Quick Controls, Widgets and Network, plus Python 3. Ensure the SDK's `bin` is on your path. The [workflow](../.github/workflows/native.yml) specifies the SDK version used for the distributed builds.

- On Windows, use the x64 Visual Studio 2022 developer shell and MSVC Qt build. Install `sqlite3:x64-windows` and `curl:x64-windows` with vcpkg. Set `HOPVIEW_DEPS` to the resulting `installed/x64-windows` directory and install Inno Setup 6.
- On macOS, use Xcode command line tools and a universal Qt SDK. The build links to SQLite and libcurl from the macOS SDK.

Run `python bin/package-native.py` from the checkout. The installer or disk image is written under `build-native/`. Run `python tests/native-capture.py PATH_TO_DESKTOP_BINARY` for a finite IPv4 and IPv6 loopback check.
