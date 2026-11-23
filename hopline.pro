QT += core gui quick quickcontrols2 qml network dbus
CONFIG += c++20 link_pkgconfig
PKGCONFIG += sqlite3
TEMPLATE = app
TARGET = hopline
SOURCES += src/main.cpp src/backend.cpp src/background.cpp src/history.cpp src/incident.cpp \
           src/monitoring.cpp src/probe.cpp src/routeevents.cpp src/graph.cpp
HEADERS += src/backend.h src/background.h src/history.h src/incident.h src/probe.h src/probeoptions.h \
           src/routeevents.h src/sqlhelpers.h src/types.h src/graph.h
RESOURCES += src/resources.qrc
