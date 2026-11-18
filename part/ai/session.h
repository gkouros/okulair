/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace OkulAIr
{
// The v1 transport is a standard PDF text note. Keep this independent of it.
class Session
{
public:
    static constexpr qsizetype MaximumBytes = 32 * 1024 * 1024;
    static constexpr qsizetype MaximumImageBytes = 4 * 1024 * 1024;
    void pruneImages();
    static QString marker();
    static bool decode(const QString &note, Session *result, QString *error);
    QString encode() const;
    void append(const QString &role, const QString &text, const QString &model = {}, const QString &kind = QStringLiteral("chat"), const QString &status = QStringLiteral("complete"));
    QJsonArray messages;
    QJsonArray workspaces;
    QString activeWorkspace = QStringLiteral("main");
    void ensureWorkspaces();
    QJsonObject workspace(const QString &id) const;
    bool setWorkspace(const QJsonObject &workspace);
    static QString workspaceOf(const QJsonObject &message);
};
}
