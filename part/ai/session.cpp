/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "session.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QSet>

using namespace OkulAIr;

QString Session::marker()
{
    return QStringLiteral("OkulAIr document conversation\n");
}

QString Session::encode() const
{
    return marker() + QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("format"), QStringLiteral("org.okulair.session")},
                                                               {QStringLiteral("version"), workspaces.isEmpty() ? 1 : 2},
                                                               {QStringLiteral("workspaces"), workspaces},
                                                               {QStringLiteral("activeWorkspace"), activeWorkspace},
                                                               {QStringLiteral("messages"), messages}}).toJson(QJsonDocument::Indented));
}

bool Session::decode(const QString &note, Session *result, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (!note.startsWith(marker()) || note.toUtf8().size() > MaximumBytes) {
        return fail(QStringLiteral("Invalid or oversized AI session note."));
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(note.mid(marker().size()).toUtf8(), &parseError);
    const auto object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || object.value(QStringLiteral("format")) != QStringLiteral("org.okulair.session")
        || (object.value(QStringLiteral("version")) != 1 && object.value(QStringLiteral("version")) != 2) || !object.value(QStringLiteral("messages")).isArray()) {
        return fail(QStringLiteral("Unsupported or damaged AI session. The original note has been preserved."));
    }
    const auto messages = object.value(QStringLiteral("messages")).toArray();
    if (messages.size() > 2000) {
        return fail(QStringLiteral("Too many messages in AI session."));
    }
    for (const auto &entry : messages) {
        const auto message = entry.toObject();
        const auto role = message.value(QStringLiteral("role")).toString();
        const auto status = message.value(QStringLiteral("status")).toString();
        if (!entry.isObject() || (role != QLatin1String("user") && role != QLatin1String("assistant")) || !message.value(QStringLiteral("content")).isString()
            || (status != QLatin1String("complete") && status != QLatin1String("interrupted"))) {
            return fail(QStringLiteral("Invalid message in AI session. The original note has been preserved."));
        }
    }
    const auto workspaces = object.value(QStringLiteral("workspaces")).toArray();
    if (workspaces.size() > 32 || (object.value(QStringLiteral("version")) == 2 && workspaces.isEmpty())) return fail(QStringLiteral("Invalid workspace list."));
    QSet<QString> ids;
    for (const auto &entry : workspaces) {
        const auto tab = entry.toObject();
        const auto id = tab.value(QStringLiteral("id")).toString();
        const auto type = tab.value(QStringLiteral("type")).toString();
        if (id.isEmpty() || id.size() > 64 || ids.contains(id) || tab.value(QStringLiteral("title")).toString().isEmpty()
            || tab.value(QStringLiteral("title")).toString().size() > 80 || (type != QLatin1String("chat") && type != QLatin1String("scratchpad")))
            return fail(QStringLiteral("Invalid workspace metadata. Original history was preserved."));
        ids.insert(id);
    }
    if (!workspaces.isEmpty() && (!ids.contains(QStringLiteral("main")) || workspaces.first().toObject().value(QStringLiteral("id")) != QStringLiteral("main"))) return fail(QStringLiteral("Missing paper chat."));
    for (const auto &entry : messages) {
        const auto id = workspaceOf(entry.toObject());
        if ((!workspaces.isEmpty() && !ids.contains(id)) || (workspaces.isEmpty() && id != QLatin1String("main"))) return fail(QStringLiteral("Message refers to a missing workspace."));
    }
    if (!workspaces.isEmpty()) {
        const auto main = workspaces.first().toObject();
        if (main.value(QStringLiteral("type")) != QLatin1String("chat") || main.value(QStringLiteral("closed")).toBool()) return fail(QStringLiteral("Invalid paper chat metadata."));
    }
    result->messages = messages;
    result->workspaces = workspaces;
    result->activeWorkspace = object.value(QStringLiteral("activeWorkspace")).toString(QStringLiteral("main"));
    if (!ids.contains(result->activeWorkspace)) result->activeWorkspace = QStringLiteral("main");
    if (error) {
        error->clear();
    }
    return true;
}

void Session::append(const QString &role, const QString &text, const QString &model, const QString &kind, const QString &status)
{
    messages.append(QJsonObject{{QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                               {QStringLiteral("role"), role},
                               {QStringLiteral("content"), text},
                               {QStringLiteral("model"), model},
                               {QStringLiteral("kind"), kind},
                               {QStringLiteral("status"), status},
                               {QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
}

void Session::ensureWorkspaces()
{
    if (workspaces.isEmpty()) workspaces.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("main")}, {QStringLiteral("title"), QStringLiteral("Main chat")}, {QStringLiteral("type"), QStringLiteral("chat")}});
}
QJsonObject Session::workspace(const QString &id) const
{
    for (const auto &entry : workspaces) if (entry.toObject().value(QStringLiteral("id")) == id) return entry.toObject();
    return {};
}
bool Session::setWorkspace(const QJsonObject &workspace)
{
    for (qsizetype i = 0; i < workspaces.size(); ++i) if (workspaces.at(i).toObject().value(QStringLiteral("id")) == workspace.value(QStringLiteral("id"))) { workspaces[i] = workspace; return true; }
    return false;
}
QString Session::workspaceOf(const QJsonObject &message)
{
    return message.value(QStringLiteral("workspace")).toString(QStringLiteral("main"));
}

void Session::pruneImages()
{
    for (qsizetype i = 0; i < workspaces.size(); ++i) {
        auto tab = workspaces.at(i).toObject();
        QSet<QString> used;
        used.insert(tab.value(QStringLiteral("selectionContext")).toObject().value(QStringLiteral("imageRef")).toString());
        for (const auto &entry : messages) {
            const auto message = entry.toObject();
            if (workspaceOf(message) == tab.value(QStringLiteral("id")).toString())
                used.insert(message.value(QStringLiteral("context")).toObject().value(QStringLiteral("imageRef")).toString());
        }
        const auto images = tab.value(QStringLiteral("images")).toObject();
        QJsonObject kept;
        for (auto it = images.begin(); it != images.end(); ++it) if (used.contains(it.key())) kept.insert(it.key(), it.value());
        if (kept.isEmpty()) tab.remove(QStringLiteral("images"));
        else tab.insert(QStringLiteral("images"), kept);
        workspaces[i] = tab;
    }
}
