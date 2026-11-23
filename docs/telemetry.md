# Compare Linux VM locations

Each VM runs `hopline-node collect`, which probes the destinations in its local JSON configuration and makes an outbound TLS connection to a receiver. The receiver listens on a TCP port, authenticates a distinct token for each location and appends the timestamped samples to Hopline history. Both programs run without a graphical login. The receiver can stay running when the desktop closes.

## Install on AlmaLinux VMs

Use the x86_64 package matching the VM’s major release. On AlmaLinux 9, enable EPEL to obtain Qt Core and Qt Network. On AlmaLinux 10 those runtime libraries come from AppStream. The node runs headless.

```sh
# AlmaLinux 9
sudo dnf install epel-release
sudo dnf install ./hopline-node-0.4.0-1.el9.x86_64.rpm

# AlmaLinux 10
sudo dnf install ./hopline-node-0.4.0-1.el10.x86_64.rpm
```

The RPM creates the `hopline` service account and installs editable examples as `/etc/hopline/collector.json` and `/etc/hopline/receiver.json`. Install each VM’s token and any private CA certificate where that account can read them. For example, on the Melbourne VM:

```sh
sudo install -o root -g hopline -m 0640 melbourne-vm.token /etc/hopline/melbourne-vm.token
sudoedit /etc/hopline/collector.json
sudo systemctl enable --now hopline-collector
sudo journalctl -u hopline-collector -f
```

Configure the receiver below before expecting live data. To rebuild an RPM locally, install GCC C++, make, rpm-build, systemd-rpm-macros, qt6-qtbase-devel, sqlite-devel, pkgconf-pkg-config and tar on the destination major release, then run `bin/package-node-rpm`. The packages appear in `build-rpm/`.

## Set up the receiver

Choose a receiver hostname reachable from your VMs. Use a server certificate covering that name, its PEM certificate chain and its PEM RSA or EC private key. On the receiver, generate a separate random token for each site. Copy only that site's token to its VM using your usual secret distribution method. For example, in a private directory on the receiver:

```sh
umask 077
openssl rand -hex 32 > melbourne-vm.token
openssl rand -hex 32 > london-vm.token
```

Start from [receiver.example.json](../deploy/receiver.example.json). Enter your certificate and key paths, sites and database. File paths in JSON are relative to the configuration file unless absolute. Open the chosen inbound TCP port on the receiver for your collectors. The endpoint speaks Hopline's line-delimited JSON protocol over TLS. A reverse proxy can forward it in TCP pass-through mode.

**To watch the feeds from the desktop**, run the receiver as your normal desktop user, point its `database` at a writable absolute path, and open that same file in Hopline. For example, with `database` set to `/home/alex/hopline/remote.sqlite`:

```sh
hopline-node receive ./receiver.json
# In a second terminal on the same host:
hopline --database /home/alex/hopline/remote.sqlite
```

For a dedicated receiver, the system service stores its database under `/var/lib/hopline-receiver` with permissions for the `hopline` account. To inspect it on a different machine, use a SQLite backup and transfer the resulting snapshot. The desktop and receiver can also share a database when they run under the same local user. SQLite WAL needs a local filesystem for concurrent processes.

## Configure the VM

Use [collector.example.json](../deploy/collector.example.json) as the starting point. Set a unique `site` that matches an entry in the receiver, the receiver hostname, its port, a token file and a writable spool path. Add one target entry per trace. Each entry accepts `target`, `family` (0 for automatic, 4, or 6), `protocol` (auto, icmp, udp or tcp), `port`, `interface`, and `intervalMs` (250–60000). For location-to-location comparisons pick the same resolved address, address family, protocol and port at each site.

TLS uses the VM's trusted CAs and verifies the receiver hostname. For an internal CA, add `"caCertificate": "ca.pem"` in the collector config. The site token travels inside the encrypted connection. TLS is the default. For a local test, both programs also accept `"insecureLoopback": true` with a literal loopback address such as `127.0.0.1`.

For a foreground test, point `spool` at a user-writable path and run:

```sh
hopline-node collect ./collector.json
```

For a system service, install the matching configuration in `/etc/hopline/collector.json` or `/etc/hopline/receiver.json`. Place its referenced credentials in that directory, owned by `root:hopline` with mode `0640`; use mode `0750` for the directory. The example units create their own state directories. Start the collector on each VM after configuring it:

```sh
sudo systemctl enable --now hopline-collector
journalctl -u hopline-collector -f
```

The receiver service uses `systemctl enable --now hopline-receiver`. Restart the appropriate unit after editing its configuration or rotating its credentials. The collector initiates every connection and only observes the targets configured by the VM administrator.

## Read the recordings

The receiver creates a recording for each location, resolved endpoint and probe configuration. A restart with the same settings continues that recording. Open two sites recorded against the same address, choose **Compare…**, and select the second site. Click or drag on a timeline to focus both on the same wall-clock interval.

The timeline uses the original collector Unix timestamps. After each acknowledged message the VM estimates its clock difference from the receiver using all four send/receive timestamps. The reported offset is the collector clock minus the receiver clock; a positive value means the VM clock runs ahead. The uncertainty includes half the observed network round trip and a two-millisecond timestamp allowance. It measures relative alignment between those machines. Keep them synchronized with their usual time service. Estimates expire after a minute or a detected wall-clock step. A queued batch carries the estimate available when it was collected, or unknown. The session displays the estimate for its most recent sample. Replaying an older batch preserves that estimate. Arrival time indicates upload freshness; old samples can arrive after reconnection.

Completed rounds are written to the VM's SQLite outbox before upload. An acknowledgement follows storage of samples and metadata at the receiver. Resending the same round preserves its original sample count. Retries back off to one minute, with no inbound collector port. The default outbox holds 64 MiB of serialized telemetry plus SQLite overhead; `spoolMiB` accepts 1–4096. When full, the collector discards its oldest batches and logs the count. `SIGTERM` preserves committed batches for the next launch. A spool is tied to its site and endpoint; use a new path if either changes.

## Build the headless binaries from source

On a Linux host with Qt 6 Base development files (Core, Network, TLS backend and qmake), GCC with C++20, make, pkg-config and SQLite development files:

```sh
bin/build-node
build-node/hopline-node --help
```

The headless target has no Qt Quick or display dependency. The same executable runs the collector and receiver. Systemd unit templates, a sysusers file and example JSON configurations are under `deploy/`.
