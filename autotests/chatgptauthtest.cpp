/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "part/ai/chatgptauth.h"
#include "part/ai/provider.h"
#include "part/ai/services.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QTest>
#include <QUrlQuery>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>

namespace OkulAIr
{
class BrowserCapture : public QObject
{
    Q_OBJECT
public:
    QUrl url;
public Q_SLOTS:
    void capture(const QUrl &value) { url = value; }
};

class ChatGPTAuthTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void signedIdentity_data();
    void signedIdentity();
    void rejectUnsignedAndMissingGrant();
    void browserCallbackAndPkce();
    void privateStorageAndSignOut();
    void subscriptionRequestAndPresets();
    void boundedResponses_data();
    void boundedResponses();
    void transportFailure();
    void visionRequestFormats();
};

namespace
{
QByteArray base64(const QByteArray &bytes) { return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); }
QByteArray number(BIGNUM *value)
{
    QByteArray bytes(BN_num_bytes(value), '\0');
    BN_bn2bin(value, reinterpret_cast<unsigned char *>(bytes.data()));
    BN_free(value);
    return base64(bytes);
}
}
void ChatGPTAuthTest::signedIdentity_data()
{
    QTest::addColumn<bool>("rsa");
    QTest::newRow("RS256") << true;
    QTest::newRow("ES256") << false;
}
void ChatGPTAuthTest::signedIdentity()
{
    QFETCH(bool, rsa);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rsa ? EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(2048)) : EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free);
    QVERIFY(key);
    QJsonObject jwk{{QStringLiteral("kid"), QStringLiteral("test-key")}, {QStringLiteral("kty"), rsa ? QStringLiteral("RSA") : QStringLiteral("EC")}};
    if (rsa) {
        BIGNUM *n = nullptr, *e = nullptr;
        QVERIFY(EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_RSA_N, &n));
        QVERIFY(EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_RSA_E, &e));
        jwk.insert(QStringLiteral("n"), QString::fromLatin1(number(n)));
        jwk.insert(QStringLiteral("e"), QString::fromLatin1(number(e)));
    } else {
        QByteArray point(65, '\0');
        size_t size = 0;
        QVERIFY(EVP_PKEY_get_octet_string_param(key.get(), OSSL_PKEY_PARAM_PUB_KEY, reinterpret_cast<unsigned char *>(point.data()), point.size(), &size));
        QCOMPARE(size, size_t(65));
        jwk.insert(QStringLiteral("crv"), QStringLiteral("P-256"));
        jwk.insert(QStringLiteral("x"), QString::fromLatin1(base64(point.mid(1, 32))));
        jwk.insert(QStringLiteral("y"), QString::fromLatin1(base64(point.mid(33, 32))));
    }
    const QJsonObject jwks{{QStringLiteral("keys"), QJsonArray{jwk}}};
    const auto now = QDateTime::currentSecsSinceEpoch();
    const QJsonObject claims{{QStringLiteral("iss"), QStringLiteral("https://auth.openai.com")}, {QStringLiteral("sub"), QStringLiteral("test-user")},
                            {QStringLiteral("aud"), QStringLiteral("oaiapp_test")}, {QStringLiteral("nonce"), QStringLiteral("test-nonce")},
                            {QStringLiteral("iat"), now}, {QStringLiteral("exp"), now + 300}};
    const auto token = [&](QJsonObject value) {
        const QJsonObject header{{QStringLiteral("alg"), rsa ? QStringLiteral("RS256") : QStringLiteral("ES256")}, {QStringLiteral("kid"), QStringLiteral("test-key")}};
        const QByteArray data = base64(QJsonDocument(header).toJson(QJsonDocument::Compact)) + '.' + base64(QJsonDocument(value).toJson(QJsonDocument::Compact));
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) return QString();
        size_t size = 0;
        if (EVP_DigestSign(context.get(), nullptr, &size, reinterpret_cast<const unsigned char *>(data.constData()), data.size()) != 1) return QString();
        QByteArray signature(size, '\0');
        if (EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(signature.data()), &size, reinterpret_cast<const unsigned char *>(data.constData()), data.size()) != 1) return QString();
        signature.resize(size);
        if (!rsa) {
            const auto *pointer = reinterpret_cast<const unsigned char *>(signature.constData());
            std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> parsed(d2i_ECDSA_SIG(nullptr, &pointer, signature.size()), ECDSA_SIG_free);
            if (!parsed) return QString();
            signature.resize(64);
            if (BN_bn2binpad(ECDSA_SIG_get0_r(parsed.get()), reinterpret_cast<unsigned char *>(signature.data()), 32) != 32
                || BN_bn2binpad(ECDSA_SIG_get0_s(parsed.get()), reinterpret_cast<unsigned char *>(signature.data() + 32), 32) != 32) return QString();
        }
        return QString::fromLatin1(data + '.' + base64(signature));
    };
    const auto jwt = token(claims);
    QVERIFY(!jwt.isEmpty());
    QString error;
    QCOMPARE(ChatGPTAuth::verifyIdToken(jwt, jwks, QStringLiteral("oaiapp_test"), QStringLiteral("test-nonce"), &error), claims);
    QVERIFY(error.isEmpty());
    QVERIFY(ChatGPTAuth::verifyIdToken(jwt, jwks, QStringLiteral("wrong-client"), QStringLiteral("test-nonce"), &error).isEmpty());
    QVERIFY(ChatGPTAuth::verifyIdToken(jwt, jwks, QStringLiteral("oaiapp_test"), QStringLiteral("wrong-nonce"), &error).isEmpty());
    auto expired = claims; expired.insert(QStringLiteral("exp"), now - 100);
    QVERIFY(ChatGPTAuth::verifyIdToken(token(expired), jwks, QStringLiteral("oaiapp_test"), QStringLiteral("test-nonce"), &error).isEmpty());
    auto wrongIssuer = claims; wrongIssuer.insert(QStringLiteral("iss"), QStringLiteral("https://evil.example"));
    QVERIFY(ChatGPTAuth::verifyIdToken(token(wrongIssuer), jwks, QStringLiteral("oaiapp_test"), QStringLiteral("test-nonce"), &error).isEmpty());
    auto tampered = jwt;
    const auto position = tampered.lastIndexOf(QLatin1Char('.')) + 2;
    tampered[position] = tampered.at(position) == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
    QVERIFY(ChatGPTAuth::verifyIdToken(tampered, jwks, QStringLiteral("oaiapp_test"), QStringLiteral("test-nonce"), &error).isEmpty());
}

void ChatGPTAuthTest::rejectUnsignedAndMissingGrant()
{
    QString error;
    QVERIFY(ChatGPTAuth::verifyIdToken(QStringLiteral("eyJhbGciOiJub25lIn0.e30."), {}, QStringLiteral("client"), {}, &error).isEmpty());
    QVERIFY(!ChatGPTAuth::grantError({{QStringLiteral("scope"), QStringLiteral("openid email")}}).isEmpty());
    const QJsonObject tokens{{QStringLiteral("scope"), QStringLiteral("openid resource.invoke chatgpt.tokens.use.direct")}, {QStringLiteral("access_token"), QStringLiteral("fake-access")},
                             {QStringLiteral("token_type"), QStringLiteral("Bearer")}, {QStringLiteral("expires_in"), 3600}};
    QVERIFY(ChatGPTAuth::grantError(tokens).isEmpty());
}

void ChatGPTAuthTest::browserCallbackAndPkce()
{
    QTemporaryDir directory;
    ChatGPTAuth auth(directory.path());
    BrowserCapture browser;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "capture");
    auth.m_busy = true;
    auth.m_store.insert(QStringLiteral("host"), QStringLiteral("urn:uuid:test-host"));
    auth.m_discovery.insert(QStringLiteral("authorization_endpoint"), QStringLiteral("https://auth.openai.com/api/accounts/authorize"));
    auth.listen();
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    QVERIFY(!browser.url.isEmpty());
    const QUrlQuery query(browser.url);
    QCOMPARE(query.queryItemValue(QStringLiteral("client_id")), QStringLiteral("dynamic_agent_client"));
    QCOMPARE(query.queryItemValue(QStringLiteral("agent_name_hint")), QStringLiteral("OkulAIr"));
    QVERIFY(query.queryItemValue(QStringLiteral("scope")).contains(QLatin1String("chatgpt.tokens.use.direct")));
    QCOMPARE(query.queryItemValue(QStringLiteral("code_challenge")), QString::fromLatin1(base64(QCryptographicHash::hash(auth.m_verifier.toLatin1(), QCryptographicHash::Sha256))));
    const QUrl redirect(query.queryItemValue(QStringLiteral("redirect_uri")));
    QCOMPARE(redirect.host(), QStringLiteral("127.0.0.1"));
    QSignalSpy errors(&auth, &ChatGPTAuth::error);
    const auto send = [&](const QByteArray &parameters) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, redirect.port());
        QTest::qWait(10);
        socket.write("GET /auth/callback?" + parameters + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        socket.flush();
        QTest::qWait(20);
    };
    send(QByteArray("state=incorrect&code=do-not-redeem"));
    QCOMPARE(errors.size(), 0);
    QVERIFY(auth.m_listener.isListening());
    send(QByteArray("state=") + auth.m_state.toLatin1() + "&error=access_denied");
    QCOMPARE(errors.size(), 1);
    QVERIFY(!auth.m_listener.isListening());
    QVERIFY(auth.m_state.isEmpty());
}

void ChatGPTAuthTest::privateStorageAndSignOut()
{
    QTemporaryDir directory;
    ChatGPTAuth auth(directory.filePath(QStringLiteral("credentials")));
    QVERIFY(auth.lock());
    auth.m_store.insert(QStringLiteral("host"), QStringLiteral("urn:uuid:stable"));
    auth.replaceAccount({{QStringLiteral("client_id"), QStringLiteral("oaiapp_test")}, {QStringLiteral("subject"), QStringLiteral("user")},
                         {QStringLiteral("access_token"), QStringLiteral("fake-access")}, {QStringLiteral("refresh_token"), QStringLiteral("fake-refresh")},
                         {QStringLiteral("expires_at"), QDateTime::currentSecsSinceEpoch() + 3600}});
    QVERIFY(auth.write());
    auth.m_lock.reset();
    const QFileInfo file(auth.m_directory + QStringLiteral("/accounts.json"));
    QVERIFY(!(file.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
    ChatGPTAuth restored(auth.m_directory);
    QCOMPARE(restored.accessToken(), QStringLiteral("fake-access"));
    restored.signOut(); // No network: discovery is intentionally absent.
    QVERIFY(restored.accessToken().isEmpty());
    QCOMPARE(restored.activeClient(), QStringLiteral("oaiapp_test"));
    QCOMPARE(restored.m_store.value(QStringLiteral("host")).toString(), QStringLiteral("urn:uuid:stable"));
    QVERIFY(restored.account().value(QStringLiteral("refresh_token")).isUndefined());
}

void ChatGPTAuthTest::boundedResponses_data()
{
    QTest::addColumn<int>("paddingBytes");
    QTest::addColumn<bool>("catalog");
    QTest::addColumn<bool>("validJson");
    QTest::addColumn<QString>("expectedError");
    // The live account catalog was 698184 bytes: larger than the old auth limit.
    QTest::newRow("large-model-catalog") << 700000 << true << true << QString();
    QTest::newRow("bounded-model-catalog") << 9 * 1024 * 1024 << true << true << QStringLiteral("download limit");
    QTest::newRow("auth-limit-preserved") << 700000 << false << true << QStringLiteral("download limit");
    QTest::newRow("invalid-json") << 100 << true << false << QStringLiteral("invalid JSON");
}
void ChatGPTAuthTest::boundedResponses()
{
    QFETCH(int, paddingBytes);
    QFETCH(bool, catalog);
    QFETCH(bool, validJson);
    QFETCH(QString, expectedError);
    QTemporaryDir directory;
    ChatGPTAuth auth(directory.path());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto payload = validJson ? QJsonDocument(QJsonObject{
        {QStringLiteral("models"), QJsonArray{QJsonObject{{QStringLiteral("slug"), QStringLiteral("fixture-model")}, {QStringLiteral("visibility"), QStringLiteral("list")}}}},
        {QStringLiteral("metadata"), QString(paddingBytes, QLatin1Char('x'))}}).toJson(QJsonDocument::Compact) : QByteArray("not-json");
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [socket, payload] {
            socket->readAll();
            if (socket->property("sent").toBool()) return;
            socket->setProperty("sent", true);
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
            socket->disconnectFromHost();
        });
    });
    bool done = false;
    QString failure;
    QJsonObject received;
    int responseStatus = 0;
    const auto callback = [&](const QJsonObject &value, int status, const QString &error) {
        received = value; responseStatus = status; failure = error; done = true;
    };
    const auto url = QUrl(QStringLiteral("http://127.0.0.1:%1/models").arg(server.serverPort()));
    if (catalog) auth.request(url, {}, {}, callback, ChatGPTAuth::ModelCatalogLimit);
    else auth.request(url, {}, {}, callback);
    QTRY_VERIFY(done);
    QCOMPARE(responseStatus, 200);
    if (expectedError.isEmpty()) {
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(received.value(QStringLiteral("models")).toArray().size(), 1);
    } else {
        QVERIFY2(failure.contains(expectedError), qPrintable(failure));
        QVERIFY(received.isEmpty());
    }
}
void ChatGPTAuthTest::transportFailure()
{
    QTemporaryDir directory;
    ChatGPTAuth auth(directory.path());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto port = server.serverPort();
    server.close();
    bool done = false;
    QString failure;
    auth.request(QUrl(QStringLiteral("http://127.0.0.1:%1/models").arg(port)), {}, {}, [&](const QJsonObject &, int status, const QString &error) {
        QCOMPARE(status, 0);
        failure = error;
        done = true;
    });
    QTRY_VERIFY(done);
    QVERIFY(failure.contains(QLatin1String("network connection failed")));
    QVERIFY(!failure.contains(QLatin1String("plan")));
}

void ChatGPTAuthTest::visionRequestFormats()
{
    const QByteArray image("fixture-png");
    const QJsonArray history{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("Explain this figure")}}};
    for (const auto protocol : {Protocol::ChatGPT, Protocol::OpenAI, Protocol::Anthropic, Protocol::Compatible}) {
        Connection connection{protocol, QUrl(QStringLiteral("https://api.openai.com/v1")), QStringLiteral("vision-model"), QStringLiteral("fake")};
        const auto body = Provider::requestBody(connection, QStringLiteral("Visible evidence"), history, image);
        const auto messages = body.value(protocol == Protocol::ChatGPT || protocol == Protocol::OpenAI ? QStringLiteral("input") : QStringLiteral("messages")).toArray();
        const auto content = messages.last().toObject().value(QStringLiteral("content")).toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content.first().toObject().value(QStringLiteral("text")).toString(), QStringLiteral("Explain this figure"));
        auto withAttachment = history;
        auto attached = withAttachment.first().toObject();
        attached.insert(QStringLiteral("images"), QJsonArray{QString::fromLatin1(QByteArray("attachment-png").toBase64())});
        withAttachment[0] = attached;
        const auto multi = Provider::requestBody(connection, QStringLiteral("Evidence"), withAttachment, image);
        const auto multiMessages = multi.value(protocol == Protocol::ChatGPT || protocol == Protocol::OpenAI ? QStringLiteral("input") : QStringLiteral("messages")).toArray();
        QCOMPARE(multiMessages.last().toObject().value(QStringLiteral("content")).toArray().size(), 3);
        QVERIFY(!multiMessages.last().toObject().contains(QStringLiteral("images")));
        const auto block = content.last().toObject();
        if (protocol == Protocol::Anthropic) {
            QCOMPARE(block.value(QStringLiteral("type")), QJsonValue(QStringLiteral("image")));
            QCOMPARE(block.value(QStringLiteral("source")).toObject().value(QStringLiteral("data")).toString().toLatin1(), image.toBase64());
        } else if (protocol == Protocol::Compatible) {
            QCOMPARE(block.value(QStringLiteral("type")), QJsonValue(QStringLiteral("image_url")));
            QVERIFY(block.value(QStringLiteral("image_url")).toObject().value(QStringLiteral("url")).toString().endsWith(QString::fromLatin1(image.toBase64())));
        } else {
            QCOMPARE(block.value(QStringLiteral("type")), QJsonValue(QStringLiteral("input_image")));
            QVERIFY(block.value(QStringLiteral("image_url")).toString().endsWith(QString::fromLatin1(image.toBase64())));
        }
    }
}

void ChatGPTAuthTest::subscriptionRequestAndPresets()
{
    Connection connection{Protocol::ChatGPT, QUrl(QStringLiteral("https://api.openai.com/v1")), QStringLiteral("account-model"), QStringLiteral("fake-oauth-token")};
    QVERIFY(Provider::validate(connection).isEmpty());
    const auto body = Provider::requestBody(connection, QStringLiteral("Evidence"), {});
    QVERIFY(body.value(QStringLiteral("stream")).toBool());
    QVERIFY(!body.value(QStringLiteral("store")).toBool(true));
    QVERIFY(!body.contains(QStringLiteral("max_output_tokens")));
    QVERIFY(!body.contains(QStringLiteral("previous_response_id")));
    connection.endpoint = QUrl(QStringLiteral("https://other.example/v1"));
    QVERIFY(!Provider::validate(connection).isEmpty());
    for (const auto &preset : servicePresets()) {
        connection = {preset.protocol, QUrl(preset.baseUrl), QStringLiteral("test"), QStringLiteral("fake"), preset.requiresKey};
        const auto error = Provider::validate(connection);
        if (Provider::accountClient(preset.protocol)) {
            QVERIFY(!preset.requiresKey);
            QVERIFY2(error.isEmpty() || error.contains(QStringLiteral("official CLI")),qPrintable(error));
        } else QVERIFY2(error.isEmpty(), qPrintable(preset.name));
        if (preset.requiresKey) { connection.key.clear(); QVERIFY(!Provider::validate(connection).isEmpty()); }
    }
    EventStream usage(Protocol::ChatGPT);
    usage.feed("data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"subscription_sharing_usage_limit_exceeded\"}}}\n\n");
    QVERIFY(usage.error.contains(QLatin1String("plan usage")));
    QVERIFY(!usage.complete);
}
}
QTEST_MAIN(OkulAIr::ChatGPTAuthTest)
#include "chatgptauthtest.moc"
