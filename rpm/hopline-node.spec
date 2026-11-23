Name:           hopline-node
Version:        0.4.0
Release:        1%{?dist}
Summary:        Headless IPv4 and IPv6 path telemetry collector and receiver
License:        LicenseRef-Unspecified
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++, make, qt6-qtbase-devel, sqlite-devel, pkgconf-pkg-config
BuildRequires:  systemd-rpm-macros
Requires(pre):  shadow-utils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description
Continuously record paths to configured targets and deliver timestamped probe
results to a Hopline receiver over an outbound TLS connection. Both modes run
without a graphical session.

%prep
%setup -q

%build
mkdir build-node
cd build-node
qmake6 ../node.pro CONFIG+=release "QMAKE_CXXFLAGS+=%{optflags}" "QMAKE_LFLAGS+=%{build_ldflags}"
%make_build

%install
install -Dm755 build-node/hopline-node %{buildroot}%{_bindir}/hopline-node
install -d %{buildroot}%{_sysconfdir}/hopline
for mode in collector receiver; do
    install -Dm644 deploy/hopline-$mode.service %{buildroot}%{_unitdir}/hopline-$mode.service
    install -Dm640 deploy/$mode.example.json %{buildroot}%{_sysconfdir}/hopline/$mode.json
done

%pre
getent group hopline >/dev/null || groupadd -r hopline
getent passwd hopline >/dev/null || useradd -r -g hopline -d /var/lib/hopline -s /sbin/nologin -c "Hopline telemetry" hopline

%post
%systemd_post hopline-collector.service hopline-receiver.service

%preun
%systemd_preun hopline-collector.service hopline-receiver.service

%postun
%systemd_postun_with_restart hopline-collector.service hopline-receiver.service

%files
%{_bindir}/hopline-node
%{_unitdir}/hopline-collector.service
%{_unitdir}/hopline-receiver.service
%dir %attr(0750,root,hopline) %{_sysconfdir}/hopline
%config(noreplace) %attr(0640,root,hopline) %{_sysconfdir}/hopline/collector.json
%config(noreplace) %attr(0640,root,hopline) %{_sysconfdir}/hopline/receiver.json
%doc docs/telemetry.md
