QT += core
QT -= gui
CONFIG += c++20 link_pkgconfig
PKGCONFIG += sqlite3
TARGET = history_test
INCLUDEPATH += ../src
SOURCES += history_test.cpp ../src/history.cpp
