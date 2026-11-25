#pragma once
#include <QHostAddress>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QTcpServer>
#include <memory>

namespace telemetry {
std::unique_ptr<QTcpServer> startReceiverViewer(quint16 port, const QByteArray &password,
                                                const QString &database, const QHostAddress &address,
                                                const QSslConfiguration &ssl);
}
