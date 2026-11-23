# Compare VM locations

Each VM runs `hopview-node collect`, which probes the destinations in its local JSON configuration and makes an outbound TLS connection to a receiver. The receiver listens on a TCP port, authenticates a distinct token for each location and appends the timestamped samples to Hopview history. Both programs run without a graphical login. The receiver can stay running when the desktop closes.

## Install on EL9 and EL10 VMs

Use the VM’s package installer to install the local x86_64 package matching its major release, with dependency resolution enabled. On EL9, enable the `epel` repository for the runtime dependencies. On EL10 they are available in `appstream`.

| VM release | Package file |
| --- | --- |
| EL9 | `hopview-node-0.4.0-1.el9.x86_64.rpm` |
| EL10 | `hopview-node-0.4.0-1.el10.x86_64.rpm` |

The package creates the `hopview` service account and installs editable examples as `/etc/hopview/collector.json` and `/etc/hopview/receiver.json`. Choose the role of each host and configure it below. The collector on a VM only needs an outbound connection to the receiver.

To rebuild the packages, use a build host on the same major release as the destination. Install the build dependencies listed in the [node package manifest](../rpm/hopview-node.spec), then run `bin/package-node-rpm` from the source tree. Binary and source packages appear in `build-rpm/`.

## Set up the receiver

Choose a receiver hostname reachable from your VMs. Use a server certificate covering that name, its PEM certificate chain and its PEM RSA or EC private key. For each site, use a cryptographically secure generator to produce 32 random bytes and save their 64 hexadecimal characters in a separate token file. The example uses `melbourne-vm.token` and `london-vm.token`. Deliver each VM its own token using your usual secret distribution method.

Edit the installed receiver configuration or start from [receiver.example.json](../deploy/receiver.example.json). Set `listen`, `port`, `certificate`, `privateKey` and `database`. In `sites`, map each site name to its token file. File paths in both configurations are relative to the configuration file unless absolute. Open the chosen inbound TCP port on the receiver for your collectors. The example uses port 8347. The endpoint speaks Hopview's line-delimited JSON protocol over TLS. A reverse proxy can forward it in TCP pass-through mode.

**To watch the feeds from the desktop**, run the receiver as your normal desktop user, point its `database` at a writable absolute path, and open that same file in Hopview. In this example, Alex has saved the configuration as `./receiver.json` and set `database` to `/home/alex/hopview/remote.db`. Start the receiver in one terminal and open Hopview in another.

```sh
hopview-node receive ./receiver.json
hopview --database /home/alex/hopview/remote.db
```

On a dedicated receiver, the service stores its database at `/var/lib/hopview-receiver/history.sqlite` by default. For a live desktop sharing that file, both processes need access to the database and its directory on the same local filesystem. To inspect a snapshot on another machine, use your database’s online backup facility. Alternatively, stop all processes using the database, copy the file and any accompanying `-wal` and `-shm` files together, then restart collection. Open the transferred database with `hopview --database PATH`.

## Configure the VM

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
