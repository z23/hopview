QT += core gui quick quickcontrols2 qml network dbus testlib
CONFIG += c++20 link_pkgconfig
PKGCONFIG += sqlite3
TARGET = integration_test
INCLUDEPATH += ../src
SOURCES += integration_test.cpp ../src/backend.cpp ../src/background.cpp ../src/history.cpp \
           ../src/incident.cpp ../src/monitoring.cpp ../src/probe.cpp ../src/routeevents.cpp ../src/graph.cpp
HEADERS += ../src/backend.h ../src/background.h ../src/probe.h ../src/graph.h ../src/types.h
RESOURCES += ../src/resources.qrc
