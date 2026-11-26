#include "localreceiver.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSslCertificate>
#include <QStandardPaths>

namespace {
QString certificateExpiryWarning(const QString &certPath) {
    QFile file(certPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto certs = QSslCertificate::fromDevice(&file, QSsl::Pem);
    if (certs.isEmpty() || certs.first().isNull())
        return {};
    const auto expiry = certs.first().expiryDate();
    if (!expiry.isValid())
        return {};
    const auto now = QDateTime::currentDateTimeUtc();
    if (expiry <= now)
        return LocalReceiver::tr(" Receiver certificate expired on %1.").arg(expiry.date().toString(Qt::ISODate));
    const auto daysLeft = now.daysTo(expiry);
    if (daysLeft <= 30)
        return LocalReceiver::tr(" Receiver certificate expires in %n day(s).", nullptr, int(daysLeft));
    return {};
}
} // namespace

LocalReceiver::LocalReceiver(QObject *parent) : QObject(parent) {
#ifdef Q_OS_WIN
    // A console-only Windows child has no window to receive QProcess::terminate().
    // Closing its private stdin pipe requests a flush and also handles parent exit.
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("HOPVIEW_PARENT_PIPE", "1");
    server_.setProcessEnvironment(environment);
#endif
    killTimer_.setSingleShot(true);
    killTimer_.setInterval(3000);
    connect(&killTimer_, &QTimer::timeout, this, [this] {
        if (certificate_.state() != QProcess::NotRunning)
            certificate_.kill();
        if (server_.state() != QProcess::NotRunning)
            server_.kill();
    });
    connect(&certificate_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            killTimer_.stop();
            staging_.reset();
            stopping_ = false;
            report(false, false, tr("Could not start certificate generation: %1").arg(certificate_.errorString()));
        }
    });
    connect(&certificate_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        killTimer_.stop();
        if (stopping_) {
            staging_.reset();
            stopping_ = false;
            report(false, false, tr("Receiver stopped."));
            return;
        }
        const bool created = status == QProcess::NormalExit && code == 0 && staging_ &&
                             QDir().rename(staging_->path(), directory_ + "/identity");
        staging_.reset();
        if (!created) {
            report(false, false, tr("Could not create the receiver certificate in %1.").arg(directory_));
            return;
        }
        launchServer();
    });
    server_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&server_, &QProcess::readyReadStandardOutput, this, [this] {
        while (server_.canReadLine()) {
            lastLine_ = QString::fromUtf8(server_.readLine()).trimmed().left(512);
            if (!stopping_ && lastLine_.startsWith("Receiving telemetry on "))
                report(false, true,
                       tr("Listening on port %1 while Hopview is running.").arg(endpoint_.port) +
                           certificateExpiryWarning(directory_ + "/identity/cert.pem"));
        }
    });
    connect(&server_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            killTimer_.stop();
            stopping_ = false;
            report(false, false, tr("Could not start the receiver: %1").arg(server_.errorString()));
        }
    });
    connect(&server_, &QProcess::finished, this, [this] {
        killTimer_.stop();
        const bool wasStopping = stopping_;
        stopping_ = false;
        report(false, false, wasStopping ? tr("Receiver stopped.") :
               tr("Receiver exited. %1").arg(lastLine_.isEmpty() ? server_.errorString() : lastLine_));
    });
}

LocalReceiver::~LocalReceiver() {
    killTimer_.stop();
    for (auto *process : {&certificate_, &server_}) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
#ifdef Q_OS_WIN
            if (process == &server_)
                process->closeWriteChannel();
            else
#endif
            process->terminate();
            if (!process->waitForFinished(2000)) {
                process->kill();
                process->waitForFinished();
            }
        }
    }
}

void LocalReceiver::setDatabase(const QString &path) {
    if (database_ != path && !busy_ && !ready_) {
        database_ = path;
        emit databaseChanged();
    }
}

void LocalReceiver::report(bool busy, bool ready, const QString &message) {
    busy_ = busy;
    ready_ = ready;
    message_ = message;
    emit stateChanged();
}

void LocalReceiver::start(const QString &address) {
    if (busy_ || ready_)
        return;
    if (database_.isEmpty() || !telemetry::parseReceiverEndpoint(address.trimmed(), endpoint_)) {
        report(false, false, tr("Enter the receiver hostname or IP address that the VMs can reach."));
        return;
    }
    const auto db = QFileInfo(database_).absoluteFilePath();
    const auto id = QCryptographicHash::hash(db.toUtf8(), QCryptographicHash::Sha256).toHex();
    directory_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                     .filePath("receivers/" + QString::fromLatin1(id));
    if (!QDir().mkpath(directory_) ||
        !QFile::setPermissions(directory_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        report(false, false, tr("Cannot prepare the receiver directory %1.").arg(directory_));
        return;
    }
    stopping_ = false;
    lastLine_.clear();
    report(true, false, tr("Starting receiver…"));
    if (QFileInfo::exists(directory_ + "/identity")) {
        launchServer();
        return;
    }
    staging_ = std::make_unique<QTemporaryDir>(directory_ + "/identity-XXXXXX");
    if (!staging_->isValid()) {
        report(false, false, tr("Cannot prepare the receiver credentials."));
        return;
    }
    const auto host = endpoint_.host.section('%', 0, 0);
    const auto alt = (QHostAddress(host).isNull() ? "DNS:" : "IP:") + host;
    const QString bundledOpenSsl = QStandardPaths::findExecutable("openssl", {QCoreApplication::applicationDirPath()});
    certificate_.start(bundledOpenSsl.isEmpty() ? "openssl" : bundledOpenSsl, {"req", "-x509", "-newkey", "rsa:3072", "-sha256", "-nodes",
                                    "-days", "730", "-subj", "/CN=Hopview receiver",
                                    "-addext", "subjectAltName=" + alt, "-keyout", staging_->filePath("key.pem"),
                                    "-out", staging_->filePath("cert.pem")});
}

void LocalReceiver::launchServer() {
    const auto file = directory_ + "/receiver.json";
    QSaveFile config(file);
    const auto bind = QHostAddress(endpoint_.host).isLoopback() ? endpoint_.host : QStringLiteral("*");
    const QJsonObject fields{{"listen", bind}, {"port", endpoint_.port},
                             {"database", QFileInfo(database_).absoluteFilePath()},
                             {"certificate", "identity/cert.pem"}, {"privateKey", "identity/key.pem"}};
    const auto bytes = QJsonDocument(fields).toJson();
    if (!config.open(QIODevice::WriteOnly) || config.write(bytes) != bytes.size() || !config.commit()) {
        report(false, false, tr("Cannot write receiver configuration: %1").arg(config.errorString()));
        return;
    }
    const QDir binaries(QCoreApplication::applicationDirPath());
    QString program = QStandardPaths::findExecutable("hopview-node", {binaries.path(), binaries.filePath("../build-node")});
    if (program.isEmpty())
        program = QStandardPaths::findExecutable("hopview-node");
    if (program.isEmpty()) {
        report(false, false, tr("The receiver executable hopview-node is missing."));
        return;
    }
    server_.start(program, {"receive", file});
}

void LocalReceiver::stop() {
    if (!ready_ && !busy_)
        return;
    stopping_ = true;
    if (certificate_.state() != QProcess::NotRunning) {
        report(true, false, tr("Stopping receiver…"));
        certificate_.terminate();
        killTimer_.start();
        return;
    }
    if (server_.state() != QProcess::NotRunning) {
        report(true, false, tr("Stopping receiver…"));
#ifdef Q_OS_WIN
        server_.closeWriteChannel();
#else
        server_.terminate();
#endif
        killTimer_.start();
        return;
    }
    killTimer_.stop();
    staging_.reset();
    stopping_ = false;
    report(false, false, tr("Receiver stopped."));
}
