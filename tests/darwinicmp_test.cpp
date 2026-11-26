#include "darwinicmp.h"

#include <QtTest>

namespace {
const QHostAddress target4(QStringLiteral("198.51.100.12")), target6(QStringLiteral("2001:db8::12"));
constexpr quint32 key = 0x12345678;
constexpr quint64 cookie = 0x0011223344556677ULL;

QByteArray ipv4(QByteArray payload, const QHostAddress &to, int headerBytes = 20) {
    QByteArray header(headerBytes, '\0');
    header[0] = char(0x40 | headerBytes / 4);
    header[9] = 1;
    qToBigEndian(to.toIPv4Address(), header.data() + 16);
    return header + payload;
}

QByteArray ipv6(QByteArray payload, int next) {
    QByteArray header(40, '\0');
    header[0] = 0x60;
    header[6] = char(next);
    std::memcpy(header.data() + 24, target6.toIPv6Address().c, 16);
    return header + payload;
}

QByteArray echo(int type) {
    QByteArray message(16, '\0');
    message[0] = char(type);
    qToBigEndian(key, message.data() + 4);
    qToBigEndian(cookie, message.data() + 8);
    return message;
}
}

class DarwinIcmpTest : public QObject {
    Q_OBJECT
private slots:
    void echoReplies() {
        const auto v4 = DarwinIcmp::parse(ipv4(echo(0), QHostAddress::LocalHost, 24), target4);
        QVERIFY(v4 && v4->echo && v4->key == key && v4->cookie == cookie);
        const auto v6 = DarwinIcmp::parse(echo(129), target6);
        QVERIFY(v6 && v6->echo && v6->key == key && v6->cookie == cookie);
        QVERIFY(!DarwinIcmp::parse(echo(128), target6));
        QVERIFY(!DarwinIcmp::parse(ipv4(echo(0).left(15), QHostAddress::LocalHost), target4));
    }

    void ipv4Errors() {
        QByteArray error(8, '\0');
        error[0] = 11;
        const auto quote = ipv4(echo(8).left(8), target4, 24);
        auto decoded = DarwinIcmp::parse(ipv4(error + quote, QHostAddress::LocalHost), target4);
        QVERIFY(decoded && decoded->key == key && decoded->transit && !decoded->echo);
        error[0] = 3;
        error[1] = 4;
        qToBigEndian(quint16(1412), error.data() + 6);
        const auto packet = ipv4(error + quote, QHostAddress::LocalHost);
        decoded = DarwinIcmp::parse(packet, target4);
        QVERIFY(decoded && decoded->mtu == 1412 && decoded->code == 4 && !decoded->transit);
        QVERIFY(!DarwinIcmp::parse(packet, QHostAddress::LocalHost));
        for (int bytes = 0; bytes < packet.size(); ++bytes)
            QVERIFY(!DarwinIcmp::parse(QByteArrayView(packet).first(bytes), target4));
        auto fragment = quote;
        fragment[7] = 1;
        QVERIFY(!DarwinIcmp::parse(ipv4(error + fragment, QHostAddress::LocalHost), target4));
    }

    void ipv6Errors() {
        QByteArray error(8, '\0'), extension(8, '\0');
        error[0] = 3;
        extension[0] = 58;
        const auto quote = ipv6(extension + echo(128).left(8), 60);
        auto decoded = DarwinIcmp::parse(error + quote, target6);
        QVERIFY(decoded && decoded->transit && decoded->key == key);
        error[0] = 2;
        qToBigEndian(quint32(1280), error.data() + 4);
        const auto packet = error + quote;
        decoded = DarwinIcmp::parse(packet, target6);
        QVERIFY(decoded && decoded->mtu == 1280 && !decoded->transit);
        QVERIFY(!DarwinIcmp::parse(packet, QHostAddress::LocalHostIPv6));
        for (int bytes = 0; bytes < packet.size(); ++bytes)
            QVERIFY(!DarwinIcmp::parse(QByteArrayView(packet).first(bytes), target6));
        extension[3] = 8;
        QVERIFY(!DarwinIcmp::parse(error + ipv6(extension + echo(128), 44), target6));
        QVERIFY(!DarwinIcmp::parse(error + ipv6(echo(128), 17), target6));
    }

    void checksum() {
        QCOMPARE(DarwinIcmp::checksum(QByteArray::fromHex("0001f203f4f5f6f7")), quint16(0x220d));
        auto odd = echo(8) + QByteArray(1, char(0xb0));
        qToBigEndian(DarwinIcmp::checksum(odd), odd.data() + 2);
        QCOMPARE(DarwinIcmp::checksum(odd), quint16(0));
    }
};

QTEST_APPLESS_MAIN(DarwinIcmpTest)
#include "darwinicmp_test.moc"
