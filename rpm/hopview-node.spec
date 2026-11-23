Name:           hopview-node
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
results to a Hopview receiver over an outbound TLS connection. Both modes run
without a graphical session.

%prep
%setup -q

%build
mkdir build-node
cd build-node
qmake6 ../node.pro CONFIG+=release "QMAKE_CXXFLAGS+=%{optflags}" "QMAKE_LFLAGS+=%{build_ldflags}"
%make_build

%install
install -Dm755 build-node/hopview-node %{buildroot}%{_bindir}/hopview-node
install -d %{buildroot}%{_sysconfdir}/hopview
for mode in collector receiver; do
    install -Dm644 deploy/hopview-$mode.service %{buildroot}%{_unitdir}/hopview-$mode.service
    install -Dm640 deploy/$mode.example.json %{buildroot}%{_sysconfdir}/hopview/$mode.json
done

%pre
getent group hopview >/dev/null || groupadd -r hopview
getent passwd hopview >/dev/null || useradd -r -g hopview -d /var/lib/hopview -s /sbin/nologin -c "Hopview telemetry" hopview

%post
%systemd_post hopview-collector.service hopview-receiver.service

%preun
%systemd_preun hopview-collector.service hopview-receiver.service

%postun
%systemd_postun_with_restart hopview-collector.service hopview-receiver.service

%files
%{_bindir}/hopview-node
%{_unitdir}/hopview-collector.service
%{_unitdir}/hopview-receiver.service
%dir %attr(0750,root,hopview) %{_sysconfdir}/hopview
%config(noreplace) %attr(0640,root,hopview) %{_sysconfdir}/hopview/collector.json
%config(noreplace) %attr(0640,root,hopview) %{_sysconfdir}/hopview/receiver.json
%doc docs/telemetry.md
