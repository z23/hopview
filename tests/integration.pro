QT += core gui quick quickcontrols2 qml network dbus testlib
CONFIG += c++20 link_pkgconfig
PKGCONFIG += sqlite3 libcurl
TARGET = integration_test
INCLUDEPATH += ../src
SOURCES += integration_test.cpp ../src/backend.cpp ../src/background.cpp ../src/baseline.cpp \
           ../src/baselineview.cpp ../src/diagnostics.cpp ../src/diagnostichistory.cpp ../src/graph.cpp \
           ../src/groups.cpp ../src/groupview.cpp ../src/history.cpp ../src/incident.cpp ../src/monitoring.cpp \
           ../src/probe.cpp ../src/remotes.cpp ../src/routeevents.cpp
HEADERS += ../src/backend.h ../src/background.h ../src/diagnostics.h ../src/probe.h ../src/graph.h ../src/types.h
RESOURCES += ../src/resources.qrc
