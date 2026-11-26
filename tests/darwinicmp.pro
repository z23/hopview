QT = core network testlib
CONFIG += console testcase c++20
CONFIG -= app_bundle
TARGET = darwinicmp_test
INCLUDEPATH += ../src
HEADERS += ../src/darwinicmp.h
SOURCES += darwinicmp_test.cpp
