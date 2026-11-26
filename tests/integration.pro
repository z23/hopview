QT += core gui quick quickcontrols2 qml network testlib
CONFIG += c++20 hopview_probes
include(../platform.pri)
CONFIG += console
CONFIG -= app_bundle
win32|macx: QT += widgets
TARGET = integration_test
INCLUDEPATH += ../src
SOURCES += integration_test.cpp ../src/backend.cpp ../src/background.cpp ../src/baseline.cpp \
           ../src/baselineview.cpp ../src/diagnostics.cpp ../src/diagnostichistory.cpp ../src/graph.cpp \
           ../src/groups.cpp ../src/groupview.cpp ../src/history.cpp ../src/incident.cpp ../src/monitoring.cpp \
           ../src/remotes.cpp ../src/routeevents.cpp
SOURCES += ../src/collectormanagement.cpp ../src/localreceiver.cpp ../src/nodeconfig.cpp
SOURCES += ../src/routeindex.cpp ../src/networkidentity.cpp ../src/storage.cpp ../src/historytools.cpp ../src/operatorview.cpp
HEADERS += ../src/collectormanagement.h ../src/localreceiver.h ../src/receiverendpoint.h ../src/backend.h ../src/background.h ../src/diagnostics.h ../src/probe.h ../src/graph.h ../src/types.h
RESOURCES += ../src/resources.qrc
