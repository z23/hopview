QT += core network
QT -= gui
CONFIG += c++20
include(../platform.pri)
CONFIG += console
CONFIG -= app_bundle
TARGET = history_test
INCLUDEPATH += ../src
SOURCES += ../src/collectormanagement.cpp ../src/nodeconfig.cpp
SOURCES += history_test.cpp ../src/baseline.cpp ../src/diagnostichistory.cpp ../src/groups.cpp \
           ../src/history.cpp ../src/incident.cpp ../src/monitoring.cpp ../src/routeevents.cpp

SOURCES += ../src/routeindex.cpp ../src/networkidentity.cpp ../src/storage.cpp
