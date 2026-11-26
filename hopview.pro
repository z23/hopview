QT += core gui quick quickcontrols2 qml network
CONFIG += c++20 hopview_probes
include(platform.pri)
win32|macx: QT += widgets
TEMPLATE = app
TARGET = hopview
macx {
    QMAKE_TARGET_BUNDLE_PREFIX = org.hopview
    QMAKE_INFO_PLIST = deploy/Info.plist
    ICON = deploy/hopview.icns
}
win32: RC_ICONS = deploy/hopview.ico
SOURCES += src/main.cpp src/backend.cpp src/background.cpp src/baseline.cpp src/baselineview.cpp \
           src/diagnostics.cpp src/diagnostichistory.cpp src/graph.cpp src/groups.cpp src/groupview.cpp \
           src/history.cpp src/incident.cpp src/monitoring.cpp src/remotes.cpp src/routeevents.cpp
SOURCES += src/collectormanagement.cpp src/localreceiver.cpp src/nodeconfig.cpp
SOURCES += src/routeindex.cpp src/networkidentity.cpp src/storage.cpp src/historytools.cpp src/operatorview.cpp
HEADERS += src/collectormanagement.h src/localreceiver.h src/receiverendpoint.h src/backend.h src/background.h src/diagnostics.h src/graph.h src/history.h src/historycache.h src/incident.h \
           src/probe.h src/probeoptions.h src/routeevents.h src/sqlhelpers.h src/types.h
RESOURCES += src/resources.qrc
