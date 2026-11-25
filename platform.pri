# Shared dependencies and probe source selection for the desktop, node and tests.
win32-msvc* {
    # vcpkg installed triplet directory, e.g. C:/vcpkg/installed/x64-windows
    deps = $$(HOPVIEW_DEPS)
    isEmpty(deps): error(Set HOPVIEW_DEPS to the installed vcpkg triplet directory)
    INCLUDEPATH += "$$deps/include"
    LIBS += -L"$$deps/lib" -lsqlite3
    contains(CONFIG, hopview_probes): LIBS += -llibcurl
    QMAKE_CXXFLAGS += /utf-8 /MP
} else:macx {
    LIBS += -lsqlite3
    contains(CONFIG, hopview_probes): LIBS += -lcurl
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sqlite3
    contains(CONFIG, hopview_probes): PKGCONFIG += libcurl
}

win32: DEFINES += NOMINMAX

contains(CONFIG, hopview_probes) {
    SOURCES += $$PWD/src/probecontrol.cpp
    win32 {
        SOURCES += $$PWD/src/probeportable.cpp $$PWD/src/echowindows.cpp
        LIBS += -liphlpapi -lws2_32
    } else:macx {
        SOURCES += $$PWD/src/probeportable.cpp $$PWD/src/echomac.cpp
    } else:linux {
        QT += dbus
        SOURCES += $$PWD/src/probe.cpp
    } else {
        error(Hopview probes currently support Linux, Windows and macOS)
    }
}
