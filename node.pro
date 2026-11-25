QT = core network dbus
CONFIG += c++20 console link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += sqlite3 libcurl
TEMPLATE = app
TARGET = hopview-node
INCLUDEPATH += src
SOURCES += src/node.cpp src/nodecollector.cpp src/nodereceiver.cpp src/nodeconfig.cpp \
           src/probe.cpp src/history.cpp src/monitoring.cpp src/routeevents.cpp
SOURCES += src/collectormanagement.cpp src/nodejoin.cpp
SOURCES += src/routeindex.cpp src/networkidentity.cpp src/storage.cpp
RESOURCES += src/node-resources.qrc
HEADERS += src/collectormanagement.h src/nodejoin.h src/receiverendpoint.h src/probe.h src/types.h src/nodeconfig.h src/history.h src/historycache.h src/sqlhelpers.h
HEADERS += src/diagnostics.h
SOURCES += src/diagnostichistory.cpp src/diagnostics.cpp src/receiverweb.cpp
