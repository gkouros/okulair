/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTcpServer>
#include <QTimer>
#include <functional>
#include <memory>

class QNetworkReply;
namespace OkulAIr
{
class ChatGPTAuth : public QObject
{
    Q_OBJECT
public:
    static ChatGPTAuth *instance();
    explicit ChatGPTAuth(const QString &directory, QObject *parent = nullptr);
    ~ChatGPTAuth() override;
    QJsonArray accounts() const;
    QString activeClient() const;
    QString accountLabel() const;
    QString accessToken() const;
    bool busy() const;
    bool hasCredentials() const;
    void signIn(bool newAccount = false);
    void selectAccount(const QString &client);
    void signOut();
    void cancel();
    void ensureAccess();
    void fetchModels();
    static QJsonObject verifyIdToken(const QString &jwt, const QJsonObject &jwks, const QString &client, const QString &nonce, QString *error);
    static QString grantError(const QJsonObject &tokens);
Q_SIGNALS:
    void changed();
    void ready();
    void modelsReady(const QJsonArray &models);
    void error(const QString &message);
    void notice(const QString &message);
private:
    friend class ChatGPTAuthTest;
    static constexpr qint64 ModelCatalogLimit = 8 * 1024 * 1024;
    using Result = std::function<void(const QJsonObject &, int, const QString &)>;
    void request(const QUrl &url, const QByteArray &body, const QByteArray &bearer, Result callback, qint64 maximumBytes = 512 * 1024);
    bool lock();
    void read();
    bool write();
    void fail(const QString &message);
    void discover(std::function<void()> callback);
    void listen();
    void exchange(const QString &code, const QString &client);
    void acceptTokens(const QJsonObject &tokens, const QString &client, bool refreshing);
    void refresh();
    QJsonObject account() const;
    void replaceAccount(const QJsonObject &value);
    QString m_directory;
    QJsonObject m_store;
    QJsonObject m_discovery;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTcpServer m_listener;
    QTimer m_timeout;
    std::unique_ptr<QLockFile> m_lock;
    QString m_state, m_nonce, m_verifier, m_redirect, m_loginClient;
    quint64 m_epoch = 0;
    bool m_busy = false;
};
}
