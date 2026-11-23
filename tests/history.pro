QT += core
QT -= gui
CONFIG += c++20 link_pkgconfig
PKGCONFIG += sqlite3
TARGET = history_test
INCLUDEPATH += ../src
SOURCES += history_test.cpp ../src/baseline.cpp ../src/diagnostichistory.cpp ../src/groups.cpp \
           ../src/history.cpp ../src/incident.cpp ../src/monitoring.cpp ../src/routeevents.cpp
