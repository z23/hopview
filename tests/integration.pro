QT += core gui quick quickcontrols2 qml network testlib
CONFIG += c++20 link_pkgconfig
PKGCONFIG += sqlite3
TARGET = integration_test
INCLUDEPATH += ../src
SOURCES += integration_test.cpp ../src/backend.cpp ../src/history.cpp ../src/probe.cpp ../src/graph.cpp
HEADERS += ../src/backend.h ../src/probe.h ../src/graph.h ../src/types.h
RESOURCES += ../src/resources.qrc
