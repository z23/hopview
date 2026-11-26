#pragma once

#include <QCoreApplication>
#include <QTimer>
#include <atomic>
#include <csignal>

// A signal handler must avoid Qt and cannot assume it runs on the main thread.
// Lock-free storage and an event-loop callback also work for Windows console
// interrupts, which the C runtime delivers on a separate thread.
inline void installPortableExitHandlers(QCoreApplication &app) {
    static std::atomic_bool requested{false};
    static_assert(decltype(requested)::is_always_lock_free);
    auto handler = +[](int) { requested.store(true, std::memory_order_relaxed); };
    std::signal(SIGINT, handler);
    std::signal(SIGTERM, handler);
#ifdef SIGBREAK
    std::signal(SIGBREAK, handler);
#endif
    auto *timer = new QTimer(&app);
    QObject::connect(timer, &QTimer::timeout, &app, [] {
        if (requested.load(std::memory_order_relaxed))
            QCoreApplication::quit();
    });
    timer->start(250);
}
