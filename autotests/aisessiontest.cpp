/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "part/ai/provider.h"
#include "part/ai/session.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>

using namespace OkulAIr;
class AiSessionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void roundTrip();
    void workspaceRoundTrip();
    void pruneUnreferencedScreenshots();
    void rejectDamagedOrFutureSessions();
    void eventFraming_data();
    void eventFraming();
    void streamErrors();
    void endpointValidation();
    void localHttpRoundTrip();
    void cancelledRequestCannotDeliver();
    void officialAccountClients();
    void thinkingEffort();
};

void AiSessionTest::pruneUnreferencedScreenshots()
{
    Session session; session.ensureWorkspaces();
    auto tab = session.workspace(QStringLiteral("main"));
    tab.insert(QStringLiteral("images"), QJsonObject{{QStringLiteral("sent"), QStringLiteral("sent-image")}, {QStringLiteral("selected"), QStringLiteral("selected-image")}, {QStringLiteral("unused"), QStringLiteral("unused-image")}});
    tab.insert(QStringLiteral("selectionContext"), QJsonObject{{QStringLiteral("imageRef"), QStringLiteral("selected")}});
    session.setWorkspace(tab);
    session.append(QStringLiteral("user"), QStringLiteral("Explain screenshot"));
    auto message = session.messages.first().toObject();
    message.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("imageRef"), QStringLiteral("sent")}}); session.messages[0] = message;
    session.pruneImages();
    auto images = session.workspace(QStringLiteral("main")).value(QStringLiteral("images")).toObject();
    QCOMPARE(images.size(), 2); QVERIFY(images.contains(QStringLiteral("sent"))); QVERIFY(images.contains(QStringLiteral("selected")));
    session.messages = {}; tab = session.workspace(QStringLiteral("main")); tab.remove(QStringLiteral("selectionContext")); session.setWorkspace(tab);
    session.pruneImages(); QVERIFY(!session.workspace(QStringLiteral("main")).contains(QStringLiteral("images")));
}

void AiSessionTest::roundTrip()
{
    Session session;
    session.append(QStringLiteral("user"), QString::fromUtf8("Τι σημαίνει αυτό; <script>\n😀"));
    session.append(QStringLiteral("assistant"), QStringLiteral("An answer [p. 2]"), QStringLiteral("test-model"), QStringLiteral("summary"), QStringLiteral("interrupted"));
    Session decoded;
    QString error;
    QVERIFY2(Session::decode(session.encode(), &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.messages, session.messages);
}

void AiSessionTest::workspaceRoundTrip()
{
    Session session; session.append(QStringLiteral("user"), QStringLiteral("Legacy question"));
    Session decoded; QString error;
    QVERIFY(Session::decode(session.encode(), &decoded, &error)); decoded.ensureWorkspaces();
    QCOMPARE(decoded.workspaces.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("main"));
    decoded.workspaces.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("notes")}, {QStringLiteral("type"), QStringLiteral("scratchpad")}, {QStringLiteral("title"), QStringLiteral("Working notes")}, {QStringLiteral("notes"), QStringLiteral("My equation")}, {QStringLiteral("closed"), true}});
    decoded.activeWorkspace = QStringLiteral("main");
    Session restored; QVERIFY(Session::decode(decoded.encode(), &restored, &error)); QCOMPARE(restored.workspaces, decoded.workspaces); QCOMPARE(restored.messages, decoded.messages);
    const auto previous = restored.encode();
    decoded.workspaces.append(decoded.workspaces.last());
    QVERIFY(!Session::decode(decoded.encode(), &restored, &error)); QCOMPARE(restored.encode(), previous);
}

void AiSessionTest::rejectDamagedOrFutureSessions()
{
    Session previous;
    previous.append(QStringLiteral("user"), QStringLiteral("keep me"));
    const auto original = previous.messages;
    QString error;
    for (const auto &input : QStringList{QStringLiteral("not a session"), Session::marker() + QStringLiteral("{"),
                             Session::marker() + QStringLiteral("{\"format\":\"org.okulair.session\",\"version\":2,\"messages\":[]}"),
                             Session::marker() + QStringLiteral("{\"format\":\"org.okulair.session\",\"version\":1,\"messages\":[{\"role\":\"system\",\"content\":\"unsafe\",\"status\":\"complete\"}]}"),
                             Session::marker() + QString(Session::MaximumBytes, QLatin1Char('x'))}) {
        QVERIFY(!Session::decode(input, &previous, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(previous.messages, original);
    }
}

void AiSessionTest::eventFraming_data()
{
    QTest::addColumn<int>("protocol");
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("compatible") << int(Protocol::Compatible)
        << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\r\n\r\ndata: [DONE]\r\n\r\n");
    QTest::newRow("responses") << int(Protocol::OpenAI)
        << QByteArray(": keepalive\n\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\ndata: {\"type\":\"response.completed\"}\n\n");
    QTest::newRow("anthropic") << int(Protocol::Anthropic)
        << QByteArray("event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\ndata: {\"type\":\"message_stop\"}\n\n");
}

void AiSessionTest::eventFraming()
{
    QFETCH(int, protocol);
    QFETCH(QByteArray, bytes);
    // Exercise every possible network split, including inside CRLF and UTF-8.
    for (qsizetype split = 0; split <= bytes.size(); ++split) {
        EventStream stream(static_cast<Protocol>(protocol));
        stream.feed(bytes.left(split));
        stream.feed(bytes.mid(split));
        QCOMPARE(stream.text, QStringLiteral("Hello"));
        QVERIFY(stream.complete);
        QVERIFY2(stream.error.isEmpty(), qPrintable(stream.error));
    }
    EventStream unicode(Protocol::OpenAI);
    const auto utf8 = QString::fromUtf8("data: {\"type\":\"response.output_text.delta\",\"delta\":\"Γειά 😀\"}\n\n").toUtf8();
    for (const auto byte : utf8) {
        unicode.feed(QByteArray(1, byte));
    }
    QCOMPARE(unicode.text, QString::fromUtf8("Γειά 😀"));
}

void AiSessionTest::streamErrors()
{
    for (const QByteArray &data : {QByteArray("data: invalid\n\n"), QByteArray("data: {\"type\":\"error\"}\n\n"), QByteArray("data: {\"type\":\"response.incomplete\"}\n\n")}) {
        EventStream stream(Protocol::OpenAI);
        stream.feed(data);
        QVERIFY(!stream.error.isEmpty());
        QVERIFY(!stream.complete);
    }
    EventStream large(Protocol::Compatible);
    large.feed(QByteArray(2 * 1024 * 1024 + 1, 'a'));
    QVERIFY(!large.error.isEmpty());
}

void AiSessionTest::endpointValidation()
{
    Connection connection{Protocol::Compatible, QUrl(QStringLiteral("http://localhost:11434/v1")), QStringLiteral("local-model"), {}};
    QVERIFY(Provider::validate(connection).isEmpty());
    for (const auto &endpoint : {QStringLiteral("http://remote.example/v1"), QStringLiteral("https://user:secret@example.com/v1"), QStringLiteral("file:///tmp/model"), QStringLiteral("https://example.com/v1?key=secret")}) {
        connection.endpoint = QUrl(endpoint);
        QVERIFY(!Provider::validate(connection).isEmpty());
    }
    connection = {Protocol::OpenAI, QUrl(QStringLiteral("https://api.openai.com/v1")), QStringLiteral("test-model"), QStringLiteral("test-key")};
    const QJsonArray history{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("Question")}}};
    const auto body = Provider::requestBody(connection, QStringLiteral("Evidence"), history);
    QCOMPARE(body.value(QStringLiteral("input")).toArray(), history);
    QCOMPARE(body.value(QStringLiteral("store")).toBool(true), false);
    QVERIFY(!QJsonDocument(body).toJson().contains("test-key"));
}

void AiSessionTest::officialAccountClients()
{
#ifndef Q_OS_UNIX
    QSKIP("Executable stub uses the Unix Python launcher");
#else
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QByteArray stub = R"PY(#!/usr/bin/python3
import json, os, sys, time
from pathlib import Path
name=Path(sys.argv[0]).name
args=sys.argv[1:]
request=json.load(sys.stdin)
assert '@' not in json.dumps(request, ensure_ascii=False)
assert 'OPENAI_API_KEY' not in os.environ
settings=json.loads(Path(os.environ['GEMINI_CLI_SYSTEM_SETTINGS_PATH']).read_text())
if name=='gemini':
    assert '--extensions' in args and 'none' in args
    assert settings['tools']['core']==['okulair_no_tools']
    assert settings['mcp']['allowed']==['okulair_no_mcp']
    assert settings['security']['auth']['enforcedType']=='oauth-personal'
elif name=='copilot':
    assert '--available-tools=okulair_no_tools' in args
    assert '--disable-builtin-mcps' in args
else:
    assert '--safe-mode' in args
    assert args[args.index('--max-tool-calls')+1]=='0'
    assert settings['security']['auth']['selectedType']=='qwen-oauth'
model=args[args.index('--model')+1] if '--model' in args else 'auto'
if model!='auto':
    if name=='copilot': assert args[args.index('--reasoning-effort')+1]=='high'
    elif name=='qwen': assert settings['model']['reasoningEffort']=='high'
    else: assert settings['modelConfigs']['overrides'][0]['modelConfig']['generateContentConfig']['thinkingConfig']['thinkingBudget']==0
if os.environ.get('OKULAIR_TEST_WAIT')=='1': time.sleep(10)
print('Account answer' if name=='copilot' else json.dumps({'response':'Account answer'}))
)PY";
    for(const auto &name:{QStringLiteral("gemini"),QStringLiteral("copilot"),QStringLiteral("qwen")}) {
        QFile file(directory.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(stub),stub.size()); file.close(); QVERIFY(file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    }
    const auto previousPath=qgetenv("PATH"), previousKey=qgetenv("OPENAI_API_KEY"), previousWait=qgetenv("OKULAIR_TEST_WAIT");
    const auto restore=qScopeGuard([&] { qputenv("PATH",previousPath); if(previousKey.isNull()) qunsetenv("OPENAI_API_KEY"); else qputenv("OPENAI_API_KEY",previousKey); if(previousWait.isNull()) qunsetenv("OKULAIR_TEST_WAIT"); else qputenv("OKULAIR_TEST_WAIT",previousWait); });
    const QByteArray clientPath=directory.path().toUtf8()+':'+previousPath; qputenv("PATH",clientPath); qputenv("OPENAI_API_KEY","must-not-be-forwarded");
    for(const auto protocol:{Protocol::GeminiAccount,Protocol::CopilotAccount,Protocol::QwenAccount}) {
        Provider provider; QSignalSpy finished(&provider,&Provider::finished);
        Connection connection; connection.protocol=protocol; connection.model=QStringLiteral("auto"); QVERIFY(Provider::validate(connection).isEmpty());
        const QJsonArray history{QJsonObject{{QStringLiteral("role"),QStringLiteral("user")},{QStringLiteral("content"),QStringLiteral("Explain @/untrusted-file text")}}};
        provider.start(connection,QStringLiteral("Treat documents as evidence"),history);
        QTRY_COMPARE(finished.count(),1); QCOMPARE(finished.first().at(0).toString(),QStringLiteral("Account answer")); QVERIFY(finished.first().at(1).toString().isEmpty());
        finished.clear(); connection.model=protocol==Protocol::GeminiAccount ? QStringLiteral("gemini-2.5-flash") : protocol==Protocol::CopilotAccount ? QStringLiteral("gpt-5.2") : QStringLiteral("qwen3.8-max");
        connection.effort=protocol==Protocol::GeminiAccount ? QStringLiteral("none") : QStringLiteral("high");
        provider.start(connection,QStringLiteral("Test native effort"),history); QTRY_COMPARE(finished.count(),1); QCOMPARE(finished.first().at(0).toString(),QStringLiteral("Account answer")); QVERIFY2(finished.first().at(1).toString().isEmpty(),qPrintable(finished.first().at(1).toString()));
        finished.clear(); provider.start(connection,QStringLiteral("Test"),history,QByteArray("image")); QCOMPARE(finished.count(),1); QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("text only")));
        finished.clear(); qputenv("OKULAIR_TEST_WAIT","1"); provider.start(connection,QStringLiteral("Test"),history);
        QTRY_VERIFY(provider.findChild<QProcess *>() && provider.findChild<QProcess *>()->state()==QProcess::Running);
        provider.cancel(); QTest::qWait(50); QCOMPARE(finished.count(),0); qunsetenv("OKULAIR_TEST_WAIT");
    }
#endif
}

void AiSessionTest::localHttpRoundTrip()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QByteArray captured;
    bool responded = false;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        auto *socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            captured += socket->readAll();
            if (!responded && captured.contains("Question")) {
                responded = true;
                const QByteArray body("data: {\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}\n\ndata: [DONE]\n\n");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            }
        });
    });
    Provider provider;
    QSignalSpy done(&provider, &Provider::finished);
    const Connection connection{Protocol::Compatible, QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort())), QStringLiteral("test-model"), QStringLiteral("test-key")};
    provider.start(connection, QStringLiteral("Evidence"), {QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("Question")}}});
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done.first().at(0).toString(), QStringLiteral("Answer"));
    QVERIFY(done.first().at(1).toString().isEmpty());
    QVERIFY(captured.startsWith("POST /v1/chat/completions "));
    QVERIFY(captured.contains("Bearer test-key"));
    QVERIFY(captured.contains("Evidence"));
}

void AiSessionTest::cancelledRequestCannotDeliver()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    Provider provider;
    QSignalSpy done(&provider, &Provider::finished);
    provider.start({Protocol::Compatible, QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort())), QStringLiteral("test"), {}}, {}, {});
    provider.cancel();
    QTest::qWait(50);
    QCOMPARE(done.size(), 0);
}
QTEST_GUILESS_MAIN(AiSessionTest)
#include "aisessiontest.moc"

void AiSessionTest::thinkingEffort()
{
    Connection c{Protocol::OpenAI,QUrl(QStringLiteral("https://api.openai.com/v1")),QStringLiteral("gpt-6.1-sol"),QStringLiteral("fake-key")};
    QCOMPARE(Provider::defaultEffort(c), QStringLiteral("medium"));
    auto account = c; account.protocol = Protocol::ChatGPT;
    QVERIFY(Provider::defaultEffort(account).isEmpty()); // Account routing may differ from the API.
    account.modelMetadata.insert(QStringLiteral("default_reasoning_level"), QStringLiteral("high"));
    QCOMPARE(Provider::defaultEffort(account), QStringLiteral("high"));
    account.modelMetadata.insert(QStringLiteral("default_reasoning_level"), QStringLiteral("invalid"));
    QVERIFY(Provider::defaultEffort(account).isEmpty());
    const QJsonArray messages{QJsonObject{{QStringLiteral("role"),QStringLiteral("user")},{QStringLiteral("content"),QStringLiteral("Explain")}}};
    QVERIFY(!Provider::effortOptions(c).contains(QStringLiteral("none"))); QVERIFY(Provider::effortOptions(c).contains(QStringLiteral("medium")));
    QVERIFY(!Provider::requestBody(c,{},messages).contains(QStringLiteral("reasoning")));
    c.effort=QStringLiteral("high"); QCOMPARE(Provider::requestBody(c,{},messages).value(QStringLiteral("reasoning")).toObject().value(QStringLiteral("effort")).toString(),QStringLiteral("high"));
    c.effort=QStringLiteral("none"); QVERIFY(!Provider::validate(c).isEmpty());
    c.model=QStringLiteral("gpt-5.2"); QVERIFY(Provider::validate(c).isEmpty());
    c.modelMetadata=QJsonObject{{QStringLiteral("supported_reasoning_levels"),QJsonArray{QJsonObject{{QStringLiteral("effort"),QStringLiteral("medium")}}}}};
    QCOMPARE(Provider::effortOptions(c),QStringList{QStringLiteral("medium")});
    c.modelMetadata=QJsonObject{{QStringLiteral("supported_reasoning_efforts"),QJsonArray{}}}; QVERIFY(Provider::effortOptions(c).isEmpty());
    c.modelMetadata={}; c.protocol=Protocol::Anthropic; c.endpoint=QUrl(QStringLiteral("https://api.anthropic.com/v1")); c.model=QStringLiteral("claude-opus-4-6"); c.effort=QStringLiteral("high");
    auto body=Provider::requestBody(c,{},messages); QCOMPARE(body.value(QStringLiteral("output_config")).toObject().value(QStringLiteral("effort")).toString(),QStringLiteral("high")); QCOMPARE(body.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),QStringLiteral("adaptive")); QVERIFY(!Provider::effortOptions(c).contains(QStringLiteral("xhigh")));
    c.model=QStringLiteral("claude-sonnet-4-5"); body=Provider::requestBody(c,{},messages); const auto budget=body.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("budget_tokens")).toInt(); QVERIFY(budget>=1024); QVERIFY(body.value(QStringLiteral("max_tokens")).toInt()>budget);
    c.effort=QStringLiteral("none"); QCOMPARE(Provider::requestBody(c,{},messages).value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),QStringLiteral("disabled"));
    c.protocol=Protocol::Compatible; c.endpoint=QUrl(QStringLiteral("https://api.deepseek.com")); c.model=QStringLiteral("deepseek-flash");
    body=Provider::requestBody(c,{},messages); QCOMPARE(body.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),QStringLiteral("disabled")); QVERIFY(!body.contains(QStringLiteral("reasoning_effort"))); QVERIFY(!Provider::effortOptions(c).contains(QStringLiteral("medium")));
    c.effort=QStringLiteral("high"); QCOMPARE(Provider::requestBody(c,{},messages).value(QStringLiteral("reasoning_effort")).toString(),QStringLiteral("high"));
    c.endpoint=QUrl(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/openai")); c.model=QStringLiteral("gemini-2.5-pro"); QVERIFY(!Provider::effortOptions(c).contains(QStringLiteral("none")));
    c.model=QStringLiteral("gemini-2.5-flash"); QVERIFY(Provider::effortOptions(c).contains(QStringLiteral("none"))); QCOMPARE(Provider::requestBody(c,{},messages).value(QStringLiteral("reasoning_effort")).toString(),QStringLiteral("high"));
    c.endpoint=QUrl(QStringLiteral("https://api.mistral.ai/v1")); c.model=QStringLiteral("mistral-small-2603"); QCOMPARE(Provider::effortOptions(c),(QStringList{QStringLiteral("none"),QStringLiteral("high")}));
    c.model=QStringLiteral("mistral-large-4"); QVERIFY(Provider::effortOptions(c).contains(QStringLiteral("medium"))); QVERIFY(!Provider::effortOptions(c).contains(QStringLiteral("none")));
    c.endpoint=QUrl(QStringLiteral("https://api.groq.com/openai/v1")); c.model=QStringLiteral("openai/gpt-oss-120b"); QVERIFY(Provider::effortOptions(c).contains(QStringLiteral("medium"))); QVERIFY(!Provider::effortOptions(c).contains(QStringLiteral("none")));
    c.endpoint=QUrl(QStringLiteral("https://api.x.ai/v1")); c.model=QStringLiteral("grok-4.7"); QVERIFY(Provider::effortOptions(c).contains(QStringLiteral("xhigh")));
    c.endpoint=QUrl(QStringLiteral("https://openrouter.ai/api/v1")); c.model=QStringLiteral("openai/gpt-5.2"); c.modelMetadata=QJsonObject{{QStringLiteral("supported_parameters"),QJsonArray{QStringLiteral("reasoning")}}}; QCOMPARE(Provider::requestBody(c,{},messages).value(QStringLiteral("reasoning")).toObject().value(QStringLiteral("effort")).toString(),QStringLiteral("high"));
    c.endpoint=QUrl(QStringLiteral("http://localhost:11434/v1")); c.model=QStringLiteral("unknown-model"); c.modelMetadata={}; QVERIFY(Provider::effortOptions(c).isEmpty());
    c.modelMetadata=QJsonObject{{QStringLiteral("supported_reasoning_efforts"),QJsonArray{QStringLiteral("low"),QStringLiteral("high")}}}; QCOMPARE(Provider::requestBody(c,{},messages).value(QStringLiteral("reasoning_effort")).toString(),QStringLiteral("high"));
    EventStream stream(Protocol::Compatible); stream.feed("data: {\"choices\":[{\"delta\":{\"content\":[{\"type\":\"thinking\",\"thinking\":[{\"text\":\"private\"}]},{\"type\":\"text\",\"text\":\"Visible answer\"}]}}]}\n\ndata: [DONE]\n\n"); QCOMPARE(stream.text,QStringLiteral("Visible answer")); QVERIFY(stream.complete);
}
