QT += core gui quick quickcontrols2 qml network dbus
CONFIG += c++20 link_pkgconfig
PKGCONFIG += sqlite3 libcurl
TEMPLATE = app
TARGET = hopview
SOURCES += src/main.cpp src/backend.cpp src/background.cpp src/baseline.cpp src/baselineview.cpp \
           src/diagnostics.cpp src/diagnostichistory.cpp src/graph.cpp src/groups.cpp src/groupview.cpp \
           src/history.cpp src/incident.cpp src/monitoring.cpp src/probe.cpp src/remotes.cpp src/routeevents.cpp
SOURCES += src/collectormanagement.cpp src/localreceiver.cpp src/nodeconfig.cpp
SOURCES += src/routeindex.cpp src/networkidentity.cpp src/storage.cpp src/historytools.cpp src/operatorview.cpp
HEADERS += src/collectormanagement.h src/localreceiver.h src/receiverendpoint.h src/backend.h src/background.h src/diagnostics.h src/graph.h src/history.h src/incident.h \
           src/probe.h src/probeoptions.h src/routeevents.h src/sqlhelpers.h src/types.h
RESOURCES += src/resources.qrc
