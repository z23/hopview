QT = core network testlib
CONFIG += console testcase c++20
CONFIG -= app_bundle
TARGET = portableprobe_test
INCLUDEPATH += ../src
HEADERS += ../src/probe.h
SOURCES += portableprobe_test.cpp ../src/probecontrol.cpp ../src/probeportable.cpp
macx: SOURCES += ../src/macresolver.cpp
