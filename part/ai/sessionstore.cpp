/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "sessionstore.h"
#include "core/annotations.h"
#include "core/document.h"
#include "core/page.h"

using namespace OkulAIr;

namespace
{
Okular::Annotation *sessionNote(const Okular::Document *document, QString *error)
{
    Okular::Annotation *found = nullptr;
    if (!document->pages()) {
        return nullptr;
    }
    for (auto *annotation : document->page(0)->annotations()) {
        if (annotation->subType() == Okular::Annotation::AText && annotation->contents().startsWith(Session::marker())) {
            if (found) {
                *error = QStringLiteral("Multiple AI session notes found. Resolve duplicates before editing AI history.");
                return nullptr;
            }
            found = annotation;
        }
    }
    return found;
}
}

bool SessionStore::load(const Okular::Document *document, Session *session, QString *error)
{
    error->clear();
    auto *note = sessionNote(document, error);
    if (!error->isEmpty()) {
        return false;
    }
    if (note) {
        return Session::decode(note->contents(), session, error);
    }
    *session = Session();
    return true;
}

bool SessionStore::write(Okular::Document *document, const Session &session, QString *error)
{
    error->clear();
    if (!document->pages() || !document->isAllowed(Okular::AllowNotes) || !document->canSaveChanges(Okular::Document::SaveAnnotationsCapability)) {
        *error = QStringLiteral("This document does not permit saving AI history as annotations.");
        return false;
    }
    const QString contents = session.encode();
    if (contents.toUtf8().size() > Session::MaximumBytes || session.messages.size() > 2000) {
        *error = QStringLiteral("AI history reached the 32 MiB storage limit or 2,000 messages. Delete unused chats or clear a chat in AI settings. Nothing was overwritten.");
        return false;
    }
    Session validated;
    if (!Session::decode(contents, &validated, error)) return false;
    auto *note = sessionNote(document, error);
    if (!error->isEmpty()) {
        return false;
    }
    if (note) {
        Session existing;
        if (!Session::decode(note->contents(), &existing, error)) {
            return false;
        }
        if (!document->canModifyPageAnnotation(note)) {
            *error = QStringLiteral("The AI session annotation is locked.");
            return false;
        }
        if (note->contents() != contents) {
            document->editPageAnnotationContents(0, note, contents, 0, 0, 0, false);
        }
    } else {
        auto *created = new Okular::TextAnnotation;
        created->setTextType(Okular::TextAnnotation::Linked);
        created->setTextIcon(QStringLiteral("Comment"));
        created->setAuthor(QStringLiteral("OkulAIr"));
        created->setContents(contents);
        created->setBoundingRectangle(Okular::NormalizedRect(0.02, 0.02, 0.05, 0.05));
        document->addPageAnnotation(0, created, false);
    }
    return true;
}
