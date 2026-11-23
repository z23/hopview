# Add VM collectors

A new collector needs a reachable receiver address and its own enrollment key. The desktop assigns its targets after it connects. The collector initiates the connection and reports measurements over TLS.

## Enroll a VM

1. Open **Collectors → Add collector…** in Hopview. Enter the hostname or IP address the VMs will use to reach the receiver. The default port is 8347. For a receiver on the desktop machine, choose **Start receiver**. Allow the selected TCP port through that machine’s firewall. The desktop creates and keeps its certificate locally.
2. Generate a separate enrollment key for the VM. Copy the join command and run it as administrator on the VM after installing the node package. For example, from a root shell

   ```sh
   hopview-node join monitor.example.net
   ```

   Paste the key at the hidden prompt. An automation can supply the key on standard input. The command saves the receiver address and a persistent installation identity, then enables and starts the collector service.
3. The VM appears under its hostname, even with zero targets. Choose **Targets…** and add a hostname or IP address with its address family and probe settings. To match another site, copy its target list into the draft. Apply the changes when the list is ready. The status shows when that VM has applied the revision. An applied revision can still contain a target that times out.

Use `monitor.example.net:9443` for a different port or `[2001:db8::10]:9443` for an IPv6 literal with a port. Each key binds to the first installation to use it. Distribute a fresh key per VM, including clones, before their first enrollment.

The receiver retains edits for disconnected VMs. During an outage a collector continues its last applied assignment and queues results on disk. Removing a target stops its probes and keeps its history. Closing a trace tab or a comparison only changes the desktop view of a managed recording.

Running a desktop receiver keeps the listener active until you stop it or exit Hopview. Start it again in **Collectors** after relaunching the app. Use the standalone receiver service below if your VMs should continue delivering while the desktop is closed.

## Install on EL9 and EL10 VMs

Use the VM’s package installer to install the local x86_64 package matching its major release, with dependency resolution enabled. On EL9, enable the `epel` repository for the runtime dependencies. On EL10 they are available in `appstream`.

| VM release | Package file |
| --- | --- |
| EL9 | [hopview-node-0.5.0-1.el9.x86_64.rpm](https://github.com/z23/hopview/releases/download/v0.5.0/hopview-node-0.5.0-1.el9.x86_64.rpm) |
| EL10 | [hopview-node-0.5.0-1.el10.x86_64.rpm](https://github.com/z23/hopview/releases/download/v0.5.0/hopview-node-0.5.0-1.el10.x86_64.rpm) |

The package creates the `hopview` service account and installs editable examples as `/etc/hopview/collector.json` and `/etc/hopview/receiver.json`. Choose the role of each host and configure it below. The collector on a VM only needs an outbound connection to the receiver.

To rebuild the packages, use a build host on the same major release as the destination. Install the build dependencies listed in the [node package manifest](../rpm/hopview-node.spec), then run `bin/package-node-rpm` from the source tree. Binary and source packages appear in `build-rpm/`.

## Run a dedicated receiver

Choose a receiver hostname reachable from the VMs and supply its PEM certificate chain and RSA or EC private key. Edit the installed receiver configuration or start from [receiver.example.json](../deploy/receiver.example.json). Set `listen`, `port`, `certificate`, `privateKey` and `database`. The listener accepts a numeric bind address or `*` for both IP families. Paths are relative to the configuration file unless absolute.

To manage these nodes, open the receiver’s **live local database** in Hopview under an account with write access. The receiver and desktop share enrollment records, revisions and status through that database. A transferred snapshot can be reviewed offline. Changes to a snapshot stay on that copy.

Keys created after the TLS listener starts include its certificate fingerprint. This lets a fresh VM verify a private certificate without installing a CA file. Preserve the receiver’s identity when moving it. Keys generated before a receiver has recorded its fingerprint rely on the VM’s trusted CAs. For a CA-verified connection, use the hostname covered by the certificate. A pinned key accepts only the recorded certificate. Replacing it requires updating the pins on its collectors.

**To watch the feeds from the desktop**, run the receiver as your normal desktop user, point its `database` at a writable absolute path, and open that same file in Hopview. In this example, Alex has saved the configuration as `./receiver.json` and set `database` to `/home/alex/hopview/remote.db`. Start the receiver in one terminal and open Hopview in another.

```sh
hopview-node receive ./receiver.json
hopview --database /home/alex/hopview/remote.db
```

On a dedicated receiver, the service stores its database at `/var/lib/hopview-receiver/history.sqlite` by default. For a live desktop sharing that file, both processes need access to the database and its directory on the same local filesystem. To inspect a snapshot on another machine, use your database’s online backup facility. Alternatively, stop all processes using the database, copy the file and any accompanying `-wal` and `-shm` files together, then restart collection. Open the transferred database with `hopview --database PATH`.

## Keep a manually configured collector

Existing collectors can continue to use a local target list. On the receiver add a `sites` object mapping each site name to its token file, for example `{"sites":{"melbourne-vm":"melbourne-vm.token"}}`. Provision a separate random token for each site and copy that token to the corresponding VM. This mode accepts target changes through the local configuration file.

Use [collector.example.json](../deploy/collector.example.json) as the starting point. Set a unique `site` that matches an entry in the receiver, the receiver hostname, its port, a token file and a writable spool path. Add one target entry per trace. Each entry accepts `target`, `family` (0 for automatic, 4, or 6), `protocol` (auto, icmp, udp or tcp), `port`, `interface`, and `intervalMs` (250–60000). For location-to-location comparisons pick the same resolved address, address family, protocol and port at each site.

TLS uses the VM's trusted CAs and verifies the receiver hostname. For an internal CA, add `"caCertificate": "ca.pem"` in the collector config. The site token travels inside the encrypted connection. TLS is the default. For a local test, both programs also accept `"insecureLoopback": true` with a literal loopback address such as `127.0.0.1`.

For a foreground test, save the configuration as `./collector.json`, point `spool` at a user-writable path and start the collector.

```sh
hopview-node collect ./collector.json
```

For a system service, place the configuration and its referenced credentials in `/etc/hopview/`, owned by `root:hopview` with mode `0640`. Use mode `0750` for the directory. For example, the Melbourne VM’s token goes in `/etc/hopview/melbourne-vm.token`.

| Role | Service unit | Configuration | Default state directory |
| --- | --- | --- | --- |
| Receiver | `hopview-receiver.service` | `/etc/hopview/receiver.json` | `/var/lib/hopview-receiver/` |
| Collector | `hopview-collector.service` | `/etc/hopview/collector.json` | `/var/lib/hopview-collector/` |

Use the host’s service manager to start the receiver and enable it at boot. Then start and enable the collector on each VM. The units create their own state directories. Follow the log for the corresponding unit to check connection and authentication. Restart that unit after editing configuration or rotating credentials.

## Read the recordings

The receiver creates a recording for each location, resolved endpoint and probe configuration. A restart with the same settings continues that recording. Open two sites recorded against the same address, choose **Compare…**, and select the second site. Click or drag on a timeline to focus both on the same wall-clock interval.

The timeline uses the original collector timestamps in UTC. After each acknowledged message the VM estimates its clock difference from the receiver using all four send/receive timestamps. The reported offset is the collector clock minus the receiver clock. A positive value means the VM clock runs ahead. The uncertainty includes half the observed network round trip and a two-millisecond timestamp allowance. It measures relative alignment between those machines. Keep them synchronized with their usual time service. Estimates expire after a minute or a detected wall-clock step. A queued batch carries the estimate available when it was collected, or unknown. The session displays the estimate for its most recent sample. Replaying an older batch preserves that estimate. Arrival time indicates upload freshness. Older samples can arrive after reconnection.

The VM writes completed rounds to its on-disk outbox before upload. The receiver acknowledges a batch after storing the samples and metadata. Resending the same round preserves its original sample count. The retry interval grows to one minute. The default outbox holds 64 MiB of serialized telemetry, plus database overhead. Set `spoolMiB` to a value from 1 to 4096 to adjust the limit. When full, the collector discards its oldest batches and logs the count. `SIGTERM` preserves committed batches for the next launch. A spool is tied to its site and endpoint. Use a new path if either changes.

## Build the headless binaries from source

Use a C++20 compiler and the development files listed in the [node package manifest](../rpm/hopview-node.spec). From the project directory, build the node and display its command syntax.

```sh
bin/build-node
build-node/hopview-node --help
```

The same executable runs the collector and receiver without a display. The source tree includes the service units, service account definition and example configurations in [deploy/](../deploy/).
