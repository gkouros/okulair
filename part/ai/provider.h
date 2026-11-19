/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkReply;
class QProcess;
namespace OkulAIr
{
enum class Protocol { Compatible, OpenAI, Anthropic, ChatGPT, GeminiAccount, CopilotAccount, QwenAccount };
struct Connection {
    Protocol protocol = Protocol::Compatible;
    QUrl endpoint;
    QString model;
    QString key;
    bool requiresKey = false;
    QString effort = {}; // Empty uses the provider default.
    QJsonObject modelMetadata = {};
};

// Stateful SSE decoding is separate from the network so framing can be tested.
class EventStream
{
public:
    explicit EventStream(Protocol protocol) : m_protocol(protocol) {}
    void feed(const QByteArray &bytes);
    QString text;
    QString error;
    bool complete = false;
private:
    void event(const QByteArray &data);
    Protocol m_protocol;
    QByteArray m_buffer;
    qsizetype m_bytes = 0;
};

class Provider : public QObject
{
    Q_OBJECT
public:
    explicit Provider(QObject *parent = nullptr);
    ~Provider() override;
    static bool accountClient(Protocol protocol);
    static QString accountExecutable(Protocol protocol);
    static QStringList effortOptions(const Connection &connection);
    static QString defaultEffort(const Connection &connection);
    static QString validate(const Connection &connection);
    static QJsonObject requestBody(const Connection &connection, const QString &instructions, const QJsonArray &history, const QByteArray &image = {});
    void start(const Connection &connection, const QString &instructions, const QJsonArray &history, const QByteArray &image = {});
    void cancel();
    void listModels(const Connection &connection);
Q_SIGNALS:
    void updated(const QString &text);
    void modelsListed(const QJsonArray &models, const QString &error);
    void finished(const QString &text, const QString &error);
private:
    void startAccountClient(const Connection &, const QString &, const QJsonArray &, const QByteArray &);
    QPointer<QProcess> m_process;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};
}
