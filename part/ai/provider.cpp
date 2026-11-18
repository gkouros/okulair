/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "provider.h"
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
#include <QTimer>
#include <QProcessEnvironment>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <memory>

using namespace OkulAIr;

void EventStream::feed(const QByteArray &bytes)
{
    m_bytes += bytes.size();
    if (m_bytes > 2 * 1024 * 1024) {
        error = QStringLiteral("The provider response exceeded the size limit.");
        return;
    }
    m_buffer += bytes;
    // Normalize only complete lines, so a split CRLF is handled correctly.
    while (true) {
        auto end = m_buffer.indexOf("\n\n");
        auto crlf = m_buffer.indexOf("\r\n\r\n");
        int separator = 2;
        if (crlf >= 0 && (end < 0 || crlf < end)) {
            end = crlf;
            separator = 4;
        }
        if (end < 0) {
            break;
        }
        const auto block = m_buffer.left(end);
        m_buffer.remove(0, end + separator);
        QByteArray data;
        for (auto line : block.split('\n')) {
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (line.startsWith("data:")) {
                line.remove(0, 5);
                if (line.startsWith(' ')) {
                    line.remove(0, 1);
                }
                if (!data.isEmpty()) {
                    data += '\n';
                }
                data += line;
            }
        }
        if (!data.isEmpty() && error.isEmpty() && !complete) {
            event(data);
        }
    }
}

void EventStream::event(const QByteArray &data)
{
    if (data == "[DONE]") {
        if (m_protocol == Protocol::Compatible) {
            complete = true;
        }
        return;
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QStringLiteral("The provider sent an invalid streaming event.");
        return;
    }
    const auto obj = doc.object();
    const auto type = obj.value(QStringLiteral("type")).toString();
    if (obj.contains(QStringLiteral("error")) || type == QLatin1String("error") || type == QLatin1String("response.failed") || type == QLatin1String("response.incomplete")) {
        const auto failure = obj.value(QStringLiteral("response")).toObject().value(QStringLiteral("error")).toObject();
        const QString code = failure.value(QStringLiteral("code")).toString(obj.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(obj.value(QStringLiteral("code")).toString()));
        if (code.startsWith(QLatin1String("subscription_sharing_usage"))) {
            error = QStringLiteral("ChatGPT plan usage is unavailable or its limit has been reached. Review ChatGPT Settings → Usage and try again when allowance is available. No API-key fallback was used.");
        } else if (code.startsWith(QLatin1String("subscription_sharing")) || code.startsWith(QLatin1String("chatpass_"))) {
            error = QStringLiteral("ChatGPT plan access could not be authorized. Reconnect your account or review its app permissions. No API-key fallback was used.");
        } else {
            error = QStringLiteral("The provider could not complete this response. Check the model, account limits, and request size.");
        }
    } else if ((m_protocol == Protocol::OpenAI || m_protocol == Protocol::ChatGPT)) {
        if (type == QLatin1String("response.output_text.delta")) {
            text += obj.value(QStringLiteral("delta")).toString();
        } else if (type == QLatin1String("response.completed")) {
            complete = true;
        }
    } else if (m_protocol == Protocol::Anthropic) {
        const auto delta = obj.value(QStringLiteral("delta")).toObject();
        if (type == QLatin1String("content_block_delta") && delta.value(QStringLiteral("type")) == QLatin1String("text_delta")) {
            text += delta.value(QStringLiteral("text")).toString();
        } else if (type == QLatin1String("message_delta") && delta.value(QStringLiteral("stop_reason")) == QLatin1String("max_tokens")) {
            error = QStringLiteral("The response reached its output limit.");
        } else if (type == QLatin1String("message_stop")) {
            complete = true;
        }
    } else {
        const auto choices = obj.value(QStringLiteral("choices")).toArray();
        if (!choices.isEmpty()) {
            const auto choice = choices.first().toObject();
            text += choice.value(QStringLiteral("delta")).toObject().value(QStringLiteral("content")).toString();
            const auto reason = choice.value(QStringLiteral("finish_reason")).toString();
            if (reason == QLatin1String("length") || reason == QLatin1String("content_filter")) {
                error = QStringLiteral("The response was truncated or filtered.");
            }
        }
    }
}

Provider::Provider(QObject *parent) : QObject(parent), m_network(this) {}
Provider::~Provider() { cancel(); }

bool Provider::accountClient(Protocol protocol) { return protocol == Protocol::GeminiAccount || protocol == Protocol::CopilotAccount || protocol == Protocol::QwenAccount; }
QString Provider::accountExecutable(Protocol protocol) { return protocol == Protocol::GeminiAccount ? QStringLiteral("gemini") : protocol == Protocol::CopilotAccount ? QStringLiteral("copilot") : QStringLiteral("qwen"); }
QString Provider::validate(const Connection &connection)
{
    if (accountClient(connection.protocol)) {
        return QStandardPaths::findExecutable(accountExecutable(connection.protocol)).isEmpty() ? QStringLiteral("Install the selected provider's official CLI, then use Sign in using official client. No API key is required.") : QString();
    }
    const auto url = connection.endpoint;
    const auto host = url.host().toLower();
    const bool loopback = host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") || host == QLatin1String("::1");
    if (!url.isValid() || host.isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || (url.scheme() != QLatin1String("https") && !(url.scheme() == QLatin1String("http") && loopback))) {
        return QStringLiteral("Use an HTTPS endpoint, or HTTP on localhost. Do not include credentials, query parameters, or fragments in the URL.");
    }
    if (connection.protocol == Protocol::ChatGPT && url != QUrl(QStringLiteral("https://api.openai.com/v1"))) {
        return QStringLiteral("ChatGPT subscription access is restricted to the official OpenAI endpoint.");
    }
    if (connection.model.trimmed().isEmpty()) {
        return QStringLiteral("Enter a model ID available from your provider.");
    }
    if ((connection.protocol != Protocol::Compatible || connection.requiresKey) && connection.key.trimmed().isEmpty()) {
        return connection.protocol == Protocol::ChatGPT
            ? QStringLiteral("No ChatGPT account is connected. Use Continue with ChatGPT to sign in.")
            : QStringLiteral("No service is connected. Enter the API key for the selected service (%1), then click Connect.").arg(connection.endpoint.host());
    }
    return {};
}

QJsonObject Provider::requestBody(const Connection &connection, const QString &instructions, const QJsonArray &history, const QByteArray &image)
{
    auto input = history;
    for (qsizetype i = 0; i < input.size(); ++i) {
        auto message = input.at(i).toObject();
        auto images = message.take(QStringLiteral("images")).toArray();
        if (i + 1 == input.size() && !image.isEmpty()) images.append(QString::fromLatin1(image.toBase64()));
        if (!images.isEmpty() && message.value(QStringLiteral("role")) == QLatin1String("user")) {
            const bool responses = connection.protocol == Protocol::OpenAI || connection.protocol == Protocol::ChatGPT;
            QJsonArray content{QJsonObject{{QStringLiteral("type"), responses ? QStringLiteral("input_text") : QStringLiteral("text")}, {QStringLiteral("text"), message.value(QStringLiteral("content"))}}};
            for (const auto &entry : images) {
                const auto data = entry.toString();
                const QString imageUrl = QStringLiteral("data:image/png;base64,") + data;
                if (responses) content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("input_image")}, {QStringLiteral("image_url"), imageUrl}});
                else if (connection.protocol == Protocol::Anthropic) content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")}, {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")}, {QStringLiteral("media_type"), QStringLiteral("image/png")}, {QStringLiteral("data"), data}}}});
                else content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")}, {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), imageUrl}}}});
            }
            message.insert(QStringLiteral("content"), content);
        }
        input[i] = message;
    }
    QJsonObject body{{QStringLiteral("model"), connection.model}, {QStringLiteral("stream"), true}};
    if ((connection.protocol == Protocol::OpenAI || connection.protocol == Protocol::ChatGPT)) {
        body.insert(QStringLiteral("instructions"), instructions);
        body.insert(QStringLiteral("input"), input);
        body.insert(QStringLiteral("store"), false);
    } else {
        auto messages = input;
        if (connection.protocol == Protocol::Anthropic) {
            body.insert(QStringLiteral("system"), instructions);
            body.insert(QStringLiteral("max_tokens"), 4096);
        } else {
            messages.prepend(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), instructions}});
        }
        body.insert(QStringLiteral("messages"), messages);
    }
    return body;
}

void Provider::start(const Connection &connection, const QString &instructions, const QJsonArray &history, const QByteArray &image)
{
    cancel();
    const auto invalid = validate(connection);
    if (!invalid.isEmpty()) {
        Q_EMIT finished({}, invalid);
        return;
    }
    if (accountClient(connection.protocol)) { startAccountClient(connection, instructions, history, image); return; }
    QUrl url = connection.endpoint;
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path + ((connection.protocol == Protocol::OpenAI || connection.protocol == Protocol::ChatGPT) ? QStringLiteral("/responses") : connection.protocol == Protocol::Anthropic ? QStringLiteral("/messages") : QStringLiteral("/chat/completions")));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(120000);
    if (!connection.key.isEmpty()) {
        request.setRawHeader(connection.protocol == Protocol::Anthropic ? "x-api-key" : "Authorization",
                             (connection.protocol == Protocol::Anthropic ? QByteArray() : QByteArray("Bearer ")) + connection.key.toUtf8());
    }
    if (connection.protocol == Protocol::Anthropic) {
        request.setRawHeader("anthropic-version", "2023-06-01");
    }
    auto *reply = m_network.post(request, QJsonDocument(requestBody(connection, instructions, history, image)).toJson(QJsonDocument::Compact));
    m_reply = reply;
    auto stream = std::make_shared<EventStream>(connection.protocol);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, stream] {
        if (m_reply != reply) {
            return;
        }
        stream->feed(reply->readAll());
        Q_EMIT updated(stream->text);
        if (!stream->error.isEmpty()) {
            reply->abort();
        }
    });
    bool sentImage = !image.isEmpty();
    for (const auto &entry : history) sentImage |= !entry.toObject().value(QStringLiteral("images")).toArray().isEmpty();
    connect(reply, &QNetworkReply::finished, this, [this, reply, stream, connection, sentImage] {
        reply->deleteLater();
        if (m_reply != reply) {
            return;
        }
        m_reply = nullptr;
        stream->feed(reply->readAll());
        auto error = stream->error;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300) {
            error = connection.protocol == Protocol::ChatGPT
                ? QStringLiteral("ChatGPT connection failed (HTTP %1). Reconnect your account or review ChatGPT Settings → Usage. No API-key billing was used.").arg(status)
                : QStringLiteral("Provider HTTP error %1. Check the selected service, credentials, model, and account limits.").arg(status);
            if (sentImage && (status == 400 || status == 422)) error = QStringLiteral("The model or service rejected this image request (HTTP %1). Choose a model that supports images, or use Selected text for text-only questions.").arg(status);
        } else if (error.isEmpty() && (reply->error() != QNetworkReply::NoError || !stream->complete)) {
            error = QStringLiteral("The connection ended before the response completed.");
        } else if (error.isEmpty() && stream->text.trimmed().isEmpty()) {
            error = QStringLiteral("The provider returned no answer text.");
        }
        Q_EMIT finished(stream->text, error);
    });
}

void Provider::cancel()
{
    if (m_process) {
        auto *process = m_process.data(); m_process = nullptr;
        process->disconnect(this); process->setParent(nullptr);
        if (process->state() == QProcess::NotRunning) process->deleteLater();
        else { connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),process,&QObject::deleteLater); process->kill(); }
    }
    if (m_reply) {
        auto *reply = m_reply.data();
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
}

void Provider::listModels(const Connection &connection)
{
    cancel();
    if (accountClient(connection.protocol)) { Q_EMIT modelsListed(QJsonArray{QJsonObject{{QStringLiteral("id"),QStringLiteral("auto")}}}, {}); return; }
    auto validation = connection;
    validation.model = QStringLiteral("catalogue-check");
    const auto error = validate(validation);
    if (!error.isEmpty()) { Q_EMIT modelsListed({}, error); return; }
    auto url = connection.endpoint;
    auto path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    url.setPath(path + QStringLiteral("/models"));
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!connection.key.isEmpty()) request.setRawHeader(connection.protocol == Protocol::Anthropic ? "x-api-key" : "Authorization", (connection.protocol == Protocol::Anthropic ? QByteArray() : QByteArray("Bearer ")) + connection.key.toUtf8());
    if (connection.protocol == Protocol::Anthropic) request.setRawHeader("anthropic-version", "2023-06-01");
    auto *reply = m_network.get(request);
    m_reply = reply;
    auto bytes = std::make_shared<QByteArray>();
    reply->setReadBufferSize(1024 * 1024);
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] { *bytes += reply->readAll(); if (bytes->size() > 8 * 1024 * 1024) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes] {
        reply->deleteLater();
        if (m_reply != reply) return;
        m_reply = nullptr;
        *bytes += reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto document = QJsonDocument::fromJson(*bytes);
        if (reply->error() != QNetworkReply::NoError || status != 200 || !document.object().value(QStringLiteral("data")).isArray() || bytes->size() > 8 * 1024 * 1024) {
            Q_EMIT modelsListed({}, QStringLiteral("Could not connect or load models (HTTP %1). Check this service's URL and credentials, or enter a model ID manually if its catalogue is unavailable.").arg(status)); return;
        }
        QJsonArray models;
        for (const auto &entry : document.object().value(QStringLiteral("data")).toArray()) {
            const auto object = entry.toObject();
            if (!object.value(QStringLiteral("id")).toString().isEmpty()) models.append(object);
        }
        Q_EMIT modelsListed(models, models.isEmpty() ? QStringLiteral("This service returned no available models.") : QString());
    });
}

void Provider::startAccountClient(const Connection &connection, const QString &instructions, const QJsonArray &history, const QByteArray &image)
{
    bool hasImages = !image.isEmpty();
    for (const auto &entry : history) hasImages |= !entry.toObject().value(QStringLiteral("images")).toArray().isEmpty();
    if (hasImages) { Q_EMIT finished({}, QStringLiteral("This official-client connection currently supports text only. Choose Document or a text Selection, or select an image-capable API/ChatGPT connection for screenshots.")); return; }
    auto directory = std::make_shared<QTemporaryDir>();
    if (!directory->isValid()) { Q_EMIT finished({}, QStringLiteral("Could not create the account client's private working folder.")); return; }
    const auto settingsPath = directory->filePath(QStringLiteral("settings.json"));
    QJsonObject settings{
        {QStringLiteral("security"), QJsonObject{{QStringLiteral("auth"), QJsonObject{{QStringLiteral("selectedType"), connection.protocol == Protocol::QwenAccount ? QStringLiteral("qwen-oauth") : QStringLiteral("oauth-personal")}, {QStringLiteral("enforcedType"), connection.protocol == Protocol::QwenAccount ? QStringLiteral("qwen-oauth") : QStringLiteral("oauth-personal")}}}}},
        {QStringLiteral("tools"), QJsonObject{{QStringLiteral("core"), QJsonArray{QStringLiteral("okulair_no_tools")}}}},
        {QStringLiteral("mcp"), QJsonObject{{QStringLiteral("allowed"), QJsonArray{QStringLiteral("okulair_no_mcp")}}}},
        {QStringLiteral("hooksConfig"), QJsonObject{{QStringLiteral("enabled"),false}}},
        {QStringLiteral("context"), QJsonObject{{QStringLiteral("fileName"),QStringLiteral("__okulair_no_context__.md")}}},
        {QStringLiteral("skills"), QJsonObject{{QStringLiteral("enabled"),false}}}};
    settings.insert(QStringLiteral("admin"), QJsonObject{{QStringLiteral("mcp"),QJsonObject{{QStringLiteral("enabled"),false}}},{QStringLiteral("extensions"),QJsonObject{{QStringLiteral("enabled"),false}}},{QStringLiteral("skills"),QJsonObject{{QStringLiteral("enabled"),false}}}});
    QFile file(settingsPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(settings).toJson()) < 0) { Q_EMIT finished({}, QStringLiteral("Could not configure the account client.")); return; }
    file.close();
    auto *process = new QProcess(this); m_process = process;
    process->setWorkingDirectory(directory->path());
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : {"OPENAI_API_KEY","ANTHROPIC_API_KEY","GEMINI_API_KEY","GOOGLE_API_KEY","GOOGLE_GENAI_USE_VERTEXAI","GITHUB_TOKEN","GH_TOKEN","COPILOT_GITHUB_TOKEN"}) environment.remove(QString::fromLatin1(key));
    environment.insert(QStringLiteral("GEMINI_CLI_SYSTEM_SETTINGS_PATH"),settingsPath);
    environment.insert(QStringLiteral("QWEN_CODE_SYSTEM_SETTINGS_PATH"),settingsPath);
    process->setProcessEnvironment(environment);
    QStringList arguments;
    if (connection.protocol == Protocol::CopilotAccount) {
        arguments = {QStringLiteral("--silent"),QStringLiteral("--available-tools=okulair_no_tools"),QStringLiteral("--disable-builtin-mcps"),QStringLiteral("--disable-mcp-server=computer-use"),QStringLiteral("--no-experimental"),QStringLiteral("--no-custom-instructions"),QStringLiteral("-p"),QStringLiteral("Answer the document question supplied on standard input. Do not use tools.")};
    } else {
        arguments = {QStringLiteral("--output-format"),QStringLiteral("json"),QStringLiteral("-p"),QStringLiteral("Answer the document question supplied on standard input. Do not use tools.")};
        if (connection.protocol == Protocol::GeminiAccount) arguments << QStringLiteral("--extensions") << QStringLiteral("none");
        else arguments << QStringLiteral("--safe-mode") << QStringLiteral("--max-tool-calls") << QStringLiteral("0") << QStringLiteral("--max-session-turns") << QStringLiteral("1") << QStringLiteral("--max-wall-time") << QStringLiteral("180s");
    }
    if (!connection.model.isEmpty() && connection.model != QLatin1String("auto")) arguments << QStringLiteral("--model") << connection.model;
    // No shell or document text in argv. Prevent CLI @file expansion in evidence.
    auto prompt = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("instructions"),instructions},{QStringLiteral("conversation"),history}}).toJson());
    prompt.replace(QLatin1Char('@'), QChar(0xff20));
    auto output = std::make_shared<QByteArray>();
    connect(process,&QProcess::readyReadStandardOutput,this,[this,process,output] {
        if (m_process != process) return;
        output->append(process->readAllStandardOutput());
        if (output->size() > 2*1024*1024) process->kill();
    });
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error) {
        if (m_process != process || error != QProcess::FailedToStart) return;
        m_process = nullptr; process->deleteLater(); Q_EMIT finished({},QStringLiteral("Could not start the official CLI. Install it, sign in in a terminal, and retry."));
    });
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,output,directory,connection](int code,QProcess::ExitStatus status) {
        Q_UNUSED(directory);
        process->deleteLater(); if(m_process!=process) return; m_process=nullptr;
        output->append(process->readAllStandardOutput());
        if (status!=QProcess::NormalExit || code!=0 || output->size()>2*1024*1024) { Q_EMIT finished({},QStringLiteral("The official client could not complete the request (exit %1). Open it in a terminal to check sign-in, quota and version compatibility. No API-key fallback was used.").arg(code)); return; }
        QString answer;
        if(connection.protocol==Protocol::CopilotAccount) answer=QString::fromUtf8(*output).trimmed();
        else {
            const auto document=QJsonDocument::fromJson(*output); auto result=document.object();
            if(document.isArray()) for(const auto &entry:document.array()) if(entry.toObject().contains(QStringLiteral("result"))) result=entry.toObject();
            answer=result.value(QStringLiteral("response")).toString(result.value(QStringLiteral("result")).toString());
            if(result.contains(QStringLiteral("error")) || result.value(QStringLiteral("is_error")).toBool()) answer.clear();
        }
        if(answer.trimmed().isEmpty()) Q_EMIT finished({},QStringLiteral("The official client returned no usable answer. Check sign-in and update the client, or select a supported model."));
        else { Q_EMIT updated(answer); Q_EMIT finished(answer,{}); }
    });
    connect(process,&QProcess::readyReadStandardError,process,[process] { process->readAllStandardError(); });
    QTimer::singleShot(180000,process,[owner=QPointer<Provider>(this),process] { if(owner && owner->m_process==process) process->kill(); });
    connect(process,&QProcess::started,process,[process,prompt] { process->write(prompt.toUtf8()); process->closeWriteChannel(); });
    process->start(QStandardPaths::findExecutable(accountExecutable(connection.protocol)),arguments);
}
