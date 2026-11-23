QT += core gui quick quickcontrols2 qml network
CONFIG += c++20 link_pkgconfig
PKGCONFIG += sqlite3
TEMPLATE = app
TARGET = hopline
SOURCES += src/main.cpp src/backend.cpp src/history.cpp src/probe.cpp src/graph.cpp
HEADERS += src/types.h src/backend.h src/history.h src/probe.h src/graph.h
RESOURCES += src/resources.qrc
