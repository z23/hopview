#include "nativeecho.h"
#include "probe.h"
#include <QTimer>
#include <QtTest>

namespace {
const QString target = QStringLiteral("192.0.2.10"), router = QStringLiteral("192.0.2.1");

struct Script {
    std::function<void(quint64, int, const NativeEcho::Reply &)> send;
    QList<quint64> discarded;
    int rounds = 0;
} script;

class ScriptedEcho final : public NativeEcho {
public:
    explicit ScriptedEcho(Reply reply) : reply_(std::move(reply)) {}
    void send(quint64 token, int ttl) override {
        if (ttl == 255) ++script.rounds;
        script.send(token, ttl, reply_);
    }
    void discard(quint64 token) override { script.discarded.append(token); }
    QVariantMap route() const override { return {{"sourceAddress", "192.0.2.2"}}; }
private:
    Reply reply_;
};
}

std::unique_ptr<NativeEcho> openNativeEcho(const QHostAddress &, const ProbeOptions &,
                                          NativeEcho::Reply reply, QString &) {
    return std::make_unique<ScriptedEcho>(std::move(reply));
}

class PortableProbeTest : public QObject {
    Q_OBJECT
private slots:
    void init() { script = {}; }

    void overlappingRounds() {
        Probe probe;
        probe.setPreviewSession(7);
        QVector<Sample> recorded;
        connect(&probe, &Probe::samples, this, [&](const QVector<Sample> &batch) { recorded += batch; });
        QSignalSpy finished(&probe, &Probe::finished), preview(&probe, &Probe::replyReceived);
        quint64 silent = 0;
        bool secondFinishedEarly = false;
        script.send = [&](quint64 token, int ttl, const NativeEcho::Reply &reply) {
            if (ttl == 1 && script.rounds == 1) {
                silent = token;
            } else if (ttl == 1) {
                reply(token, {{}, -2, false, {"local-unsent", {}, -1, -1, 0, 22}});
            } else {
                reply(token, {target, 3.5, true, {}});
                if (ttl == 2 && script.rounds == 2) {
                    secondFinishedEarly = recorded.isEmpty() && !script.discarded.contains(silent);
                    probe.finish(7);
                }
            }
        };
        probe.start(7, target, 4, 250, "icmp");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(secondFinishedEarly);
        QCOMPARE(script.rounds, 2);
        QCOMPARE(script.discarded, QList<quint64>{silent});
        QCOMPARE(preview.size(), 4); // Endpoint and TTL 2, independently, in each round.
        QCOMPARE(recorded.size(), 6);
        QVERIFY(recorded[0].timeMs < recorded[3].timeMs);
        for (int i = 0; i < recorded.size(); ++i) {
            QCOMPARE(recorded[i].hop, i % 3);
            QCOMPARE(recorded[i].timeMs, recorded[(i / 3) * 3].timeMs);
            if (i % 3 != 1) {
                QVERIFY(recorded[i].destination);
                QCOMPARE(recorded[i].rttMs, 3.5);
            }
        }
        QCOMPARE(recorded[1].rttMs, -1.0);
        QVERIFY(recorded[1].failure.kind.isEmpty());
        QCOMPARE(recorded[4].rttMs, -2.0);
        QCOMPARE(recorded[4].failure.kind, QStringLiteral("local-unsent"));
        QCOMPARE(recorded[4].failure.systemError, 22);
    }

    void routeGrowth() {
        Probe probe;
        QVector<QVector<Sample>> rounds;
        connect(&probe, &Probe::samples, this, [&](const QVector<Sample> &batch) { rounds.append(batch); });
        QSignalSpy finished(&probe, &Probe::finished), measurements(&probe, &Probe::measurement);
        script.send = [&](quint64 token, int ttl, const NativeEcho::Reply &reply) {
            const int round = script.rounds;
            QTimer::singleShot(0, &probe, [&, token, ttl, reply, round] {
                const bool destination = ttl >= (round == 1 ? 2 : 3);
                reply(token, {destination ? target : router, 2, destination, {}});
                if (round == 3 && ttl == 3) probe.finish(11);
            });
        };
        probe.start(11, target, 4, 250);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(measurements.size(), 1);
        QCOMPARE(measurements.first()[2].toMap()["sourceAddress"].toString(), QStringLiteral("192.0.2.2"));
        QCOMPARE(rounds.size(), 3);
        QCOMPARE(rounds[0].size(), 3);
        QCOMPARE(rounds[1].size(), 3);
        QCOMPARE(rounds[2].size(), 4);
        QVERIFY(rounds[0].last().destination && !rounds[1].last().destination && rounds[2].last().destination);
        QCOMPARE(rounds[1].last().address, router);
        for (const auto &round : rounds) {
            QCOMPARE(round.first().hop, 0);
            QVERIFY(round.first().destination);
            QCOMPARE(round.first().address, target);
        }
    }

    void cancelOutstanding() {
        Probe probe;
        QList<quint64> outstanding;
        script.send = [&](quint64 token, int, const NativeEcho::Reply &) { outstanding.append(token); };
        QSignalSpy samples(&probe, &Probe::samples);
        probe.start(12, target, 4, 1000, "icmp");
        QVERIFY(outstanding.size() > 1);
        probe.stop(12);
        QCOMPARE(script.discarded, outstanding);
        QCOMPARE(samples.size(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(samples.size(), 0);
    }

    void stopInsideFirstHopReply() {
        Probe probe;
        probe.setPreviewSession(20);
        QList<int> hops;
        quint64 endpoint = 0;
        script.send = [&](quint64 token, int ttl, const NativeEcho::Reply &reply) {
            hops.append(ttl);
            if (ttl == 255) endpoint = token;
            else reply(token, {router, 2, false, {}});
        };
        connect(&probe, &Probe::replyReceived, &probe, [&](qint64 id) { probe.stop(id); });
        QSignalSpy samples(&probe, &Probe::samples);
        probe.start(20, target, 4, 250);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(hops, QList<int>({255, 1}));
        QCOMPARE(script.discarded, QList<quint64>{endpoint});
        QCOMPARE(samples.size(), 0);
    }
};

QTEST_GUILESS_MAIN(PortableProbeTest)
#include "portableprobe_test.moc"
