/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "chatgptauth.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QUuid>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

using namespace OkulAIr;
namespace
{
QString randomValue()
{
    quint32 values[8];
    QRandomGenerator::system()->fillRange(values);
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(values), sizeof(values)).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
QByteArray decode64(const QString &value)
{
    return QByteArray::fromBase64(value.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
}
QString encodedForm(const QUrlQuery &query)
{
    QByteArray result;
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        if (!result.isEmpty()) result += '&';
        result += QUrl::toPercentEncoding(item.first) + '=' + QUrl::toPercentEncoding(item.second);
    }
    return QString::fromLatin1(result);
}
bool trustedAuthUrl(const QString &value)
{
    const QUrl url(value);
    return url.scheme() == QLatin1String("https") && url.host() == QLatin1String("auth.openai.com") && url.port(-1) == -1 && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}
}

QJsonObject ChatGPTAuth::verifyIdToken(const QString &jwt, const QJsonObject &jwks, const QString &client, const QString &nonce, QString *error)
{
    *error = QStringLiteral("ChatGPT identity could not be verified. Please sign in again.");
    if (jwt.size() > 32768) return {};
    const auto parts = jwt.split(QLatin1Char('.'));
    if (parts.size() != 3) return {};
    const auto header = QJsonDocument::fromJson(decode64(parts[0])).object();
    const auto claims = QJsonDocument::fromJson(decode64(parts[1])).object();
    const auto algorithm = header.value(QStringLiteral("alg")).toString();
    if ((algorithm != QLatin1String("RS256") && algorithm != QLatin1String("ES256")) || header.contains(QStringLiteral("crit")) || header.value(QStringLiteral("kid")).toString().isEmpty()) return {};
    QJsonObject key;
    for (const auto &entry : jwks.value(QStringLiteral("keys")).toArray()) {
        const auto candidate = entry.toObject();
        if (candidate.value(QStringLiteral("kid")) == header.value(QStringLiteral("kid"))) {
            if (!key.isEmpty()) return {};
            key = candidate;
        }
    }
    if (key.isEmpty() || (key.contains(QStringLiteral("alg")) && key.value(QStringLiteral("alg")) != algorithm)
        || (key.contains(QStringLiteral("use")) && key.value(QStringLiteral("use")) != QLatin1String("sig"))) return {};
    std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
    if (!builder) return {};
    EVP_PKEY *rawKey = nullptr;
    QByteArray signature = decode64(parts[2]);
    const bool rsa = algorithm == QLatin1String("RS256");
    const auto n = decode64(key.value(QStringLiteral("n")).toString());
    const auto e = decode64(key.value(QStringLiteral("e")).toString());
    std::unique_ptr<BIGNUM, decltype(&BN_free)> modulus(BN_bin2bn(reinterpret_cast<const unsigned char *>(n.constData()), n.size(), nullptr), BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> exponent(BN_bin2bn(reinterpret_cast<const unsigned char *>(e.constData()), e.size(), nullptr), BN_free);
    QByteArray point;
    if (rsa) {
        if (key.value(QStringLiteral("kty")) != QLatin1String("RSA") || n.size() < 256 || n.size() > 1024 || e.isEmpty() || e.size() > 8 || !modulus || !exponent) return {};
        if (!OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, modulus.get()) || !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, exponent.get())) return {};
    } else {
        const auto x = decode64(key.value(QStringLiteral("x")).toString());
        const auto y = decode64(key.value(QStringLiteral("y")).toString());
        if (key.value(QStringLiteral("kty")) != QLatin1String("EC") || key.value(QStringLiteral("crv")) != QLatin1String("P-256") || x.size() != 32 || y.size() != 32 || signature.size() != 64) return {};
        point = QByteArray(1, '\x04') + x + y;
        if (!OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0)
            || !OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, point.constData(), point.size())) return {};
        std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> ecSignature(ECDSA_SIG_new(), ECDSA_SIG_free);
        auto *r = BN_bin2bn(reinterpret_cast<const unsigned char *>(signature.constData()), 32, nullptr);
        auto *s = BN_bin2bn(reinterpret_cast<const unsigned char *>(signature.constData() + 32), 32, nullptr);
        if (!ecSignature || !r || !s || !ECDSA_SIG_set0(ecSignature.get(), r, s)) { BN_free(r); BN_free(s); return {}; }
        const int length = i2d_ECDSA_SIG(ecSignature.get(), nullptr);
        if (length <= 0) return {};
        signature.resize(length);
        auto *destination = reinterpret_cast<unsigned char *>(signature.data());
        if (i2d_ECDSA_SIG(ecSignature.get(), &destination) != length) return {};
    }
    std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> parameters(OSSL_PARAM_BLD_to_param(builder.get()), OSSL_PARAM_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(EVP_PKEY_CTX_new_from_name(nullptr, rsa ? "RSA" : "EC", nullptr), EVP_PKEY_CTX_free);
    if (!parameters || !keyContext || EVP_PKEY_fromdata_init(keyContext.get()) != 1 || EVP_PKEY_fromdata(keyContext.get(), &rawKey, EVP_PKEY_PUBLIC_KEY, parameters.get()) != 1) return {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(rawKey, EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    const QByteArray signedData = parts[0].toLatin1() + '.' + parts[1].toLatin1();
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, publicKey.get()) != 1
        || EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char *>(signature.constData()), signature.size(), reinterpret_cast<const unsigned char *>(signedData.constData()), signedData.size()) != 1) return {};
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const auto audience = claims.value(QStringLiteral("aud"));
    const bool audienceMatches = audience == client || (audience.isArray() && audience.toArray().contains(client));
    if (claims.value(QStringLiteral("iss")) != QLatin1String("https://auth.openai.com") || !audienceMatches || claims.value(QStringLiteral("sub")).toString().isEmpty()
        || claims.value(QStringLiteral("exp")).toInteger() < now - 5 || !claims.contains(QStringLiteral("iat")) || claims.value(QStringLiteral("iat")).toInteger() > now + 5
        || (!nonce.isEmpty() && claims.value(QStringLiteral("nonce")) != nonce)
        || (claims.contains(QStringLiteral("nbf")) && claims.value(QStringLiteral("nbf")).toInteger() > now + 5)
        || (audience.isArray() && audience.toArray().size() > 1 && claims.value(QStringLiteral("azp")) != client)
        || (claims.contains(QStringLiteral("azp")) && claims.value(QStringLiteral("azp")) != client)) return {};
    error->clear();
    return claims;
}

QString ChatGPTAuth::grantError(const QJsonObject &tokens)
{
    const auto scopes = tokens.value(QStringLiteral("scope")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (!scopes.contains(QStringLiteral("chatgpt.tokens.use.direct")) || !scopes.contains(QStringLiteral("resource.invoke")))
        return QStringLiteral("ChatGPT plan access was not granted. Enable plan usage for OkulAIr in ChatGPT settings, then sign in again. No paid API fallback will be used.");
    if (tokens.value(QStringLiteral("access_token")).toString().isEmpty() || tokens.value(QStringLiteral("token_type")).toString().compare(QLatin1String("Bearer"), Qt::CaseInsensitive) != 0 || tokens.value(QStringLiteral("expires_in")).toInteger() <= 0)
        return QStringLiteral("ChatGPT returned an incomplete credential set. Please sign in again.");
    return {};
}

ChatGPTAuth *ChatGPTAuth::instance()
{
    static auto *auth = new ChatGPTAuth(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/okulair/chatgpt"), QCoreApplication::instance());
    return auth;
}
ChatGPTAuth::ChatGPTAuth(const QString &directory, QObject *parent) : QObject(parent), m_directory(directory), m_network(this)
{
    read();
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(5 * 60 * 1000);
    connect(&m_timeout, &QTimer::timeout, this, [this] { fail(QStringLiteral("ChatGPT sign-in timed out. Click Continue with ChatGPT to try again.")); });
    connect(&m_listener, &QTcpServer::newConnection, this, [this] {
        while (m_listener.hasPendingConnections()) {
            auto *socket = m_listener.nextPendingConnection();
            const auto epoch = m_epoch;
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(5000, socket, &QTcpSocket::disconnectFromHost);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, epoch] {
                if (epoch != m_epoch) { socket->disconnectFromHost(); return; }
                QByteArray input = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", input);
                if (!input.contains("\r\n\r\n") && input.size() < 8192) return;
                const auto line = input.left(input.indexOf("\r\n")).split(' ');
                const QUrl url = line.size() == 3 ? QUrl::fromEncoded(line[1]) : QUrl();
                const QUrlQuery query(url);
                const bool valid = m_busy && !m_state.isEmpty() && input.size() < 8192 && line.size() == 3 && line[0] == "GET" && url.path() == QLatin1String("/auth/callback")
                    && !query.queryItemValue(QStringLiteral("state")).isEmpty() && query.queryItemValue(QStringLiteral("state")) == m_state;
                const QByteArray response = valid ? QByteArray("Sign-in response received. You can return to OkulAIr.") : QByteArray("Invalid sign-in callback. Return to OkulAIr.");
                socket->write(QByteArray(valid ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 400 Bad Request\r\n") + "Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + QByteArray::number(response.size()) + "\r\n\r\n" + response);
                socket->disconnectFromHost();
                if (!valid) return;
                m_listener.close();
                m_state.clear();
                if (query.hasQueryItem(QStringLiteral("error"))) { fail(QStringLiteral("ChatGPT sign-in was declined or unavailable. No API-key billing was enabled.")); return; }
                const auto returnedClient = query.queryItemValue(QStringLiteral("client_id"));
                const QString client = m_loginClient.isEmpty() ? returnedClient : m_loginClient;
                if (client.isEmpty() || client == QLatin1String("dynamic_agent_client") || (!m_loginClient.isEmpty() && !returnedClient.isEmpty() && returnedClient != m_loginClient)
                    || query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded).isEmpty()) { fail(QStringLiteral("ChatGPT registration did not return the expected client and code.")); return; }
                exchange(query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded), client);
            });
        }
    });
}
ChatGPTAuth::~ChatGPTAuth() { cancel(); }
QJsonArray ChatGPTAuth::accounts() const { return m_store.value(QStringLiteral("accounts")).toArray(); }
QString ChatGPTAuth::activeClient() const { return m_store.value(QStringLiteral("active")).toString(); }
QJsonObject ChatGPTAuth::account() const
{
    for (const auto &entry : accounts()) if (entry.toObject().value(QStringLiteral("client_id")) == activeClient()) return entry.toObject();
    return {};
}
QString ChatGPTAuth::accountLabel() const
{
    const auto current = account();
    return current.value(QStringLiteral("email")).toString().isEmpty() ? QStringLiteral("ChatGPT account") : current.value(QStringLiteral("email")).toString();
}
QString ChatGPTAuth::accessToken() const
{
    const auto current = account();
    return current.value(QStringLiteral("expires_at")).toInteger() > QDateTime::currentSecsSinceEpoch() + 60 ? current.value(QStringLiteral("access_token")).toString() : QString();
}
bool ChatGPTAuth::busy() const { return m_busy; }
bool ChatGPTAuth::hasCredentials() const { return !accessToken().isEmpty() || !account().value(QStringLiteral("refresh_token")).toString().isEmpty(); }
void ChatGPTAuth::read()
{
    m_store = {};
    QFile file(m_directory + QStringLiteral("/accounts.json"));
    const QFileInfo info(file);
    if (info.isSymLink() || (info.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther))) return;
    if (file.open(QIODevice::ReadOnly) && file.size() < 1024 * 1024) m_store = QJsonDocument::fromJson(file.readAll()).object();
}
bool ChatGPTAuth::lock()
{
    if (!QDir().mkpath(m_directory) || QFileInfo(m_directory).isSymLink() || !QFile::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) { fail(QStringLiteral("Could not create a private ChatGPT connection folder.")); return false; }
    m_lock = std::make_unique<QLockFile>(m_directory + QStringLiteral("/accounts.lock"));
    if (!m_lock->tryLock(0)) { fail(QStringLiteral("Another OkulAIr window is updating ChatGPT sign-in. Try again shortly.")); return false; }
    read();
    return true;
}
bool ChatGPTAuth::write()
{
    QSaveFile file(m_directory + QStringLiteral("/accounts.json"));
    const auto data = QJsonDocument(m_store).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || file.write(data) != data.size() || !file.commit()) { read(); fail(QStringLiteral("Could not securely save the ChatGPT connection.")); return false; }
    return true;
}
void ChatGPTAuth::cancel()
{
    ++m_epoch;
    m_listener.close();
    m_timeout.stop();
    m_state.clear(); m_nonce.clear(); m_verifier.clear();
    if (m_reply) { auto *reply = m_reply.data(); m_reply = nullptr; reply->abort(); reply->deleteLater(); }
    m_lock.reset();
    m_busy = false;
}
void ChatGPTAuth::fail(const QString &message) { cancel(); Q_EMIT changed(); Q_EMIT error(message); }
void ChatGPTAuth::request(const QUrl &url, const QByteArray &body, const QByteArray &bearer, Result callback, qint64 maximumBytes)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    if (!bearer.isEmpty()) request.setRawHeader("Authorization", "Bearer " + bearer);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    auto *reply = body.isEmpty() ? m_network.get(request) : m_network.post(request, body);
    m_reply = reply;
    reply->setReadBufferSize(1024 * 1024);
    const auto epoch = m_epoch;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes, maximumBytes] {
        *bytes += reply->readAll();
        if (bytes->size() > maximumBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, epoch, bytes, callback, maximumBytes] {
        reply->deleteLater();
        if (epoch != m_epoch) return;
        m_reply = nullptr;
        *bytes += reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString transportError;
        if (bytes->size() > maximumBytes) {
            transportError = QStringLiteral("The response exceeded the download limit (%1 KiB).").arg(maximumBytes / 1024);
        } else if (reply->error() != QNetworkReply::NoError && (status == 0 || status < 400)) {
            switch (reply->error()) {
            case QNetworkReply::HostNotFoundError:
                transportError = QStringLiteral("The server could not be found. Check your internet connection and DNS."); break;
            case QNetworkReply::TimeoutError:
            case QNetworkReply::OperationCanceledError:
                transportError = QStringLiteral("The request timed out or was interrupted. Check your connection and retry."); break;
            case QNetworkReply::SslHandshakeFailedError:
                transportError = QStringLiteral("The secure connection failed. Check your system clock, certificates, and proxy settings."); break;
            default:
                transportError = QStringLiteral("The network connection failed (network error %1). Check your connection and retry.").arg(int(reply->error())); break;
            }
        }
        if (!transportError.isEmpty()) { callback({}, status, transportError); return; }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(*bytes, &parseError);
        if (status == 200 && !bytes->isEmpty() && (parseError.error != QJsonParseError::NoError || !document.isObject())) {
            callback({}, status, QStringLiteral("The server returned an invalid JSON response.")); return;
        }
        callback(document.object(), status, {});
    });
}
void ChatGPTAuth::discover(std::function<void()> callback)
{
    if (!m_discovery.isEmpty()) { callback(); return; }
    request(QUrl(QStringLiteral("https://auth.openai.com/.well-known/openid-configuration")), {}, {}, [this, callback](const QJsonObject &value, int status, const QString &transportError) {
        if (!transportError.isEmpty()) { fail(QStringLiteral("Could not connect to ChatGPT sign-in: %1").arg(transportError)); return; }
        if (status != 200 || value.value(QStringLiteral("issuer")) != QLatin1String("https://auth.openai.com")
            || !trustedAuthUrl(value.value(QStringLiteral("authorization_endpoint")).toString()) || !trustedAuthUrl(value.value(QStringLiteral("token_endpoint")).toString())
            || !trustedAuthUrl(value.value(QStringLiteral("jwks_uri")).toString())) { fail(QStringLiteral("The ChatGPT sign-in configuration could not be verified.")); return; }
        m_discovery = value;
        callback();
    });
}
void ChatGPTAuth::signIn(bool newAccount)
{
    if (m_busy) return;
    if (!lock()) return;
    if (m_store.value(QStringLiteral("host")).toString().isEmpty()) {
        m_store.insert(QStringLiteral("host"), QString(QStringLiteral("urn:uuid:") + QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!write()) return;
    }
    m_loginClient = newAccount ? QString() : activeClient();
    m_busy = true;
    Q_EMIT changed();
    discover([this] { listen(); });
}
void ChatGPTAuth::listen()
{
    if (!m_listener.listen(QHostAddress::LocalHost, 0)) { fail(QStringLiteral("Could not start the browser sign-in callback on 127.0.0.1.")); return; }
    m_state = randomValue(); m_nonce = randomValue(); m_verifier = randomValue();
    m_redirect = QStringLiteral("http://127.0.0.1:%1/auth/callback").arg(m_listener.serverPort());
    QUrl url(m_discovery.value(QStringLiteral("authorization_endpoint")).toString());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), m_loginClient.isEmpty() ? QStringLiteral("dynamic_agent_client") : m_loginClient);
    if (m_loginClient.isEmpty()) query.addQueryItem(QStringLiteral("agent_name_hint"), QStringLiteral("OkulAIr"));
    query.addQueryItem(QStringLiteral("ext_agent_host_id"), m_store.value(QStringLiteral("host")).toString());
    query.addQueryItem(QStringLiteral("redirect_uri"), m_redirect);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("scope"), QStringLiteral("openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"));
    query.addQueryItem(QStringLiteral("resource"), QStringLiteral("https://api.openai.com/v1"));
    query.addQueryItem(QStringLiteral("state"), m_state);
    query.addQueryItem(QStringLiteral("nonce"), m_nonce);
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    // Deliberately omit id_token_hint: desktop browser launch diagnostics may log URLs.
    url.setQuery(query);
    m_timeout.start();
    if (!QDesktopServices::openUrl(url)) { fail(QStringLiteral("Could not open your browser for ChatGPT sign-in.")); return; }
    Q_EMIT notice(QStringLiteral("Finish sign-in and plan-use consent in your browser, then return to OkulAIr. No API key is needed."));
}
void ChatGPTAuth::exchange(const QString &code, const QString &client)
{
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    body.addQueryItem(QStringLiteral("client_id"), client);
    body.addQueryItem(QStringLiteral("code"), code);
    body.addQueryItem(QStringLiteral("code_verifier"), m_verifier);
    body.addQueryItem(QStringLiteral("redirect_uri"), m_redirect);
    body.addQueryItem(QStringLiteral("resource"), QStringLiteral("https://api.openai.com/v1"));
    request(QUrl(m_discovery.value(QStringLiteral("token_endpoint")).toString()), encodedForm(body).toUtf8(), {}, [this, client](const QJsonObject &tokens, int status, const QString &transportError) {
        if (!transportError.isEmpty()) { fail(QStringLiteral("Could not complete ChatGPT sign-in: %1").arg(transportError)); return; }
        if (status != 200) { fail(QStringLiteral("ChatGPT did not complete sign-in (HTTP %1). Try signing in again; account or workspace policy may restrict access.").arg(status)); return; }
        acceptTokens(tokens, client, false);
    });
}
void ChatGPTAuth::replaceAccount(const QJsonObject &value)
{
    auto records = accounts();
    bool replaced = false;
    for (qsizetype i = 0; i < records.size(); ++i) if (records[i].toObject().value(QStringLiteral("client_id")) == value.value(QStringLiteral("client_id"))) { records[i] = value; replaced = true; break; }
    if (!replaced) records.append(value);
    m_store.insert(QStringLiteral("accounts"), records);
    m_store.insert(QStringLiteral("active"), value.value(QStringLiteral("client_id")));
}
void ChatGPTAuth::acceptTokens(const QJsonObject &tokens, const QString &client, bool refreshing)
{
    QJsonObject combined = tokens;
    QJsonObject previous;
    for (const auto &entry : accounts()) if (entry.toObject().value(QStringLiteral("client_id")) == client) previous = entry.toObject();
    if (refreshing && !combined.contains(QStringLiteral("scope"))) combined.insert(QStringLiteral("scope"), previous.value(QStringLiteral("scope")));
    const auto invalid = grantError(combined);
    if (!invalid.isEmpty()) { fail(invalid); return; }
    const auto save = [this, combined, previous, client, refreshing](const QJsonObject &claims) {
        if (!previous.value(QStringLiteral("subject")).toString().isEmpty() && previous.value(QStringLiteral("client_id")) == client && previous.value(QStringLiteral("subject")) != claims.value(QStringLiteral("sub"))) { fail(QStringLiteral("ChatGPT returned a different identity for the selected registration. The previous connection was preserved.")); return; }
        if (claims.contains(QStringLiteral("at_hash"))) {
            const auto hash = QCryptographicHash::hash(combined.value(QStringLiteral("access_token")).toString().toLatin1(), QCryptographicHash::Sha256).left(16).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
            if (claims.value(QStringLiteral("at_hash")) != QString::fromLatin1(hash)) { fail(QStringLiteral("ChatGPT access-token verification failed.")); return; }
        }
        auto saved = refreshing ? previous : QJsonObject();
        for (const auto &name : {QStringLiteral("access_token"), QStringLiteral("refresh_token"), QStringLiteral("id_token"), QStringLiteral("scope")}) if (combined.contains(name)) saved.insert(name, combined.value(name));
        saved.insert(QStringLiteral("client_id"), client);
        saved.insert(QStringLiteral("subject"), claims.value(QStringLiteral("sub")));
        saved.insert(QStringLiteral("email"), claims.value(QStringLiteral("email")));
        saved.insert(QStringLiteral("expires_at"), QDateTime::currentSecsSinceEpoch() + combined.value(QStringLiteral("expires_in")).toInteger());
        replaceAccount(saved);
        if (!write()) return;
        cancel();
        Q_EMIT changed();
        Q_EMIT ready();
    };
    if (refreshing && !combined.contains(QStringLiteral("id_token"))) {
        // A refresh remains bound to the previously verified subject/client pair.
        save(QJsonObject{{QStringLiteral("sub"), previous.value(QStringLiteral("subject"))}, {QStringLiteral("email"), previous.value(QStringLiteral("email"))}});
        return;
    }
    request(QUrl(m_discovery.value(QStringLiteral("jwks_uri")).toString()), {}, {}, [this, combined, client, refreshing, save](const QJsonObject &keys, int status, const QString &transportError) {
        if (!transportError.isEmpty()) { fail(QStringLiteral("Could not verify ChatGPT signing keys: %1").arg(transportError)); return; }
        QString error;
        const auto claims = verifyIdToken(combined.value(QStringLiteral("id_token")).toString(), keys, client, refreshing ? QString() : m_nonce, &error);
        if (status != 200 || claims.isEmpty()) { fail(error.isEmpty() ? QStringLiteral("Could not verify ChatGPT signing keys.") : error); return; }
        save(claims);
    });
}
void ChatGPTAuth::ensureAccess()
{
    if (m_busy) { Q_EMIT error(QStringLiteral("ChatGPT sign-in is already in progress. Finish it in your browser first.")); return; }
    if (!lock()) return;
    if (!accessToken().isEmpty()) { m_lock.reset(); Q_EMIT ready(); return; }
    if (account().value(QStringLiteral("refresh_token")).toString().isEmpty()) { m_lock.reset(); Q_EMIT error(QStringLiteral("No ChatGPT service is connected. Click Continue with ChatGPT first.")); return; }
    m_busy = true;
    Q_EMIT changed();
    discover([this] { refresh(); });
}
void ChatGPTAuth::refresh()
{
    const auto current = account();
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    body.addQueryItem(QStringLiteral("client_id"), activeClient());
    body.addQueryItem(QStringLiteral("refresh_token"), current.value(QStringLiteral("refresh_token")).toString());
    body.addQueryItem(QStringLiteral("resource"), QStringLiteral("https://api.openai.com/v1"));
    const QString client = activeClient();
    request(QUrl(m_discovery.value(QStringLiteral("token_endpoint")).toString()), encodedForm(body).toUtf8(), {}, [this, client](const QJsonObject &tokens, int status, const QString &transportError) {
        if (!transportError.isEmpty()) { fail(QStringLiteral("Could not renew the ChatGPT connection: %1 Your saved connection was preserved.").arg(transportError)); return; }
        if (status != 200) {
            const auto code = tokens.value(QStringLiteral("error")).toString();
            if (QStringList{QStringLiteral("invalid_grant"), QStringLiteral("invalid_refresh_token"), QStringLiteral("token_expired"), QStringLiteral("refresh_token_expired"), QStringLiteral("refresh_token_invalidated"), QStringLiteral("refresh_token_reused")}.contains(code)) {
                auto invalid = account();
                for (const auto &key : {QStringLiteral("access_token"), QStringLiteral("refresh_token"), QStringLiteral("id_token"), QStringLiteral("expires_at")}) invalid.remove(key);
                replaceAccount(invalid);
                if (!write()) return;
            }
            fail(QStringLiteral("ChatGPT could not renew this session (HTTP %1). Retry later or use Continue with ChatGPT to reconnect. No API fallback was used.").arg(status)); return;
        }
        acceptTokens(tokens, client, true);
    });
}
void ChatGPTAuth::selectAccount(const QString &client)
{
    if (m_busy || client == activeClient() || !lock()) return;
    for (const auto &entry : accounts()) if (entry.toObject().value(QStringLiteral("client_id")) == client) { m_store.insert(QStringLiteral("active"), client); if (!write()) return; break; }
    m_lock.reset();
    Q_EMIT changed();
}
void ChatGPTAuth::fetchModels()
{
    if (m_busy || accessToken().isEmpty()) return;
    m_busy = true;
    Q_EMIT changed();
    request(QUrl(QStringLiteral("https://api.openai.com/v1/models")), {}, accessToken().toUtf8(), [this](const QJsonObject &value, int status, const QString &transportError) {
        m_busy = false;
        Q_EMIT changed();
        if (!transportError.isEmpty()) { Q_EMIT error(QStringLiteral("Could not load ChatGPT models: %1 Your sign-in is saved. Click Continue with ChatGPT to retry.").arg(transportError)); return; }
        if (status == 401) { Q_EMIT error(QStringLiteral("ChatGPT rejected this session (HTTP 401). Sign out and reconnect your account.")); return; }
        if (status == 403) { Q_EMIT error(QStringLiteral("ChatGPT model access was denied (HTTP 403). Review plan access and workspace policy in ChatGPT settings.")); return; }
        if (status != 200) { Q_EMIT error(QStringLiteral("Could not load ChatGPT models (HTTP %1). Retry shortly; your sign-in is saved.").arg(status)); return; }
        if (!value.value(QStringLiteral("models")).isArray()) { Q_EMIT error(QStringLiteral("ChatGPT returned an unexpected model catalog. Your sign-in is saved.")); return; }
        QJsonArray visible;
        for (const auto &entry : value.value(QStringLiteral("models")).toArray()) {
            const auto model = entry.toObject();
            if (model.value(QStringLiteral("visibility")) == QLatin1String("list") && !model.value(QStringLiteral("slug")).toString().isEmpty()) visible.append(model);
        }
        if (visible.isEmpty()) { Q_EMIT error(QStringLiteral("No models are available for this ChatGPT account or workspace.")); return; }
        Q_EMIT modelsReady(visible);
    }, ModelCatalogLimit); // Account catalogs include extensive model metadata.
}
void ChatGPTAuth::signOut()
{
    if (m_busy) cancel();
    if (!lock()) return;
    m_busy = true;
    const auto localSignOut = [this](bool revoked) {
        auto value = account();
        for (const auto &key : {QStringLiteral("access_token"), QStringLiteral("refresh_token"), QStringLiteral("id_token"), QStringLiteral("expires_at")}) value.remove(key);
        replaceAccount(value);
        if (!write()) return;
        cancel(); Q_EMIT changed();
        Q_EMIT notice(revoked ? QStringLiteral("Signed out of ChatGPT.") : QStringLiteral("Signed out locally. Remote revocation was not confirmed; disconnect OkulAIr in ChatGPT settings if needed."));
    };
    const auto refreshToken = account().value(QStringLiteral("refresh_token")).toString();
    if (refreshToken.isEmpty()) { localSignOut(true); return; }
    // Discovery failure must not prevent local sign-out.
    const auto revoke = [this, refreshToken, localSignOut] {
        const auto endpoint = m_discovery.value(QStringLiteral("revocation_endpoint")).toString();
        if (!trustedAuthUrl(endpoint)) { localSignOut(false); return; }
        QUrlQuery body;
        body.addQueryItem(QStringLiteral("token"), refreshToken);
        body.addQueryItem(QStringLiteral("token_type_hint"), QStringLiteral("refresh_token"));
        body.addQueryItem(QStringLiteral("client_id"), activeClient());
        request(QUrl(endpoint), encodedForm(body).toUtf8(), {}, [localSignOut](const QJsonObject &, int status, const QString &transportError) { localSignOut(status == 200 && transportError.isEmpty()); });
    };
    // Use cached discovery when available; otherwise clear locally and explain.
    if (m_discovery.isEmpty()) localSignOut(false); else revoke();
}
