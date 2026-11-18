/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "session.h"
namespace Okular { class Document; }
namespace OkulAIr
{
class SessionStore
{
public:
    static bool load(const Okular::Document *document, Session *session, QString *error);
    static bool write(Okular::Document *document, const Session &session, QString *error);
};
}
