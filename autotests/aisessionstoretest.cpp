/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "part/ai/sessionstore.h"
#include "core/annotations.h"
#include "core/document.h"
#include "core/page.h"
#include "settings_core.h"
#include <QFile>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace OkulAIr;
class AiSessionStoreTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void portablePdfRoundTrip();
    void preserveViewportForBackgroundHistory();
    void undoAndLockedNote();
    void preserveMalformedAndDuplicateNotes();
};

static bool open(Okular::Document &document, const QString &path)
{
    return document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)) == Okular::Document::OpenSuccess;
}

void AiSessionStoreTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    Okular::SettingsCore::instance(QStringLiteral("aisessionstoretest"));
}

void AiSessionStoreTest::portablePdfRoundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto source = directory.filePath(QStringLiteral("original.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), source));
    Okular::Document document(nullptr);
    QVERIFY(open(document, source));
    auto *manual = new Okular::TextAnnotation;
    manual->setContents(QStringLiteral("A human note"));
    manual->setBoundingRectangle(Okular::NormalizedRect(0.2, 0.2, 0.3, 0.3));
    document.addPageAnnotation(0, manual);
    Session session;
    session.append(QStringLiteral("user"), QString::fromUtf8("Explain this paper. Ελληνικά 😀"));
    session.append(QStringLiteral("assistant"), QStringLiteral("A summary [p. 1]."), QStringLiteral("mock-model"), QStringLiteral("summary"));
    QString error;
    QVERIFY2(SessionStore::write(&document, session, &error), qPrintable(error));
    const auto saved = directory.filePath(QStringLiteral("saved.pdf"));
    QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
    document.closeDocument();
    // A different location and Document instance must not need the original path or caches.
    const auto moved = directory.filePath(QStringLiteral("moved.pdf"));
    QVERIFY(QFile::rename(saved, moved));
    QVERIFY(QFile::remove(source));
    Okular::Document reopened(nullptr);
    QVERIFY(open(reopened, moved));
    Session restored;
    QVERIFY2(SessionStore::load(&reopened, &restored, &error), qPrintable(error));
    QCOMPARE(restored.messages, session.messages);
    QCOMPARE(reopened.page(0)->annotations().size(), 2);
    bool humanPresent = false;
    for (const auto *annotation : reopened.page(0)->annotations()) {
        humanPresent |= annotation->contents() == QLatin1String("A human note");
    }
    QVERIFY(humanPresent);
}

void AiSessionStoreTest::preserveViewportForBackgroundHistory()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral(KDESRCDIR "data/simple-multipage.pdf")));
    QVERIFY(document.pages() > 1);
    Okular::DocumentViewport view(1);
    view.rePos.enabled = true;
    view.rePos.normalizedX = 0.73;
    view.rePos.normalizedY = 0.68;
    document.setViewport(view);
    document.setVisiblePageRects({new Okular::VisiblePageRect(1, Okular::NormalizedRect(0.6, 0.5, 0.9, 0.85))});
    const auto position = document.viewport().toString();
    Session session;
    session.append(QStringLiteral("user"), QStringLiteral("Explain this region"));
    QString error;
    QVERIFY2(SessionStore::write(&document, session, &error), qPrintable(error));
    QCOMPARE(document.viewport().toString(), position); // New page-one note.
    session.append(QStringLiteral("assistant"), QStringLiteral("A focused answer"), QStringLiteral("fixture-model"));
    QVERIFY2(SessionStore::write(&document, session, &error), qPrintable(error));
    QCOMPARE(document.viewport().toString(), position); // Updating the note.
    document.undo();
    QCOMPARE(document.viewport().toString(), position);
    document.undo();
    QCOMPARE(document.viewport().toString(), position);
    document.redo();
    document.redo();
    QCOMPARE(document.viewport().toString(), position);
    Session restored;
    QVERIFY(SessionStore::load(&document, &restored, &error));
    QCOMPARE(restored.messages, session.messages);
    // Ordinary annotation commands still navigate to the edited annotation.
    auto *manual = new Okular::TextAnnotation;
    manual->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.2, 0.2));
    document.addPageAnnotation(0, manual);
    QCOMPARE(document.viewport().pageNumber, 0);
    document.setViewport(view);
    document.editPageAnnotationContents(0, manual, QStringLiteral("A manual edit"), 0, 0, 0);
    QCOMPARE(document.viewport().pageNumber, 0);
}

void AiSessionStoreTest::undoAndLockedNote()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral(KDESRCDIR "data/file1.pdf")));
    Session session;
    session.append(QStringLiteral("user"), QStringLiteral("First"));
    QString error;
    QVERIFY2(SessionStore::write(&document, session, &error), qPrintable(error));
    session.append(QStringLiteral("assistant"), QStringLiteral("Second"));
    QVERIFY(SessionStore::write(&document, session, &error));
    document.undo();
    Session restored;
    QVERIFY(SessionStore::load(&document, &restored, &error));
    QCOMPARE(restored.messages.size(), 1);
    document.redo();
    QVERIFY(SessionStore::load(&document, &restored, &error));
    QCOMPARE(restored.messages.size(), 2);
    auto *note = document.page(0)->annotations().first();
    note->setFlags(note->flags() | Okular::Annotation::DenyWrite);
    session.append(QStringLiteral("user"), QStringLiteral("Third"));
    QVERIFY(!SessionStore::write(&document, session, &error));
    QVERIFY(!error.isEmpty());
    document.setAnnotationEditingEnabled(false);
    QVERIFY(!SessionStore::write(&document, session, &error));
}

void AiSessionStoreTest::preserveMalformedAndDuplicateNotes()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral(KDESRCDIR "data/file1.pdf")));
    auto *damaged = new Okular::TextAnnotation;
    const QString contents = Session::marker() + QStringLiteral("{damaged");
    damaged->setContents(contents);
    damaged->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.2, 0.2));
    document.addPageAnnotation(0, damaged);
    Session session;
    QString error;
    QVERIFY(!SessionStore::load(&document, &session, &error));
    QVERIFY(!SessionStore::write(&document, session, &error));
    QCOMPARE(damaged->contents(), contents);
    auto *duplicate = new Okular::TextAnnotation;
    duplicate->setContents(Session().encode());
    duplicate->setBoundingRectangle(Okular::NormalizedRect(0.3, 0.3, 0.4, 0.4));
    document.addPageAnnotation(0, duplicate);
    QVERIFY(!SessionStore::load(&document, &session, &error));
    QVERIFY(error.contains(QLatin1String("Multiple")));
    QCOMPARE(document.page(0)->annotations().size(), 2);
}
QTEST_MAIN(AiSessionStoreTest)
#include "aisessionstoretest.moc"
