/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "part/ai/assistantwidget.h"
#include "part/ai/sessionstore.h"
#include "core/annotations.h"
#include "core/document.h"
#include "core/page.h"
#include "settings_core.h"
#include <QApplication>
#include <KMessageWidget>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QComboBox>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QClipboard>
#include <QMimeData>
#include <QDialogButtonBox>
#include <QDialog>
#include <QDockWidget>
#include <QMainWindow>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QImage>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextCursor>
#include <QScopeGuard>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QShortcut>
#include <QTimer>
#include <QMessageBox>
#include <QScrollBar>
#include <QPointer>
#include <QTabBar>
#include <QToolButton>
#include <QMenu>
#include <QCheckBox>
#include <QListWidget>
#include <QPdfWriter>
#include <QPainter>
#include <QRegularExpression>
#include <QColorDialog>
#include <QSpinBox>

using namespace OkulAIr;
namespace
{
class FakeEndpoint : public QTcpServer
{
public:
    FakeEndpoint()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            streamSocket = socket;
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                const QByteArray input = socket->property("input").toByteArray() + socket->readAll();
                socket->setProperty("input", input);
                request = input;
                if (hold || socket->property("sent").toBool()) {
                    return;
                }
                const auto split = input.indexOf("\r\n\r\n");
                if (split < 0) {
                    return;
                }
                int length = 0;
                for (const auto &line : input.left(split).split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        length = line.mid(15).trimmed().toInt();
                    }
                }
                if (input.size() < split + 4 + length) {
                    return;
                }
                if (defer && defer(input)) return;
                socket->setProperty("sent", true);
                if (input.startsWith("GET ")) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"+QJsonDocument(QJsonObject{{QStringLiteral("data"),models}}).toJson(QJsonDocument::Compact));
                    socket->disconnectFromHost();
                    return;
                }
                const auto json = QJsonDocument(QJsonObject{{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), responder ? responder(input) : answer}}}}}}}).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\ndata: " + json + "\n\ndata: [DONE]\n\n");
                socket->disconnectFromHost();
            });
        });
    }
    QJsonArray models{QJsonObject{{QStringLiteral("id"),QStringLiteral("discovered-model")}}};
    std::function<QString(const QByteArray &)> responder;
    std::function<bool(const QByteArray &)> defer;
    void stream(const QString &text, bool finish = false)
    {
        if (!streamSocket) return;
        if (!streamSocket->property("streamHeaders").toBool()) {
            streamSocket->setProperty("streamHeaders", true);
            streamSocket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
        }
        const auto event = QJsonDocument(QJsonObject{{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), text}}}}}}}).toJson(QJsonDocument::Compact);
        streamSocket->write("data: " + event + "\n\n");
        if (finish) { streamSocket->write("data: [DONE]\n\n"); streamSocket->disconnectFromHost(); }
        else streamSocket->flush();
    }
    QPointer<QTcpSocket> streamSocket;
    QByteArray request;
    QString answer = QStringLiteral("An answer [p. 1]. <img src=\"https://example.com/tracker\">");
    bool hold = false;
};

bool open(Okular::Document &document, const QString &name)
{
    const auto path = QFileInfo(name).isAbsolute() ? name : QStringLiteral(KDESRCDIR "data/") + name;
    return document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)) == Okular::Document::OpenSuccess;
}

QPointer<QTimer> sharingAnswerTimer;
void answerSharing(bool allow = true, int *count = nullptr)
{
    delete sharingAnswerTimer.data();
    auto *timer = new QTimer(qApp);
    sharingAnswerTimer = timer;
    QObject::connect(timer, &QTimer::timeout, qApp, [timer, allow, count] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != QLatin1String("aiSharingConfirmation")) return;
        timer->stop();
        timer->deleteLater();
        QVERIFY(dialog->informativeText().contains(QLatin1String("outside your device")));
        QVERIFY(dialog->informativeText().contains(QLatin1String("remembered")));
        if (count) ++*count;
        if (qEnvironmentVariableIsSet("OKULAIR_CHAT_POLISH_PREVIEW")) QVERIFY(dialog->grab().save(QStringLiteral("/tmp/okulair-sharing-preview.png")));
        dialog->button(allow ? QMessageBox::Yes : QMessageBox::Cancel)->click();
    });
    timer->start(5);
}

void configure(AssistantWidget &widget, FakeEndpoint &server)
{
    widget.findChild<QComboBox *>()->setCurrentIndex(0);
    widget.findChild<QLineEdit *>(QStringLiteral("aiEndpoint"))->setText(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()));
    widget.findChild<QLineEdit *>(QStringLiteral("aiModel"))->setText(QStringLiteral("mock-model"));
}
}

class AiAssistantTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void chatAndConsent();
    void visibleConnectionErrors();
    void discoverModels();
    void summary();
    void highlights();
    void switchingDocumentsCancels();
    void keyboardSendAndNewline();
    void focusedContext_data();
    void focusedContext();
    void missingSelectionDoesNotSend();
    void movableChatDock();
    void legacyHighlightsStayOutOfChat();
    void rememberedSharing();
    void streamingScrollControl();
    void attachments();
    void clearControls();
    void workspaceIsolationAndNotes();
    void legacyWorkspaceMigration();
    void pinnedContextAndSelectionActions();
    void exactEvidenceLinks();
    void groupedEvidenceLinks();
    void highlightCategoriesAndFilters();
    void highlightBatchCoverage();
    void requestQueueAndCancellation();
    void explainManualHighlight();
    void editableCategories();
    void independentBackgroundHighlights();
    void cancelledHighlightStatus();
    void copyChatShortcut();
    void apiConnectionError();
    void screenshotPromptThumbnail();
    void selectExistingHighlightCategories();
    void checkedHighlightCategories();
    void thinkingEffortUi();
    void removedScreenshotStaysRemoved();
    void deleteOtherChats();
    void consecutiveStreamingAnswers();
    void requestPopupKeepsLayout();
    void progressivelyFormattedAnswer();
};

void AiAssistantTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    Okular::SettingsCore::instance(QStringLiteral("aiassistanttest"));
    KConfigGroup sharing(KSharedConfig::openConfig(), QStringLiteral("OkulAIrDocumentSharing"));
    sharing.deleteGroup();
    sharing.sync();
}

void AiAssistantTest::cleanup()
{
    delete sharingAnswerTimer.data();
}

void AiAssistantTest::visibleConnectionErrors()
{
    Okular::Document document(nullptr);
    AssistantWidget widget(&document, nullptr);
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("A question"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    auto *banner = widget.findChild<KMessageWidget *>(QStringLiteral("aiStatus"));
    QVERIFY(banner);
    QCOMPARE(banner->messageType(), KMessageWidget::Error);
    QVERIFY(banner->text().contains(QLatin1String("Open a PDF")));
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    widget.findChild<QComboBox *>(QStringLiteral("aiService"))->setCurrentIndex(3);
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("A question"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QCOMPARE(banner->messageType(), KMessageWidget::Error);
    QVERIFY(banner->text().contains(QLatin1String("Continue with ChatGPT")));
    QVERIFY(document.page(0)->annotations().isEmpty());
    if (qEnvironmentVariableIsSet("OKULAIR_TEST_CHATGPT_PREVIEW")) {
        widget.resize(450, 900);
        widget.show();
        QVERIFY(widget.grab().save(QStringLiteral("/tmp/okulair-chatgpt-preview.png")));
    }
}

void AiAssistantTest::discoverModels()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    widget.findChild<QLineEdit *>(QStringLiteral("aiModel"))->clear();
    widget.findChild<QPushButton *>(QStringLiteral("aiConnect"))->click();
    auto *picker = widget.findChild<QComboBox *>(QStringLiteral("aiModelPicker"));
    QTRY_COMPARE(picker->count(), 1);
    QCOMPARE(picker->currentText(), QStringLiteral("discovered-model"));
    QVERIFY(server.request.startsWith("GET /v1/models "));
    QCOMPARE(widget.findChild<KMessageWidget *>(QStringLiteral("aiStatus"))->messageType(), KMessageWidget::Positive);
}

void AiAssistantTest::chatAndConsent()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("Explain the document"));
    answerSharing(false);
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QVERIFY(server.request.isEmpty());
    QCOMPARE(document.page(0)->annotations().size(), 0);
    answerSharing();
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    Session session;
    QString error;
    QTRY_VERIFY(SessionStore::load(&document, &session, &error) && session.messages.size() == 2);
    QVERIFY(server.request.contains("random text"));
    QCOMPARE(session.messages.last().toObject().value(QStringLiteral("model")).toString(), QStringLiteral("mock-model"));
    const auto html = widget.findChild<QTextBrowser *>()->toHtml();
    QVERIFY(html.contains(QLatin1String("&lt;img")));
    QVERIFY(!html.contains(QLatin1String("<img src=\"https://example.com")));
    QVERIFY(!html.contains(QLatin1String("okulair://page/")));
    if (qEnvironmentVariableIsSet("OKULAIR_TEST_PREVIEW")) {
        widget.resize(430, 850);
        widget.show();
        QTest::qWait(20);
        QVERIFY(widget.grab().save(QStringLiteral("/tmp/okulair-ai-preview.png")));
    }
    document.undo();
    QVERIFY(SessionStore::load(&document, &session, &error));
    QCOMPARE(session.messages.size(), 1);
}

void AiAssistantTest::summary()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    server.answer = QStringLiteral("## In one sentence\nA simpler way to study a problem.\n\n## Goal\n- Explain **the main idea**. [p. 1]\n\n## Caveats\n- Inferred concern: more evidence is needed.\n<img src=\"https://example.com/tracker\"> ");
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    answerSharing();
    widget.findChild<QPushButton *>(QStringLiteral("aiSummary"))->click();
    Session session;
    QString error;
    QTRY_VERIFY(SessionStore::load(&document, &session, &error) && session.messages.size() == 2);
    QCOMPARE(session.messages.last().toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("summary"));
    QVERIFY(server.request.contains("author-stated limitations"));
    QCOMPARE(widget.findChild<QPushButton *>(QStringLiteral("aiSummary"))->text(), QStringLiteral("Overview"));
    QVERIFY(server.request.contains("How it works"));
    QVERIFY(server.request.contains("Key takeaways"));
    QVERIFY(server.request.contains("Why it matters"));
    const auto *history = widget.findChild<QTextBrowser *>();
    const auto plain = history->toPlainText();
    QVERIFY(plain.contains(QLatin1String("In one sentence")));
    QVERIFY(plain.contains(QStringLiteral("• Explain the main idea.")));
    QVERIFY(!plain.contains(QLatin1String("##")));
    QVERIFY(!plain.contains(QLatin1String("**")));
    QVERIFY(!plain.contains(QLatin1String("Create a reader-friendly")));
    QVERIFY(!history->toHtml().contains(QLatin1String("okulair://page/")));
    QVERIFY(!history->toHtml().contains(QLatin1String("<img src=\"https://example.com")));
    if (qEnvironmentVariableIsSet("OKULAIR_OVERVIEW_PREVIEW")) {
        widget.resize(460, 850); widget.show(); QTest::qWait(20);
        QVERIFY(widget.grab().save(QStringLiteral("/tmp/okulair-overview-preview.png")));
    }
}

void AiAssistantTest::highlights()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    server.answer = QStringLiteral("[{\"page\":1,\"quote\":\"some random text\",\"category\":\"keyword\",\"reason\":\"An exact quote\"},{\"page\":1,\"quote\":\"hallucinated passage\",\"reason\":\"reject this\"}]");
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    answerSharing();
    QTimer apply;
    connect(&apply, &QTimer::timeout, this, [&] {
        if (auto *modal = QApplication::activeModalWidget()) {
            if (auto *buttons = modal->findChild<QDialogButtonBox *>()) {
                apply.stop();
                buttons->button(QDialogButtonBox::Apply)->click();
            }
        }
    });
    apply.start(10);
    widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    auto count = [&] {
        int highlights = 0;
        for (auto *annotation : document.page(0)->annotations()) {
            highlights += annotation->subType() == Okular::Annotation::AHighlight;
        }
        return highlights;
    };
    QTRY_COMPARE(count(), 1);
    const auto *highlight = document.page(0)->annotations().first();
    QCOMPARE(highlight->author(), QStringLiteral("AI assistant · mock-model"));
    QVERIFY(highlight->creationDate().isValid());
    QVERIFY(highlight->modificationDate().isValid());
    QVERIFY(qAbs(highlight->creationDate().secsTo(QDateTime::currentDateTime())) < 10);
    QCOMPARE(highlight->style().color(), QColor(QStringLiteral("#F6D86B")));
    QVERIFY(highlight->contents().contains(QLatin1String("An exact quote")));
    QVERIFY(!highlight->contents().contains(QLatin1String("OkulAIr")));
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().isEmpty());
    Session session;
    QString error;
    QVERIFY(SessionStore::load(&document, &session, &error));
    QVERIFY(session.messages.isEmpty());
    QTemporaryDir directory;
    const auto saved = directory.filePath(QStringLiteral("highlight.pdf"));
    QVERIFY(document.saveChanges(saved, &error));
    Okular::Document reopened(nullptr);
    QVERIFY(reopened.openDocument(saved, QUrl::fromLocalFile(saved), QMimeDatabase().mimeTypeForFile(saved)) == Okular::Document::OpenSuccess);
    const auto *restored = reopened.page(0)->annotations().first();
    QCOMPARE(restored->author(), highlight->author());
    QCOMPARE(restored->creationDate().toSecsSinceEpoch(), highlight->creationDate().toSecsSinceEpoch());
    QVERIFY(restored->modificationDate().isValid());
    document.undo();
    QCOMPARE(count(), 0);
}

void AiAssistantTest::keyboardSendAndNewline()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    answerSharing();
    auto *question = widget.findChild<QPlainTextEdit *>();
    question->setPlainText(QStringLiteral("Keyboard question"));
    question->moveCursor(QTextCursor::End);
    QTest::keyClick(question, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(question->toPlainText(), QStringLiteral("Keyboard question\n"));
    QVERIFY(server.request.isEmpty());
    QTest::keyClick(question, Qt::Key_Return);
    Session session;
    QString error;
    QTRY_VERIFY(SessionStore::load(&document, &session, &error) && session.messages.size() == 2);
    QVERIFY(server.request.contains("Keyboard question"));
    QVERIFY(question->toPlainText().isEmpty());
}

void AiAssistantTest::focusedContext_data()
{
    QTest::addColumn<int>("scope");
    QTest::newRow("selected-text") << 1;
    QTest::newRow("current-view-image") << 2;
}
void AiAssistantTest::focusedContext()
{
    QFETCH(int, scope);
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    FocusContext focus;
    focus.text = QStringLiteral("--- PAGE 1 (selection) ---\nonly focused evidence");
    focus.label = QStringLiteral("Scoped evidence · p. 1");
    focus.regions = QJsonArray{QJsonObject{{QStringLiteral("page"), 0}}};
    if (scope == 2) {
        QImage image(10, 10, QImage::Format_RGB32);
        image.fill(Qt::white);
        QBuffer buffer(&focus.image); buffer.open(QIODevice::WriteOnly); QVERIFY(image.save(&buffer, "PNG"));
    }
    bool selectedRequested = false;
    widget.setContextSource([&](bool selected) { selectedRequested = selected; return focus; });
    widget.findChild<QButtonGroup *>(QStringLiteral("aiScope"))->button(scope)->click();
    answerSharing();
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("Explain the focus"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    Session session;
    QString error;
    QTRY_VERIFY(SessionStore::load(&document, &session, &error) && session.messages.size() == 2);
    QCOMPARE(selectedRequested, scope == 1);
    QVERIFY(server.request.contains("only focused evidence"));
    QVERIFY(!server.request.contains("some random text")); // Rest of the PDF stays local.
    QCOMPARE(server.request.contains("data:image/png;base64,"), scope == 2);
    const auto context = session.messages.first().toObject().value(QStringLiteral("context")).toObject();
    QCOMPARE(context.value(QStringLiteral("text")).toString(), focus.text);
    QCOMPARE(context.value(QStringLiteral("visual")).toBool(), scope == 2);
    QVERIFY(!session.encode().contains(QLatin1String("base64"))); // Image bytes never enter the PDF note.
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().contains(focus.label));
}

void AiAssistantTest::missingSelectionDoesNotSend()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    widget.setContextSource([](bool) { return FocusContext{}; });
    widget.findChild<QButtonGroup *>(QStringLiteral("aiScope"))->button(1)->click();
    answerSharing();
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("Explain this"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QVERIFY(server.request.isEmpty());
    QVERIFY(widget.findChild<KMessageWidget *>()->text().contains(QLatin1String("Select text")));
    QVERIFY(document.page(0)->annotations().isEmpty());
}

void AiAssistantTest::movableChatDock()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    QMainWindow host;
    host.setCentralWidget(new QWidget);
    auto *widget = new AssistantWidget(&document, nullptr);
    auto *settings = widget->settingsWidget();
    auto *settingsDock = new QDockWidget(&host);
    settingsDock->setObjectName(QStringLiteral("testSettings"));
    settingsDock->setWidget(settings);
    host.addDockWidget(Qt::LeftDockWidgetArea, settingsDock);
    widget->attachChatDock(&host);
    auto *dock = host.findChild<QDockWidget *>(QStringLiteral("okulair_ai_chat"));
    QVERIFY(dock);
    QCOMPARE(dock->allowedAreas(), Qt::AllDockWidgetAreas);
    QVERIFY(!widget->isAncestorOf(settings));
    for (const auto area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea, Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea}) {
        host.addDockWidget(area, dock);
        QCOMPARE(host.dockWidgetArea(dock), area);
    }
    host.addDockWidget(Qt::RightDockWidgetArea, dock);
    for (const auto *button : widget->findChildren<QPushButton *>()) {
        QVERIFY(!button->text().contains(QLatin1String("Save PDF")));
        QVERIFY(!button->text().contains(QLatin1String("Export")));
    }
    dock->hide();
    settings->findChild<QPushButton *>(QStringLiteral("aiShowChat"))->click();
    QVERIFY(!dock->isHidden());
    widget->setContextSource([](bool) { FocusContext context; context.text = QStringLiteral("selected words"); context.label = QStringLiteral("Selection"); return context; });
    widget->showChat(true);
    QCOMPARE(widget->findChild<QButtonGroup *>(QStringLiteral("aiScope"))->checkedId(), 1);
    QSignalSpy openSettings(widget, &AssistantWidget::settingsRequested);
    widget->findChild<QPushButton *>(QStringLiteral("aiShowSettings"))->click();
    QCOMPARE(openSettings.size(), 1);
    host.resize(1200, 800); host.show();
    dock->setFloating(true);
    QVERIFY(dock->isFloating());
    host.hide();
    QVERIFY(dock->isHidden());
    host.show();
    QVERIFY(!dock->isHidden());
    dock->setFloating(false);
    host.addDockWidget(Qt::RightDockWidgetArea, dock);
    if (qEnvironmentVariableIsSet("OKULAIR_LAYOUT_PREVIEW")) {
        QVERIFY(host.grab().save(QStringLiteral("/tmp/okulair-layout-preview.png")));
    }
    host.addDockWidget(Qt::BottomDockWidgetArea, dock);
    delete widget; // Document observer must die before Document.
    QMainWindow reopened;
    reopened.setCentralWidget(new QWidget);
    auto *restored = new AssistantWidget(&document, nullptr);
    restored->attachChatDock(&reopened);
    auto *restoredDock = reopened.findChild<QDockWidget *>(QStringLiteral("okulair_ai_chat"));
    QVERIFY(restoredDock);
    QCOMPARE(reopened.dockWidgetArea(restoredDock), Qt::BottomDockWidgetArea);
    delete restored;
}

void AiAssistantTest::legacyHighlightsStayOutOfChat()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    Session old;
    old.append(QStringLiteral("user"), QStringLiteral("Earlier question"));
    old.append(QStringLiteral("assistant"), QStringLiteral("Earlier answer"), QStringLiteral("fixture"));
    old.append(QStringLiteral("user"), QStringLiteral("legacy-highlighter-request"), {}, QStringLiteral("highlights"));
    old.append(QStringLiteral("assistant"), QStringLiteral("legacy-highlighter-json"), QStringLiteral("fixture"), QStringLiteral("highlights"));
    QString error;
    QVERIFY(SessionStore::write(&document, old, &error));
    FakeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    const auto transcript = widget.findChild<QTextBrowser *>()->toPlainText();
    QVERIFY(transcript.contains(QLatin1String("Earlier answer")));
    QVERIFY(!transcript.contains(QLatin1String("legacy-highlighter")));
    answerSharing();
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("New question"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    Session updated;
    QTRY_VERIFY(SessionStore::load(&document, &updated, &error) && updated.messages.size() == 6);
    QVERIFY(!server.request.contains("legacy-highlighter"));
    QVERIFY(server.request.contains("Earlier answer"));
    QCOMPARE(updated.messages.at(3), old.messages.at(3)); // Preserve old PDF data, while hiding it from chat.
}

void AiAssistantTest::rememberedSharing()
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("remembered.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), path));
    FakeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int confirmations = 0;
    const auto ask = [&](AssistantWidget &widget, int expectedMessages, Okular::Document &document) {
        widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("A remembered question"));
        widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
        Session session; QString error;
        QTRY_VERIFY(SessionStore::load(&document, &session, &error) && session.messages.size() == expectedMessages);
    };
    {
        Okular::Document document(nullptr);
        QVERIFY(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)) == Okular::Document::OpenSuccess);
        AssistantWidget widget(&document, nullptr);
        configure(widget, server);
        answerSharing(true, &confirmations);
        ask(widget, 2, document);
        ask(widget, 4, document); // No second confirmation.
    }
    QCOMPARE(confirmations, 1);
    {
        Okular::Document reopened(nullptr);
        QVERIFY(reopened.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)) == Okular::Document::OpenSuccess);
        AssistantWidget widget(&reopened, nullptr);
        configure(widget, server);
        ask(widget, 2, reopened); // Permission survives reopen; no PDF save required.
        auto *reset = widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiResetSharing"));
        QVERIFY(reset && reset->isEnabled());
        reset->click();
        answerSharing(true, &confirmations);
        ask(widget, 4, reopened);
        QCOMPARE(confirmations, 2);
        FakeEndpoint another;
        QVERIFY(another.listen(QHostAddress::LocalHost));
        configure(widget, another);
        answerSharing(false, &confirmations);
        widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("Other destination"));
        widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
        QVERIFY(another.request.isEmpty());
        QCOMPARE(confirmations, 3);
    }
}

void AiAssistantTest::streamingScrollControl()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    server.hold = true;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    widget.resize(480, 760);
    widget.show();
    auto *history = widget.findChild<QTextBrowser *>();
    auto *question = widget.findChild<QPlainTextEdit *>();
    auto *scope = widget.findChild<QWidget *>(QStringLiteral("aiScopeBar"));
    auto *status = widget.findChild<KMessageWidget *>();
    QVERIFY(history->geometry().bottom() < scope->geometry().top());
    QVERIFY(scope->geometry().bottom() < question->geometry().top());
    QVERIFY(question->geometry().bottom() < status->geometry().top());
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->geometry().bottom() < status->geometry().top());
    question->setPlainText(QStringLiteral("Stream a long answer"));
    answerSharing();
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(server.request.contains("Stream a long answer"));
    QString first;
    for (int i = 0; i < 100; ++i) first += QStringLiteral("Earlier paragraph %1\n").arg(i);
    server.stream(first);
    auto *scroll = history->verticalScrollBar();
    QTRY_VERIFY(scroll->maximum() > 300);
    QTRY_COMPARE(scroll->value(), scroll->maximum());
    scroll->setValue(scroll->maximum() / 3);
    const auto readingPosition = scroll->value();
    const auto previousMaximum = scroll->maximum();
    server.stream(QString(100, QLatin1Char('x')) + QStringLiteral("\n") + first + QStringLiteral("stream-marker-one"));
    QTRY_VERIFY(history->toPlainText().contains(QLatin1String("stream-marker-one")));
    QVERIFY(scroll->maximum() > previousMaximum);
    QCOMPARE(scroll->value(), readingPosition);
    scroll->setValue(scroll->maximum()); // Returning to bottom resumes following.
    QSignalSpy changes(history->document(), &QTextDocument::contentsChange);
    server.stream(first + QStringLiteral("stream-marker-two"));
    QTRY_VERIFY(history->toPlainText().contains(QLatin1String("stream-marker-two")));
    QCOMPARE(scroll->value(), scroll->maximum());
    for (const auto &change : changes) QVERIFY(change.at(0).toInt() > history->toPlainText().indexOf(QStringLiteral("AI assistant"))); // Only the active answer is reformatted.
    scroll->setValue(scroll->maximum() / 2);
    const auto finalReadingPosition = scroll->value();
    server.stream(QStringLiteral("Finished."), true);
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QCOMPARE(scroll->value(), finalReadingPosition);
    const auto html = history->toHtml();
    QVERIFY(html.contains(QLatin1String("<table")));
    QVERIFY(history->toPlainText().contains(QLatin1String("AI assistant")));
    if (qEnvironmentVariableIsSet("OKULAIR_CHAT_POLISH_PREVIEW")) {
        QVERIFY(widget.grab().save(QStringLiteral("/tmp/okulair-chat-polish-preview.png")));
    }
}

void AiAssistantTest::attachments()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    auto *question = widget.findChild<QPlainTextEdit *>();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile text(directory.filePath(QStringLiteral("notes.txt")));
    QVERIFY(text.open(QIODevice::WriteOnly));
    text.write("External evidence: attachment-marker-123"); text.close();
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(text.fileName()), QUrl::fromLocalFile(QStringLiteral(KDESRCDIR "data/file2.pdf"))});
    QApplication::clipboard()->setMimeData(mime);
    QTest::keyClick(question, Qt::Key_V, Qt::ControlModifier);
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiAttachment0")));
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiAttachment1")));
    // A detailed screenshot above both the old 96 KiB image cap and 1 MiB
    // session cap must remain readable and survive saving/reopening the PDF.
    QImage image(640, 480, QImage::Format_RGB32);
    quint32 random = 0x12345678;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        image.setPixel(x, y, 0xff000000 | (random & 0xffffff));
    }
    QApplication::clipboard()->setImage(image);
    QTest::keyClick(question, Qt::Key_V, Qt::ControlModifier);
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiAttachment2")));
    QVERIFY(server.request.isEmpty()); // Attaching never uploads content.
    question->setPlainText(QStringLiteral("Compare these attachments"));
    answerSharing(false);
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QVERIFY(server.request.isEmpty());
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiAttachment2")));
    answerSharing();
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QVERIFY(server.request.contains("attachment-marker-123"));
    QVERIFY(server.request.contains("data:image/png;base64,"));
    Session session; QString error;
    QVERIFY(SessionStore::load(&document, &session, &error));
    const auto files = session.messages.first().toObject().value(QStringLiteral("attachments")).toArray();
    QCOMPARE(files.size(), 3);
    QVERIFY(files.at(1).toObject().value(QStringLiteral("text")).toString().contains(QLatin1String("Attachment file2.pdf, p. 1")));
    const auto savedScreenshot = QByteArray::fromBase64(files.at(2).toObject().value(QStringLiteral("image")).toString().toLatin1());
    QVERIFY(savedScreenshot.size() > 96 * 1024);
    QCOMPARE(QImage::fromData(savedScreenshot).size(), image.size());
    QVERIFY(session.encode().toUtf8().size() > 1024 * 1024);
    QVERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiAttachment0")));
    Session decoded;
    QVERIFY(Session::decode(session.encode(), &decoded, &error));
    QCOMPARE(decoded.messages, session.messages);
    const auto saved = directory.filePath(QStringLiteral("with-attachments.pdf"));
    QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
    Okular::Document reopened(nullptr);
    QVERIFY(reopened.openDocument(saved, QUrl::fromLocalFile(saved), QMimeDatabase().mimeTypeForFile(saved)) == Okular::Document::OpenSuccess);
    Session restored;
    QVERIFY(SessionStore::load(&reopened, &restored, &error));
    QCOMPARE(restored.messages, session.messages);
    server.request.clear();
    question->setPlainText(QStringLiteral("What about the earlier screenshot?"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QVERIFY(server.request.contains("attachment-marker-123"));
    QVERIFY(server.request.contains("data:image/png;base64,"));
    QApplication::clipboard()->clear();
}

void AiAssistantTest::clearControls()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    Session session; session.append(QStringLiteral("user"), QStringLiteral("Clear me"));
    QString error;
    QVERIFY(SessionStore::write(&document, session, &error));
    auto *human = new Okular::HighlightAnnotation;
    human->setAuthor(QStringLiteral("Reader"));
    human->setBoundingRectangle(Okular::NormalizedRect(.1, .1, .2, .2));
    auto *ai = new Okular::HighlightAnnotation;
    ai->setAuthor(QStringLiteral("AI assistant · mock-model"));
    ai->setBoundingRectangle(Okular::NormalizedRect(.5, .5, .6, .6));
    document.addPageAnnotation(0, human, false);
    document.addPageAnnotation(0, ai, false);
    AssistantWidget widget(&document, nullptr);
    const auto viewport = document.viewport().toString();
    widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiClearHighlights"))->click();
    QCOMPARE(document.page(0)->annotations().size(), 2);
    QVERIFY(document.page(0)->annotations().contains(human));
    QCOMPARE(document.viewport().toString(), viewport);
    document.undo();
    QCOMPARE(document.page(0)->annotations().size(), 3);
    QCOMPARE(document.viewport().toString(), viewport);
    widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiClearUserHighlights"))->click();
    QCOMPARE(document.page(0)->annotations().size(), 2);
    QVERIFY(document.page(0)->annotations().contains(ai));
    QVERIFY(!document.page(0)->annotations().contains(human));
    QCOMPARE(document.viewport().toString(), viewport);
    document.undo();
    QCOMPARE(document.page(0)->annotations().size(), 3);
    QVERIFY(document.page(0)->annotations().contains(human));
    QCOMPARE(document.viewport().toString(), viewport);
    widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiClearChat"))->click();
    QVERIFY(SessionStore::load(&document, &session, &error));
    QVERIFY(session.messages.isEmpty());
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().isEmpty());
    document.undo();
    QVERIFY(SessionStore::load(&document, &session, &error));
    QCOMPARE(session.messages.size(), 1);
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().contains(QLatin1String("Clear me")));
}

void AiAssistantTest::legacyWorkspaceMigration()
{
    Okular::Document document(nullptr); QVERIFY(open(document, QStringLiteral("file1.pdf")));
    Session legacy; legacy.ensureWorkspaces();
    auto main = legacy.workspace(QStringLiteral("main")); main.insert(QStringLiteral("title"), QStringLiteral("Paper chat")); legacy.setWorkspace(main);
    legacy.workspaces.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("notes")}, {QStringLiteral("type"), QStringLiteral("scratchpad")}, {QStringLiteral("title"), QStringLiteral("My notes")}, {QStringLiteral("notes"), QStringLiteral("Preserve my research notes")}});
    QString error; QVERIFY(SessionStore::write(&document, legacy, &error));
    AssistantWidget widget(&document, nullptr);
    auto *tabs = widget.findChild<QTabBar *>(QStringLiteral("aiWorkspaceTabs"));
    QCOMPARE(tabs->tabText(0), QStringLiteral("Main chat"));
    tabs->setCurrentIndex(1);
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().contains(QStringLiteral("Preserve my research notes")));
    Session saved; QVERIFY(SessionStore::load(&document, &saved, &error));
    QCOMPARE(saved.workspace(QStringLiteral("notes")).value(QStringLiteral("type")).toString(), QStringLiteral("chat"));
    QVERIFY(!saved.workspace(QStringLiteral("notes")).contains(QStringLiteral("notes")));
    QCOMPARE(saved.messages.size(), 1);
}

void AiAssistantTest::workspaceIsolationAndNotes()
{
    Okular::Document document(nullptr); QVERIFY(open(document, QStringLiteral("file1.pdf")));
    Session original; original.append(QStringLiteral("user"), QStringLiteral("legacy-main-only")); QString error;
    QVERIFY(SessionStore::write(&document, original, &error));
    AssistantWidget widget(&document, nullptr);
    FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost)); configure(widget, server);
    auto *tabs = widget.findChild<QTabBar *>(QStringLiteral("aiWorkspaceTabs"));
    QCOMPARE(tabs->count(), 1);
    auto *plus = widget.findChild<QToolButton *>(QStringLiteral("aiNewWorkspace"));
    plus->click();
    QCOMPARE(tabs->tabText(0), QStringLiteral("Main chat"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("Chat"));
    QCOMPARE(tabs->count(), 2);
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().isEmpty());
    widget.findChild<QPlainTextEdit *>(QStringLiteral("aiQuestion"))->setPlainText(QStringLiteral("private-new-tab"));
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QVERIFY(server.request.contains("private-new-tab")); QVERIFY(!server.request.contains("legacy-main-only"));
    tabs->setCurrentIndex(0);
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().contains(QLatin1String("legacy-main-only")));
    QVERIFY(!widget.findChild<QTextBrowser *>()->toPlainText().contains(QLatin1String("private-new-tab")));
    QMetaObject::invokeMethod(tabs, "tabCloseRequested", Qt::DirectConnection, Q_ARG(int, 1));
    QCOMPARE(tabs->count(), 1);
    Session stored; QVERIFY(SessionStore::load(&document, &stored, &error));
    QVERIFY(stored.workspaces.at(1).toObject().value(QStringLiteral("closed")).toBool()); QCOMPARE(stored.messages.size(), 3);
    QTimer::singleShot(0, &widget, [] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu); auto *restore = menu->actions().at(2)->menu(); QVERIFY(restore); QVERIFY(!restore->actions().isEmpty());
        restore->actions().first()->trigger(); menu->close();
    });
    widget.findChild<QToolButton *>(QStringLiteral("aiWorkspaceMenu"))->click();
    QCOMPARE(tabs->count(), 2);
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().contains(QLatin1String("private-new-tab")));
    plus->click();
    QCOMPARE(tabs->tabText(2), QStringLiteral("Chat"));
    widget.resize(900, 800); widget.show(); QTest::qWait(20);
    QTest::mouseDClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(2).center());
    auto *editor = widget.findChild<QLineEdit *>(QStringLiteral("aiTabNameEditor")); QVERIFY(editor); QVERIFY(editor->isVisible());
    editor->setText(QStringLiteral("My research")); QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(tabs->tabText(2), QStringLiteral("My research"));
    auto *strip = widget.findChild<QWidget *>(QStringLiteral("aiTabStrip")); QVERIFY(strip);
    QVERIFY(plus->x() >= tabs->geometry().right()); QVERIFY(plus->x() - tabs->geometry().right() < 5);
    QTest::mouseClick(strip, Qt::LeftButton, Qt::NoModifier, QPoint(plus->geometry().right() + 20, strip->height()/2));
    QCOMPARE(tabs->count(), 4);
    QVERIFY(!widget.findChild<QToolButton *>(QStringLiteral("aiPinContext")));
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("tabs.pdf"));
    QVERIFY(document.saveChanges(path, &error));
    Okular::Document reopened(nullptr); QVERIFY(open(reopened, path));
    AssistantWidget restored(&reopened, nullptr);
    QCOMPARE(restored.findChild<QTabBar *>(QStringLiteral("aiWorkspaceTabs"))->count(), 4);
    QCOMPARE(restored.findChild<QTabBar *>(QStringLiteral("aiWorkspaceTabs"))->tabText(2), QStringLiteral("My research"));
    if (qEnvironmentVariableIsSet("OKULAIR_READING_PREVIEW")) { restored.resize(650, 850); restored.show(); QTest::qWait(20); QVERIFY(restored.grab().save(QStringLiteral("/tmp/okulair-workspaces-preview.png"))); }

}

void AiAssistantTest::pinnedContextAndSelectionActions()
{
    Okular::Document document(nullptr); QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr); configure(widget, server);
    FocusContext view; view.text = QStringLiteral("fixed-figure-evidence"); view.label = QStringLiteral("Figure view · p. 1");
    view.regions = QJsonArray{QJsonObject{{QStringLiteral("page"), 0}, {QStringLiteral("left"), 0}, {QStringLiteral("top"), 0}, {QStringLiteral("right"), 1}, {QStringLiteral("bottom"), 1}}};
    QImage image(80, 60, QImage::Format_RGB32); image.fill(Qt::red); QBuffer buffer(&view.image); buffer.open(QIODevice::WriteOnly); QVERIFY(image.save(&buffer, "PNG"));
    int captures = 0;
    widget.setContextSource([&](bool selected) { ++captures; auto result = view; if (selected) result.text = QStringLiteral("selection-equation-evidence"); return result; });
    widget.prepareSelection(view, true);
    QCOMPARE(captures, 0);
    view.text = QStringLiteral("different-current-view-do-not-send");
    auto *question = widget.findChild<QPlainTextEdit *>(QStringLiteral("aiQuestion")); question->setPlainText(QStringLiteral("Explain the pinned figure"));
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QCOMPARE(captures, 0); QVERIFY(server.request.contains("fixed-figure-evidence")); QVERIFY(!server.request.contains("different-current-view")); QVERIFY(server.request.contains("data:image/png;base64,"));
    widget.openSelectionChat(true);
    QCOMPARE(captures, 1);
    question->setPlainText(QStringLiteral("Explain this equation step by step."));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QCOMPARE(widget.findChild<QTabBar *>(QStringLiteral("aiWorkspaceTabs"))->count(), 3);
    QTRY_VERIFY(server.request.contains("selection-equation-evidence"));
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QVERIFY(server.request.contains("selection-equation-evidence")); QVERIFY(!server.request.contains("fixed-figure-evidence"));
    widget.findChild<QTabBar *>(QStringLiteral("aiWorkspaceTabs"))->setCurrentIndex(1);
    Session saved; QString error; QVERIFY(SessionStore::load(&document, &saved, &error));
    const auto screenshotChat = saved.activeWorkspace;
    QVERIFY(!saved.workspace(screenshotChat).value(QStringLiteral("images")).toObject().isEmpty());
    widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiClearChat"))->click();
    QVERIFY(SessionStore::load(&document, &saved, &error));
    QVERIFY(!saved.workspace(screenshotChat).contains(QStringLiteral("images")));
    QVERIFY(!saved.workspace(screenshotChat).contains(QStringLiteral("selectionContext")));
    QVERIFY(saved.messages.isEmpty()); // Clear chat removes every chat and resets Main chat.
    QCOMPARE(saved.workspaces.size(), 1);
    document.undo();
    QVERIFY(SessionStore::load(&document, &saved, &error));
    QVERIFY(!saved.workspace(screenshotChat).value(QStringLiteral("images")).toObject().isEmpty());
}

void AiAssistantTest::requestQueueAndCancellation()
{
    Okular::Document document(nullptr); QVERIFY(open(document,QStringLiteral("file1.pdf")));
    FakeEndpoint server; server.hold=true; QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document,nullptr); configure(widget,server);
    FocusContext focus; focus.label=QStringLiteral("Captured view"); focus.text=QStringLiteral("original-view-context");
    widget.setContextSource([&](bool) { return focus; });
    auto *question=widget.findChild<QPlainTextEdit *>(QStringLiteral("aiQuestion"));
    auto *ask=widget.findChild<QPushButton *>(QStringLiteral("aiAsk"));
    question->setPlainText(QStringLiteral("first-request")); answerSharing(); ask->click();
    QTRY_VERIFY(server.request.contains("first-request"));
    QVERIFY(ask->isEnabled());
    widget.findChild<QButtonGroup *>(QStringLiteral("aiScope"))->button(2)->click();
    question->setPlainText(QStringLiteral("second-request")); ask->click();
    auto *requests=widget.findChild<QListWidget *>(QStringLiteral("aiRequestList")); QCOMPARE(requests->count(),2);
    focus.text=QStringLiteral("changed-view-do-not-use");
    question->setPlainText(QStringLiteral("cancelled-third-request")); ask->click(); QCOMPARE(requests->count(),3);
    requests->itemWidget(requests->item(2))->findChild<QToolButton *>()->click(); QCOMPARE(requests->count(),2);
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    server.hold=false;
    widget.findChild<QToolButton *>(QStringLiteral("aiCancelRunningRequest"))->click();
    QTRY_VERIFY(server.request.contains("second-request"));
    QVERIFY(server.request.contains("original-view-context")); QVERIFY(!server.request.contains("changed-view-do-not-use")); QVERIFY(!server.request.contains("cancelled-third-request"));
    QTRY_COMPARE(requests->count(),0);
}

void AiAssistantTest::explainManualHighlight()
{
    QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("manual.pdf")); QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"),path));
    Okular::Document document(nullptr); QVERIFY(open(document,path));
    FakeEndpoint server; server.answer=QStringLiteral("A brief explanation.\nIt preserves your reading position."); QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document,nullptr); configure(widget,server);
    auto *enabled=widget.settingsWidget()->findChild<QCheckBox *>(QStringLiteral("aiAutoExplainHighlights")); QVERIFY(enabled); enabled->setChecked(true);
    document.requestTextPage(0); const auto *page=document.page(0);
    std::unique_ptr<Okular::RegularAreaRect> area(page->findText(99,QStringLiteral("some random text"),Okular::FromTop,Qt::CaseSensitive)); QVERIFY(area && !area->isEmpty());
    auto *annotation=new Okular::HighlightAnnotation; annotation->setAuthor(QStringLiteral("Reader")); annotation->setContents(QStringLiteral("some random text\n")); annotation->setBoundingRectangle(area->first());
    for(const auto &rect:*area) { Okular::HighlightAnnotation::Quad quad; quad.setPoint(Okular::NormalizedPoint(rect.left,rect.bottom),0); quad.setPoint(Okular::NormalizedPoint(rect.right,rect.bottom),1); quad.setPoint(Okular::NormalizedPoint(rect.right,rect.top),2); quad.setPoint(Okular::NormalizedPoint(rect.left,rect.top),3); annotation->highlightQuads().append(quad); }
    const auto viewport=document.viewport(); answerSharing(); document.addPageAnnotation(0,annotation,false);
    QTRY_VERIFY(annotation->contents().contains(QStringLiteral("AI explanation")));
    QVERIFY(!annotation->contents().contains(QStringLiteral("some random text"))); QVERIFY(!annotation->contents().contains(QLatin1Char('\n')));
    QCOMPARE(annotation->author(),QStringLiteral("Reader")); QCOMPARE(document.viewport(),viewport);
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().isEmpty());
    enabled->setChecked(false);
}

void AiAssistantTest::editableCategories()
{
    QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("categories.pdf")); QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"),path));
    Okular::Document document(nullptr); QVERIFY(open(document,path));
    FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost)); AssistantWidget widget(&document,nullptr); configure(widget,server);
    QTimer::singleShot(0,&widget,[] { auto *dialog=QApplication::activeModalWidget(); QVERIFY(dialog); QVERIFY(dialog->findChild<QLineEdit *>(QStringLiteral("aiCategoryName"))); dialog->findChild<QLineEdit *>(QStringLiteral("aiCategoryName"))->setText(QStringLiteral("Limitations")); dialog->findChild<QLineEdit *>(QStringLiteral("aiCategoryDescription"))->setText(QStringLiteral("Study limitations")); dialog->findChild<QSpinBox *>(QStringLiteral("aiCategoryWords"))->setValue(4); dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click(); });
    widget.findChild<QPushButton *>(QStringLiteral("aiAddCategory"))->click();
    QPushButton *category=nullptr;
    for (auto *button:widget.findChildren<QPushButton *>()) if(button->objectName().startsWith(QLatin1String("aiSelectCategory_")) && button->text()==QLatin1String("Limitations")) category=button;
    QVERIFY(category); QCOMPARE(category->text(),QStringLiteral("Limitations")); const auto id=category->property("category").toString();
    QTimer::singleShot(0,&widget,[] { auto *dialog=qobject_cast<QColorDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog); dialog->setCurrentColor(QColor(QStringLiteral("#B47AFF"))); dialog->accept(); });
    widget.findChild<QPushButton *>(QStringLiteral("aiCategoryColor_%1").arg(id))->click();
    server.answer=QString::fromUtf8(QJsonDocument(QJsonArray{QJsonObject{{QStringLiteral("page"),1},{QStringLiteral("category"),id},{QStringLiteral("quote"),QStringLiteral("random")},{QStringLiteral("reason"),QStringLiteral("Relevant caveat\n\nWith a concise explanation")}}}).toJson());
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    QTRY_COMPARE(document.page(0)->annotations().size(),1);
    const auto *annotation=document.page(0)->annotations().first(); QCOMPARE(annotation->style().color(),QColor(QStringLiteral("#B47AFF")));
    QVERIFY(!annotation->contents().contains(QStringLiteral("random"))); QVERIFY(!annotation->contents().contains(QStringLiteral("\\n")));
    QVERIFY(!widget.findChild<QDialog *>(QStringLiteral("aiHighlightPreview")));
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QTimer::singleShot(0,&widget,[] { auto *dialog=QApplication::activeModalWidget(); QVERIFY(dialog); dialog->findChild<QPushButton *>(QStringLiteral("aiDeleteEditedCategory"))->click(); });
    QTest::mouseDClick(widget.findChild<QPushButton *>(QStringLiteral("aiSelectCategory_%1").arg(id)),Qt::LeftButton); QVERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiSelectCategory_%1").arg(id))); QCOMPARE(document.page(0)->annotations().size(),1);
}

void AiAssistantTest::exactEvidenceLinks()
{
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("evidence.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), path));
    Okular::Document document(nullptr); QVERIFY(open(document, path));
    FakeEndpoint server; server.answer = QStringLiteral("A grounded claim [p. 1 | some random text]. An invented claim [p. 1 | made-up-source-quote]. Page-only reference [p. 1]. Grouped reference [pp. 1, 2–3]. Invalid page [p. 999 | some random text]."); QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr); configure(widget, server);
    widget.findChild<QPlainTextEdit *>(QStringLiteral("aiQuestion"))->setPlainText(QStringLiteral("Explain with evidence"));
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    auto *history = widget.findChild<QTextBrowser *>();
    const auto html = history->toHtml();
    const auto match = QRegularExpression(QStringLiteral("href=\"(okulair://evidence/[^\"]+)\"")).match(html);
    QVERIFY(match.hasMatch()); QVERIFY(!history->toPlainText().contains(QLatin1String("quote not located")));
    QVERIFY(!html.contains(QLatin1String("okulair://page/")));
    QCOMPARE(html.count(QLatin1String("okulair://evidence/")), 1);
    QCOMPARE(history->toPlainText().count(QStringLiteral("[p. 1]")), 1);
    QVERIFY(!history->toPlainText().contains(QLatin1String("· evidence")));
    QVERIFY(!history->toPlainText().contains(QLatin1String("[pp.")));
    QVERIFY(!history->toPlainText().contains(QLatin1String("[p. 999")));
    const bool clean = document.isHistoryClean(); const auto annotations = document.page(0)->annotations().size();
    QMetaObject::invokeMethod(history, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(match.captured(1))));
    QCOMPARE(document.currentPage(), 0u); QCOMPARE(document.page(0)->annotations().size(), annotations); QCOMPARE(document.isHistoryClean(), clean);
    QVERIFY(!document.page(0)->textSelection()); // Evidence does not overwrite the user's selection.
    const auto saved = directory.filePath(QStringLiteral("saved.pdf")); QString error;
    QVERIFY(document.saveChanges(saved, &error));
    Okular::Document reopened(nullptr); QVERIFY(open(reopened, saved));
    AssistantWidget restored(&reopened, nullptr);
    const auto restoredHtml = restored.findChild<QTextBrowser *>()->toHtml();
    QCOMPARE(restoredHtml.count(QLatin1String("okulair://evidence/")), 1);
    QVERIFY(!restoredHtml.contains(QLatin1String("okulair://page/")));
    QCOMPARE(restored.findChild<QTextBrowser *>()->toPlainText().count(QStringLiteral("[p. 1]")), 1);
}

void AiAssistantTest::highlightCategoriesAndFilters()
{
    Okular::Document document(nullptr); QVERIFY(open(document, QStringLiteral("file1.pdf")));
    auto *manual = new Okular::HighlightAnnotation; manual->setAuthor(QStringLiteral("Reader")); manual->setBoundingRectangle(Okular::NormalizedRect(.8, .8, .9, .9)); document.addPageAnnotation(0, manual, false);
    FakeEndpoint server;
    server.answer = QStringLiteral("[{\"page\":1,\"category\":\"keyword\",\"quote\":\"random\",\"reason\":\"Scanning cue\"},{\"page\":1,\"category\":\"core\",\"quote\":\"pdf file\",\"reason\":\"Central term\"},{\"page\":1,\"category\":\"explanation\",\"quote\":\"because i thought\",\"reason\":\"Short explanation\"},{\"page\":1,\"category\":\"keyword\",\"quote\":\"This is some random text put here because\",\"reason\":\"Reject long passage\"}]");
    QVERIFY(server.listen(QHostAddress::LocalHost)); AssistantWidget widget(&document, nullptr); configure(widget, server);
    widget.findChild<QComboBox *>(QStringLiteral("aiHighlightDensity"))->setCurrentIndex(1);
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    QTRY_COMPARE(document.page(0)->annotations().size(), 4);
    Okular::Annotation *keyword = nullptr, *core = nullptr, *explanation = nullptr;
    for (auto *annotation : document.page(0)->annotations()) {
        if (annotation->contents().contains(QLatin1String("Category: Keyword"))) keyword = annotation;
        if (annotation->contents().contains(QLatin1String("Category: Core contribution"))) core = annotation;
        if (annotation->contents().contains(QLatin1String("Category: Explanation"))) explanation = annotation;
    }
    QVERIFY(keyword && core && explanation);
    if (qEnvironmentVariableIsSet("OKULAIR_READING_PREVIEW")) { auto *panel = widget.findChild<QDialog *>(QStringLiteral("aiHighlightPanel")); panel->show(); QTest::qWait(20); QVERIFY(panel->grab().save(QStringLiteral("/tmp/okulair-highlights-preview.png"))); }
    QCOMPARE(keyword->style().color(), QColor(QStringLiteral("#F6D86B"))); QCOMPARE(core->style().color(), QColor(QStringLiteral("#F28B82"))); QCOMPARE(explanation->style().color(), QColor(QStringLiteral("#64E0CA")));
    document.setHistoryClean(true);
    auto *filter = widget.findChild<QCheckBox *>(QStringLiteral("aiShowCategory_keyword")); filter->setChecked(false);
    QVERIFY(keyword->isHiddenInView()); QVERIFY(!(keyword->flags() & Okular::Annotation::Hidden)); QVERIFY(!manual->isHiddenInView()); QVERIFY(!core->isHiddenInView()); QVERIFY(document.isHistoryClean());
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("filtered.pdf")); QString error;
    QVERIFY(document.saveChanges(path, &error)); Okular::Document reopened(nullptr); QVERIFY(open(reopened, path));
    QCOMPARE(reopened.page(0)->annotations().size(), 4);
    for (const auto *annotation : reopened.page(0)->annotations()) { QVERIFY(!annotation->isHiddenInView()); QVERIFY(!(annotation->flags() & Okular::Annotation::Hidden)); }
    filter->setChecked(true); QVERIFY(!keyword->isHiddenInView());
    server.answer = QStringLiteral("[{\"page\":1,\"category\":\"keyword\",\"quote\":\"text\",\"reason\":\"Replacement cue\"}]");
    widget.findChild<QCheckBox *>(QStringLiteral("aiShowCategory_core"))->setChecked(false);
    widget.findChild<QCheckBox *>(QStringLiteral("aiShowCategory_explanation"))->setChecked(false);
    widget.findChild<QPushButton *>(QStringLiteral("aiRegenerateHighlights"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->isEnabled() || document.page(0)->annotations().size() == 4);
    QTRY_VERIFY([&] { for (const auto *a : document.page(0)->annotations()) if (a->contents().contains(QLatin1String("Replacement cue"))) return true; return false; }());
    QCOMPARE(document.page(0)->annotations().size(), 4); QVERIFY(document.page(0)->annotations().contains(manual)); QVERIFY(document.page(0)->annotations().contains(core)); QVERIFY(document.page(0)->annotations().contains(explanation));
}

void AiAssistantTest::highlightBatchCoverage()
{
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("five-pages.pdf"));
    {
        QPdfWriter pdf(path); QPainter painter(&pdf);
        for (int page = 1; page <= 5; ++page) { if (page != 1) QVERIFY(pdf.newPage()); painter.drawText(200, 300, QStringLiteral("Distinctive keyword%1 for this page.").arg(page)); }
    }
    Okular::Document document(nullptr); QVERIFY(open(document, path));
    FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost));
    int batches = 0;
    server.responder = [&](const QByteArray &wire) {
        ++batches; QJsonArray suggestions;
        for (int page = 1; page <= 5; ++page) if (wire.contains(QStringLiteral("--- PAGE %1 ---").arg(page).toUtf8())) suggestions.append(QJsonObject{{QStringLiteral("page"), page}, {QStringLiteral("quote"), QStringLiteral("keyword%1").arg(page)}, {QStringLiteral("category"), QStringLiteral("keyword")}, {QStringLiteral("reason"), QStringLiteral("A short cue")}});
        return QString::fromUtf8(QJsonDocument(suggestions).toJson(QJsonDocument::Compact));
    };
    AssistantWidget widget(&document, nullptr); configure(widget, server);
    widget.findChild<QComboBox *>(QStringLiteral("aiHighlightDensity"))->setCurrentIndex(0);
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    QTRY_COMPARE(document.page(4)->annotations().size(), 1); QCOMPARE(batches, 5);
    for (uint page = 0; page < document.pages(); ++page) QCOMPARE(document.page(page)->annotations().size(), 1);
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().isEmpty());
}

void AiAssistantTest::switchingDocumentsCancels()
{
    Okular::Document document(nullptr);
    QVERIFY(open(document, QStringLiteral("file1.pdf")));
    FakeEndpoint server;
    server.hold = true;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr);
    configure(widget, server);
    answerSharing();
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("A question for document one"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!server.request.isEmpty());
    document.closeDocument();
    QVERIFY(open(document, QStringLiteral("file2.pdf")));
    server.request.clear();
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("A question for document two"));
    answerSharing(false);
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QVERIFY(server.request.isEmpty());
    Session session;
    QString error;
    QVERIFY(SessionStore::load(&document, &session, &error));
    QVERIFY(session.messages.isEmpty());
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().isEmpty());
}
QTEST_MAIN(AiAssistantTest)
#include "aiassistanttest.moc"

void AiAssistantTest::independentBackgroundHighlights()
{
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("background.pdf"));
    { QPdfWriter pdf(path); QPainter painter(&pdf);
      for (int page = 1; page <= 3; ++page) { if (page != 1) QVERIFY(pdf.newPage()); painter.drawText(200, 300, QStringLiteral("Distinctive keyword%1 for this page.").arg(page)); } }
    Okular::Document document(nullptr); QVERIFY(open(document, path));
    FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost));
    QPointer<QTcpSocket> held;
    server.defer = [&](const QByteArray &wire) { if (!held && wire.contains("You create reading cues")) { held = server.streamSocket; return true; } return false; };
    int batches = 0;
    server.responder = [&](const QByteArray &wire) {
        if (!wire.contains("You create reading cues")) return QStringLiteral("The selected passage means this.");
        ++batches; QJsonArray cues;
        for (int page = 1; page <= 3; ++page) if (wire.contains(QStringLiteral("--- PAGE %1 ---").arg(page).toUtf8())) cues.append(QJsonObject{{QStringLiteral("page"),page},{QStringLiteral("quote"),QStringLiteral("keyword%1").arg(page)},{QStringLiteral("category"),QStringLiteral("keyword")},{QStringLiteral("reason"),QStringLiteral("Reading cue")}});
        return QString::fromUtf8(QJsonDocument(cues).toJson(QJsonDocument::Compact));
    };
    AssistantWidget widget(&document, nullptr); configure(widget, server); widget.show();
    widget.findChild<QPushButton *>(QStringLiteral("aiHighlights"))->click();
    auto *panel = widget.findChild<QDialog *>(QStringLiteral("aiHighlightPanel")); QVERIFY(panel->isVisible());
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    QTRY_VERIFY(held); QVERIFY(!panel->isVisible());
    widget.setContextSource([](bool) { return FocusContext{QStringLiteral("Selected passage"),QStringLiteral("Selection"),{}, {},QJsonArray{QJsonObject{{QStringLiteral("page"),0},{QStringLiteral("left"),0},{QStringLiteral("top"),0},{QStringLiteral("right"),1},{QStringLiteral("bottom"),1}}}}; });
    widget.openSelectionChat(true);
    widget.findChild<QPlainTextEdit *>(QStringLiteral("aiQuestion"))->setPlainText(QStringLiteral("Explain this selection"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(widget.findChild<QTextBrowser *>()->toPlainText().contains(QLatin1String("The selected passage means this.")));
    QVERIFY(held && held->state() == QAbstractSocket::ConnectedState);
    QVERIFY(widget.findChild<QToolButton *>(QStringLiteral("aiCancelHighlights")));
    QVERIFY(!panel->isVisible());
    server.defer = {};
    server.streamSocket = held;
    server.stream(QStringLiteral("[{\"page\":1,\"quote\":\"keyword1\",\"category\":\"keyword\",\"reason\":\"Reading cue\"}]"), true);
    QTRY_COMPARE(document.page(2)->annotations().size(),1);
    int firstPageHighlights = 0;
    for (const auto *annotation : document.page(0)->annotations()) firstPageHighlights += annotation->subType() == Okular::Annotation::AHighlight;
    QCOMPARE(firstPageHighlights,1); QCOMPARE(document.page(1)->annotations().size(),1);
    QCOMPARE(batches,2);
    QTRY_VERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->isEnabled());
    QVERIFY(!panel->isVisible()); QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().contains(QLatin1String("The selected passage means this.")));
    // Cancelling chat is also independent of the background job, and its own × works.
    server.hold = true; server.request.clear();
    widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    QTRY_VERIFY(!server.request.isEmpty());
    widget.findChild<QPlainTextEdit *>(QStringLiteral("aiQuestion"))->setPlainText(QStringLiteral("Another question"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->click();
    QVERIFY(widget.findChild<QToolButton *>(QStringLiteral("aiCancelHighlights")));
    widget.findChild<QToolButton *>(QStringLiteral("aiCancelHighlights"))->click();
    QTRY_VERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->isEnabled());
}

void AiAssistantTest::copyChatShortcut()
{
    Okular::Document document(nullptr); AssistantWidget widget(&document,nullptr);
    auto *history=widget.findChild<QTextBrowser *>(); history->setPlainText(QStringLiteral("Copy this answer"));
    auto cursor=history->textCursor(); cursor.select(QTextCursor::Document); history->setTextCursor(cursor);
    QShortcut competing(QKeySequence::Copy,&widget); bool stoleCopy=false;
    connect(&competing,&QShortcut::activated,&widget,[&] { stoleCopy=true; });
    widget.show(); widget.activateWindow(); history->setFocus(); QTest::qWait(20);
    QApplication::clipboard()->setText(QStringLiteral("old clipboard"));
    QTest::keyClick(history,Qt::Key_C,Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(),QStringLiteral("Copy this answer")); QVERIFY(!stoleCopy);
}

void AiAssistantTest::apiConnectionError()
{
    Okular::Document document(nullptr); AssistantWidget widget(&document,nullptr);
    auto *service=widget.findChild<QComboBox *>(QStringLiteral("aiService"));
    for(int index: {1,2,4,5,6,7,8,9}) {
        service->setCurrentIndex(index);
        widget.findChild<QLineEdit *>(QStringLiteral("aiModel"))->setText(QStringLiteral("model"));
        widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiConnect"))->click();
        const auto message=widget.findChild<KMessageWidget *>(QStringLiteral("aiStatus"))->text();
        QVERIFY2(message.contains(QLatin1String("API key")),qPrintable(message));
        QVERIFY2(!message.contains(QLatin1String("ChatGPT")),qPrintable(message));
    }
}

void AiAssistantTest::screenshotPromptThumbnail()
{
    QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("screenshot-source.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"),path));
    Okular::Document document(nullptr); QVERIFY(open(document,path));
    FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document,nullptr); configure(widget,server); widget.resize(480,850); widget.show();
    FocusContext capture; capture.text=QStringLiteral("Screenshot document evidence"); capture.label=QStringLiteral("Screenshot · p. 1");
    capture.regions=QJsonArray{QJsonObject{{QStringLiteral("page"),0},{QStringLiteral("left"),0},{QStringLiteral("top"),0},{QStringLiteral("right"),1},{QStringLiteral("bottom"),1}}};
    QImage image(640,320,QImage::Format_RGB32); image.fill(QColor(QStringLiteral("#315B83")));
    { QPainter painter(&image); painter.setPen(Qt::white); painter.drawText(QRect(0,0,640,320),Qt::AlignCenter,QStringLiteral("Captured document region")); }
    QBuffer buffer(&capture.image); buffer.open(QIODevice::WriteOnly); QVERIFY(image.save(&buffer,"PNG"));
    widget.prepareSelection(capture);
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("Explain this screenshot"));
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QVERIFY(server.request.contains("data:image/png;base64,"));
    Session saved; QString error; QVERIFY(SessionStore::load(&document,&saved,&error));
    const auto tab=saved.workspace(saved.activeWorkspace); QVERIFY(!tab.contains(QStringLiteral("selectionContext"))); QCOMPARE(tab.value(QStringLiteral("scope")).toInt(),0);
    const auto context=saved.messages.first().toObject().value(QStringLiteral("context")).toObject();
    const auto reference=context.value(QStringLiteral("imageRef")).toString(); QVERIFY(!reference.isEmpty());
    QVERIFY(tab.value(QStringLiteral("images")).toObject().contains(reference));
    auto verifyThumbnail=[](QTextBrowser *history) {
        int count=0;
        for(auto block=history->document()->begin();block.isValid();block=block.next()) for(auto it=block.begin();!it.atEnd();++it) {
            const auto fragment=it.fragment(); if(!fragment.charFormat().isImageFormat()) continue;
            const auto format=fragment.charFormat().toImageFormat(); QVERIFY(format.name().startsWith(QLatin1String("okulair-image:/")));
            QVERIFY(format.width()<=320); QVERIFY(format.height()<=220);
            const auto resource=history->document()->resource(QTextDocument::ImageResource,QUrl(format.name())); QVERIFY(!resource.value<QImage>().isNull()); ++count;
        }
        QCOMPARE(count,1);
    };
    verifyThumbnail(widget.findChild<QTextBrowser *>());
    if(qEnvironmentVariableIsSet("OKULAIR_SCREENSHOT_CHAT_PREVIEW")) { QTest::qWait(30); QVERIFY(widget.grab().save(QStringLiteral("/tmp/okulair-screenshot-chat.png"))); }
    const auto pdf=directory.filePath(QStringLiteral("saved-chat.pdf")); QVERIFY2(document.saveChanges(pdf,&error),qPrintable(error));
    Okular::Document reopened(nullptr); QVERIFY(open(reopened,pdf)); AssistantWidget restored(&reopened,nullptr);
    verifyThumbnail(restored.findChild<QTextBrowser *>());
    Session reopenedSession; QVERIFY(SessionStore::load(&reopened,&reopenedSession,&error));
    QVERIFY(!reopenedSession.workspace(reopenedSession.activeWorkspace).contains(QStringLiteral("selectionContext")));
}

void AiAssistantTest::consecutiveStreamingAnswers()
{
    QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("streaming.pdf")); QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"),path));
    Okular::Document document(nullptr); QVERIFY(open(document,path));
    FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost)); AssistantWidget widget(&document,nullptr); configure(widget,server);
    widget.resize(480,800); widget.show();
    auto *question=widget.findChild<QPlainTextEdit *>(); auto *history=widget.findChild<QTextBrowser *>();
    server.answer=QStringLiteral("Earlier answer must remain intact.\nA second paragraph.");
    question->setPlainText(QStringLiteral("First question")); answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QVERIFY(history->toPlainText().contains(server.answer));
    server.hold=true; server.request.clear(); question->setPlainText(QStringLiteral("Second question")); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(server.request.contains("Second question"));
    const auto before=history->toPlainText(); QSignalSpy changes(history->document(),&QTextDocument::contentsChange);
    server.stream(QStringLiteral("New answer begins here."));
    QTRY_VERIFY(history->toPlainText().contains(QLatin1String("New answer begins here.")));
    QVERIFY(history->toPlainText().trimmed().startsWith(before.trimmed()));
    server.stream(QStringLiteral(" More text follows smoothly."));
    QTRY_VERIFY(history->toPlainText().contains(QLatin1String("More text follows smoothly.")));
    QVERIFY(history->toPlainText().contains(server.answer));
    for(const auto &change:changes) QVERIFY(change.at(0).toInt() >= before.trimmed().size());
    server.stream(QStringLiteral(" Finished."),true);
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QVERIFY(history->toPlainText().contains(server.answer));
    QCOMPARE(history->toPlainText().count(QStringLiteral("New answer begins here.")),1);
}

void AiAssistantTest::selectExistingHighlightCategories()
{
    KConfigGroup config(KSharedConfig::openConfig(),QStringLiteral("OkulAIrHighlights"));
    const auto previous=config.readEntry("Categories",QByteArray()); const bool had=config.hasKey("Categories");
    const auto restore=qScopeGuard([&] { if(had) config.writeEntry("Categories",previous); else config.deleteEntry("Categories"); config.sync(); });
    config.deleteEntry("Categories"); config.sync();
    QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("edit-categories.pdf")); QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"),path));
    Okular::Document document(nullptr); QVERIFY(open(document,path)); AssistantWidget widget(&document,nullptr);
    QVERIFY(!widget.findChild<QComboBox *>(QStringLiteral("aiHighlightCategory")));
    const auto idForName=[&](const QString &name) { for(auto *button:widget.findChildren<QPushButton *>()) if(button->objectName().startsWith(QLatin1String("aiSelectCategory_")) && button->text()==name) return button->property("category").toString(); return QString(); };
    const auto category=[&](const QString &id) { return widget.findChild<QPushButton *>(QStringLiteral("aiSelectCategory_%1").arg(id)); };
    auto editName=[](const QString &expected,const QString &replacement) {
        QTimer::singleShot(0,qApp,[expected,replacement] {
            auto *dialog=QApplication::activeModalWidget(); QVERIFY(dialog);
            auto *name=dialog->findChild<QLineEdit *>(QStringLiteral("aiCategoryName")); QVERIFY(name); QCOMPARE(name->text(),expected);
            name->setText(replacement); dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
    };
    editName({},QStringLiteral("First custom")); widget.findChild<QPushButton *>(QStringLiteral("aiAddCategory"))->click(); const auto first=idForName(QStringLiteral("First custom"));
    editName({},QStringLiteral("Second custom")); widget.findChild<QPushButton *>(QStringLiteral("aiAddCategory"))->click(); const auto second=idForName(QStringLiteral("Second custom"));
    editName(QStringLiteral("Keyword"),QStringLiteral("Scanning terms")); QTest::mouseDClick(widget.findChild<QPushButton *>(QStringLiteral("aiSelectCategory_keyword")),Qt::LeftButton);
    QCOMPARE(category(QStringLiteral("keyword"))->text(),QStringLiteral("Scanning terms"));
    editName(QStringLiteral("First custom"),QStringLiteral("Renamed first")); QTest::mouseDClick(widget.findChild<QPushButton *>(QStringLiteral("aiSelectCategory_%1").arg(first)),Qt::LeftButton);
    QCOMPARE(category(second)->text(),QStringLiteral("Second custom"));
    QTimer::singleShot(0,&widget,[] { auto *dialog=QApplication::activeModalWidget(); QVERIFY(dialog); dialog->findChild<QPushButton *>(QStringLiteral("aiDeleteEditedCategory"))->click(); });
    QTest::mouseDClick(category(first),Qt::LeftButton); QVERIFY(!category(first)); QVERIFY(category(second));
    QTimer::singleShot(0,qApp,[] { auto *dialog=QApplication::activeModalWidget(); QVERIFY(dialog); QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("aiCategoryName"))->text(),QStringLiteral("Core contribution")); dialog->findChild<QPushButton *>(QStringLiteral("aiDeleteEditedCategory"))->click(); });
    QTest::mouseDClick(widget.findChild<QPushButton *>(QStringLiteral("aiSelectCategory_core")),Qt::LeftButton); QVERIFY(!category(QStringLiteral("core")));
    QVERIFY(category(second)); QVERIFY(category(QStringLiteral("keyword")));
    widget.findChild<QPushButton *>(QStringLiteral("aiResetCategories"))->click();
    QVERIFY(!category(second)); QVERIFY(category(QStringLiteral("keyword"))); QVERIFY(category(QStringLiteral("core"))); QVERIFY(category(QStringLiteral("explanation")));
    QCOMPARE(category(QStringLiteral("keyword"))->text(),QStringLiteral("Keyword"));
    QCOMPARE(widget.findChild<QPushButton *>(QStringLiteral("aiCategoryColor_keyword"))->text(),QStringLiteral("#f6d86b"));
    QVERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiAllCategories")));
    QVERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiEditCategory")));
    QVERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiDeleteCategory")));
    QCOMPARE(widget.findChild<QPushButton *>(QStringLiteral("aiAddCategory"))->text(),QStringLiteral("+"));
    QVERIFY(widget.findChild<QCheckBox *>(QStringLiteral("aiShowCategory_keyword"))->isChecked());
    QVERIFY(!config.hasKey("Categories"));
}

void AiAssistantTest::checkedHighlightCategories()
{
    KConfigGroup config(KSharedConfig::openConfig(),QStringLiteral("OkulAIrHighlights"));
    const auto previous=config.entryMap(); const auto restore=qScopeGuard([&] { config.deleteGroup(); for(auto it=previous.cbegin();it!=previous.cend();++it) config.writeEntry(it.key(),it.value()); config.sync(); });
    QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("checked.pdf")); QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"),path));
    Okular::Document document(nullptr); QVERIFY(open(document,path)); FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document,nullptr); configure(widget,server); widget.findChild<QPushButton *>(QStringLiteral("aiResetCategories"))->click();
    auto *keyword=widget.findChild<QCheckBox *>(QStringLiteral("aiShowCategory_keyword")); auto *core=widget.findChild<QCheckBox *>(QStringLiteral("aiShowCategory_core")); auto *explanation=widget.findChild<QCheckBox *>(QStringLiteral("aiShowCategory_explanation"));
    keyword->setChecked(false); core->setChecked(false); explanation->setChecked(false);
    QVERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->isEnabled());
    QVERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiRegenerateHighlights"))->isEnabled());
    widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click(); QVERIFY(server.request.isEmpty());
    core->setChecked(true); explanation->setChecked(true);
    const auto suggestions=[](const QString &reason) { return QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{{QStringLiteral("page"),1},{QStringLiteral("category"),QStringLiteral("keyword")},{QStringLiteral("quote"),QStringLiteral("some")},{QStringLiteral("reason"),reason}},
        QJsonObject{{QStringLiteral("page"),1},{QStringLiteral("category"),QStringLiteral("core")},{QStringLiteral("quote"),QStringLiteral("random")},{QStringLiteral("reason"),reason}},
        QJsonObject{{QStringLiteral("page"),1},{QStringLiteral("category"),QStringLiteral("explanation")},{QStringLiteral("quote"),QStringLiteral("text")},{QStringLiteral("reason"),reason}}}).toJson()); };
    server.answer=suggestions(QStringLiteral("Initial checked cue")); answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    QTRY_COMPARE(document.page(0)->annotations().size(),2);
    QVERIFY(server.request.contains("core (Core contribution)")); QVERIFY(server.request.contains("explanation (Explanation)")); QVERIFY(!server.request.contains("keyword (Keyword)"));
    for(const auto *annotation:document.page(0)->annotations()) QVERIFY(!annotation->uniqueName().contains(QLatin1String(":keyword:")));
    auto *unchecked=new Okular::HighlightAnnotation; unchecked->setAuthor(QStringLiteral("AI assistant · fixture")); unchecked->setUniqueName(QStringLiteral("okulair-ai:keyword:kept")); unchecked->setBoundingRectangle(Okular::NormalizedRect(.8,.8,.85,.85)); document.addPageAnnotation(0,unchecked,false);
    auto *manual=new Okular::HighlightAnnotation; manual->setAuthor(QStringLiteral("Reader")); manual->setBoundingRectangle(Okular::NormalizedRect(.9,.9,.95,.95)); document.addPageAnnotation(0,manual,false);
    server.answer=suggestions(QStringLiteral("Regenerated checked cue")); widget.findChild<QPushButton *>(QStringLiteral("aiRegenerateHighlights"))->click();
    QTRY_VERIFY([&] { int replaced=0; for(const auto *annotation:document.page(0)->annotations()) replaced+=annotation->contents().contains(QLatin1String("Regenerated checked cue")); return replaced==2; }());
    QCOMPARE(document.page(0)->annotations().size(),4); QVERIFY(document.page(0)->annotations().contains(unchecked)); QVERIFY(document.page(0)->annotations().contains(manual));
    QVERIFY(!QApplication::activeModalWidget());
}

void AiAssistantTest::thinkingEffortUi()
{
    QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("effort.pdf")); QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"),path));
    Okular::Document document(nullptr); QVERIFY(open(document,path)); FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost));
    server.models=QJsonArray{QJsonObject{{QStringLiteral("id"),QStringLiteral("reasoning-test")},{QStringLiteral("default_reasoning_level"),QStringLiteral("medium")},{QStringLiteral("supported_reasoning_efforts"),QJsonArray{QStringLiteral("none"),QStringLiteral("medium"),QStringLiteral("high")}}}};
    AssistantWidget widget(&document,nullptr); configure(widget,server);
    widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiConnect"))->click();
    auto *model=widget.findChild<QComboBox *>(QStringLiteral("aiModelPicker")); auto *effort=widget.findChild<QComboBox *>(QStringLiteral("aiThinkingEffort")); QVERIFY(effort);
    QTRY_COMPARE(model->currentData().toString(),QStringLiteral("reasoning-test")); QVERIFY(effort->isEnabled()); QCOMPARE(effort->count(),4); QCOMPARE(effort->itemText(0),QStringLiteral("Default (medium)"));
    effort->setCurrentIndex(effort->findData(QStringLiteral("high"))); QMetaObject::invokeMethod(effort,"activated",Q_ARG(int,effort->currentIndex()));
    widget.findChild<QLineEdit *>(QStringLiteral("aiModel"))->setText(QStringLiteral("unknown")); QVERIFY(!effort->isEnabled()); QCOMPARE(effort->currentData().toString(),QString());
    model->setCurrentIndex(0); QTRY_COMPARE(effort->currentData().toString(),QStringLiteral("high"));
    server.hold=true; server.request.clear(); auto *question=widget.findChild<QPlainTextEdit *>(); question->setPlainText(QStringLiteral("First effort question")); answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(server.request.contains("First effort question")); QVERIFY(server.request.contains("\"reasoning_effort\":\"high\""));
    effort->setCurrentIndex(effort->findData(QStringLiteral("medium"))); question->setPlainText(QStringLiteral("Queued effort question")); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    effort->setCurrentIndex(effort->findData(QStringLiteral("none"))); server.hold=false; server.stream(QStringLiteral("First answer"),true);
    QTRY_VERIFY(server.request.contains("Queued effort question")); QVERIFY(server.request.contains("\"reasoning_effort\":\"medium\""));
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    Session saved; QString error; QVERIFY(SessionStore::load(&document,&saved,&error)); QCOMPARE(saved.messages.last().toObject().value(QStringLiteral("reasoningEffort")).toString(),QStringLiteral("medium"));
    QVERIFY(widget.findChild<QTextBrowser *>()->toPlainText().contains(QLatin1String("Thinking: medium")));
}

void AiAssistantTest::removedScreenshotStaysRemoved()
{
    QTemporaryDir directory;
    const auto input = directory.filePath(QStringLiteral("input.pdf"));
    const auto output = directory.filePath(QStringLiteral("output.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), input));
    Okular::Document document(nullptr); QVERIFY(open(document, input));
    {
        AssistantWidget widget(&document, nullptr);
        auto *question = widget.findChild<QPlainTextEdit *>(QStringLiteral("aiQuestion"));
        QImage image(100, 100, QImage::Format_RGB32); image.fill(Qt::green);
        QApplication::clipboard()->setImage(image);
        QTest::keyClick(question, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiAttachment0")));
        // Tab changes persist the composer, reproducing the previously stale draft.
        auto *tabs = widget.findChild<QTabBar *>(); QVERIFY(tabs);
        widget.findChild<QToolButton *>(QStringLiteral("aiNewWorkspace"))->click();
        tabs->setCurrentIndex(0);
        Session saved; QString error; QVERIFY(SessionStore::load(&document, &saved, &error));
        QCOMPARE(saved.workspace(QStringLiteral("main")).value(QStringLiteral("attachments")).toArray().size(), 1);
        widget.findChild<QPushButton *>(QStringLiteral("aiAttachment0"))->click();
        QVERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiAttachment0")));
        QVERIFY(SessionStore::load(&document, &saved, &error));
        QVERIFY(saved.workspace(QStringLiteral("main")).value(QStringLiteral("attachments")).toArray().isEmpty());
        FocusContext capture; capture.label = QStringLiteral("Screenshot · p. 1");
        QBuffer png(&capture.image); QVERIFY(png.open(QIODevice::WriteOnly)); QVERIFY(image.save(&png, "PNG")); png.close();
        widget.prepareSelection(capture);
        QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiSelectionContext")));
        widget.findChild<QPushButton *>(QStringLiteral("aiSelectionContext"))->click();
        QVERIFY(SessionStore::load(&document, &saved, &error));
        QVERIFY(!saved.workspace(QStringLiteral("main")).contains(QStringLiteral("selectionContext")));
        QVERIFY(saved.workspace(QStringLiteral("main")).value(QStringLiteral("images")).toObject().isEmpty());
        QVERIFY2(document.saveChanges(output, &error), qPrintable(error));
    }
    document.closeDocument(); QVERIFY(open(document, output));
    AssistantWidget reopened(&document, nullptr);
    QVERIFY(!reopened.findChild<QPushButton *>(QStringLiteral("aiAttachment0")));
    QVERIFY(!reopened.findChild<QPushButton *>(QStringLiteral("aiSelectionContext")));
}

void AiAssistantTest::deleteOtherChats()
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("chats.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), path));
    Okular::Document document(nullptr); QVERIFY(open(document, path));
    Session session; session.ensureWorkspaces(); session.append(QStringLiteral("user"), QStringLiteral("Keep Main history"));
    auto other = session.workspace(QStringLiteral("main"));
    other.insert(QStringLiteral("id"), QStringLiteral("other")); other.insert(QStringLiteral("title"), QStringLiteral("Other"));
    other.insert(QStringLiteral("closed"), true); session.workspaces.append(other);
    session.append(QStringLiteral("user"), QStringLiteral("Delete other history"));
    auto message = session.messages.last().toObject(); message.insert(QStringLiteral("workspace"),QStringLiteral("other")); session.messages[session.messages.size()-1] = message;
    QString error; QVERIFY(SessionStore::write(&document, session, &error));
    AssistantWidget widget(&document, nullptr);
    widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiDeleteOtherChats"))->click();
    QVERIFY(SessionStore::load(&document, &session, &error));
    QCOMPARE(session.workspaces.size(), 1); QCOMPARE(session.messages.size(), 1);
    QCOMPARE(session.messages.first().toObject().value(QStringLiteral("content")).toString(), QStringLiteral("Keep Main history"));
    widget.settingsWidget()->findChild<QPushButton *>(QStringLiteral("aiClearChat"))->click();
    QVERIFY(SessionStore::load(&document, &session, &error));
    QCOMPARE(session.workspaces.size(), 1); QVERIFY(session.messages.isEmpty());
    const auto saved = directory.filePath(QStringLiteral("saved.pdf"));
    QVERIFY(document.saveChanges(saved, &error));
    Okular::Document reopened(nullptr); QVERIFY(open(reopened, saved));
    QVERIFY(SessionStore::load(&reopened, &session, &error));
    QCOMPARE(session.workspaces.size(), 1); QVERIFY(session.messages.isEmpty());
}

void AiAssistantTest::requestPopupKeepsLayout()
{
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("popup.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), path));
    Okular::Document document(nullptr); QVERIFY(open(document, path));
    FakeEndpoint server; server.hold = true; QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr); configure(widget, server); widget.resize(540,800); widget.show();
    auto *history = widget.findChild<QTextBrowser *>();
    auto *question = widget.findChild<QPlainTextEdit *>();
    auto *activity = widget.findChild<QToolButton *>(QStringLiteral("aiRequestActivity")); QVERIFY(activity);
    auto *popup = widget.findChild<QWidget *>(QStringLiteral("aiRequests")); QVERIFY(popup);
    QTest::qWait(30);
    const auto bounds = history->geometry(); const auto composer = question->geometry(); const auto button = activity->geometry();
    QVERIFY(popup->isWindow()); QVERIFY(!popup->isVisible());
    question->setPlainText(QStringLiteral("First popup request")); answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(server.request.contains("First popup request")); QTRY_VERIFY(activity->text().contains(QLatin1String("1")));
    QCOMPARE(history->geometry(), bounds); QCOMPARE(question->geometry(), composer); QCOMPARE(activity->geometry(), button);
    QVERIFY(!popup->isVisible());
    activity->click(); QTRY_VERIFY(popup->isVisible());
    if (qEnvironmentVariableIsSet("OKULAIR_CHAT_POLISH_PREVIEW")) { QVERIFY(widget.grab().save(QStringLiteral("/tmp/okulair-activity-running.png"))); QVERIFY(popup->grab().save(QStringLiteral("/tmp/okulair-activity-popup.png"))); }
    QCOMPARE(history->geometry(), bounds); QCOMPARE(question->geometry(), composer);
    QTest::keyClick(popup, Qt::Key_Escape); QTRY_VERIFY(!popup->isVisible());
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    question->setPlainText(QStringLiteral("Queued popup request")); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QCOMPARE(widget.findChild<QListWidget *>(QStringLiteral("aiRequestList"))->count(), 2);
    activity->click(); QTRY_VERIFY(popup->isVisible());
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("aiRequestList"));
    list->itemWidget(list->item(1))->findChild<QToolButton *>()->click(); QCOMPARE(list->count(), 1);
    widget.findChild<QToolButton *>(QStringLiteral("aiCancelRunningRequest"))->click();
    QTRY_COMPARE(list->count(), 0); QVERIFY(!popup->isVisible());
    QTRY_COMPARE(history->geometry(), bounds); QCOMPARE(question->geometry(), composer); QCOMPARE(activity->geometry(), button);
    if (qEnvironmentVariableIsSet("OKULAIR_CHAT_POLISH_PREVIEW")) QVERIFY(widget.grab().save(QStringLiteral("/tmp/okulair-activity-preview.png")));
}

void AiAssistantTest::progressivelyFormattedAnswer()
{
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("formatted.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), path));
    Okular::Document document(nullptr); QVERIFY(open(document, path));
    FakeEndpoint server; server.hold = true; QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr); configure(widget, server); widget.resize(540,800); widget.show();
    auto *history = widget.findChild<QTextBrowser *>(); auto *question = widget.findChild<QPlainTextEdit *>();
    question->setPlainText(QStringLiteral("Format this answer")); answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(server.request.contains("Format this answer"));
    server.stream(QStringLiteral("## Main goal\n\n- **Understand"));
    QTRY_VERIFY(history->toPlainText().contains(QLatin1String("Understand")));
    QVERIFY(!history->toPlainText().contains(QLatin1String("##"))); QVERIFY(!history->toPlainText().contains(QLatin1String("**")));
    QTextCursor bold = history->document()->find(QStringLiteral("Understand")); QVERIFY(!bold.isNull()); QVERIFY(bold.charFormat().fontWeight() >= QFont::Bold);
    QTextCursor heading = history->document()->find(QStringLiteral("Main goal")); QVERIFY(heading.charFormat().fontWeight() >= QFont::Bold);
    server.stream(QStringLiteral(" the method**\n\n```python\nprint('safe')\n```\n<img src=\"https://example.com/private.png\">"));
    QTRY_VERIFY(history->toPlainText().contains(QLatin1String("print('safe')")));
    QVERIFY(history->toPlainText().contains(QLatin1String("<img src="))); // Provider HTML is always literal.
    QVERIFY(!history->toHtml().contains(QLatin1String("<img")));
    const auto before = history->toPlainText();
    server.stream(QString(), true);
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    const auto after = history->toPlainText();
    QCOMPARE(after.mid(after.indexOf(QStringLiteral("Main goal"))).trimmed(), before.mid(before.indexOf(QStringLiteral("Main goal"))).trimmed());
    if (qEnvironmentVariableIsSet("OKULAIR_CHAT_POLISH_PREVIEW")) QVERIFY(widget.grab().save(QStringLiteral("/tmp/okulair-formatted-preview.png")));
}

void AiAssistantTest::cancelledHighlightStatus()
{
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("cancel-status.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), path));
    Okular::Document document(nullptr); QVERIFY(open(document, path));
    FakeEndpoint server; server.hold = true; QVERIFY(server.listen(QHostAddress::LocalHost));
    AssistantWidget widget(&document, nullptr); configure(widget, server);
    auto *status = widget.findChild<KMessageWidget *>(QStringLiteral("aiStatus"));
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    QTRY_VERIFY(!server.request.isEmpty());
    QVERIFY(status->text().contains(QLatin1String("Highlighting in background")));
    widget.findChild<QToolButton *>(QStringLiteral("aiCancelHighlights"))->click();
    QVERIFY(status->text().contains(QLatin1String("Highlighting cancelled")));
    QVERIFY(!status->text().contains(QLatin1String("Highlighting in background")));
    QCOMPARE(widget.findChild<QListWidget *>(QStringLiteral("aiRequestList"))->count(), 0);
    QTest::qWait(100);
    QVERIFY(status->text().contains(QLatin1String("Highlighting cancelled")));
    // A cancellation must not claim that an independent chat request stopped.
    server.request.clear(); widget.findChild<QPushButton *>(QStringLiteral("aiGenerateHighlights"))->click();
    QTRY_VERIFY(!server.request.isEmpty());
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("Keep this chat running"));
    widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(server.request.contains("Keep this chat running"));
    widget.findChild<QToolButton *>(QStringLiteral("aiCancelHighlights"))->click();
    QVERIFY(status->text().contains(QLatin1String("current AI request continues")));
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    QCOMPARE(widget.findChild<QListWidget *>(QStringLiteral("aiRequestList"))->count(), 1);
    widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->click();
}

void AiAssistantTest::groupedEvidenceLinks()
{
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("grouped.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), path));
    Okular::Document document(nullptr); QVERIFY(open(document, path));
    FakeEndpoint server; QVERIFY(server.listen(QHostAddress::LocalHost));
    server.answer = QStringLiteral("Two passages [p. 1 | some random text][p. 1 | pdf file][p. 1 | some random text]. Another claim [p. 1 | some random text].");
    AssistantWidget widget(&document, nullptr); configure(widget, server);
    widget.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("Cite two passages"));
    answerSharing(); widget.findChild<QPushButton *>(QStringLiteral("aiAsk"))->click();
    QTRY_VERIFY(!widget.findChild<QPushButton *>(QStringLiteral("aiStop"))->isEnabled());
    auto *history = widget.findChild<QTextBrowser *>();
    QVERIFY(history->toPlainText().contains(QStringLiteral("[p. 1: 1, 2]")));
    QCOMPARE(history->toPlainText().count(QStringLiteral("[p. 1]")), 1); // Separate claims keep their own anchor.
    QVERIFY(!history->toPlainText().contains(QStringLiteral("[p. 1][p. 1]")));
    QCOMPARE(history->toHtml().count(QStringLiteral("okulair://evidence/")), 3); // Adjacent exact duplicates collapse.
    auto links = QRegularExpression(QStringLiteral("href=\"(okulair://evidence/[^\"]+)\"")).globalMatch(history->toHtml());
    QStringList targets;
    while (links.hasNext()) targets.append(links.next().captured(1));
    QCOMPARE(targets.size(), 3); QVERIFY(targets.at(0) != targets.at(1)); QCOMPARE(targets.at(0), targets.at(2));
    for (const auto &target : targets) QMetaObject::invokeMethod(history, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(target)));
    const auto saved = directory.filePath(QStringLiteral("saved.pdf")); QString error;
    QVERIFY(document.saveChanges(saved, &error));
    Okular::Document reopened(nullptr); QVERIFY(open(reopened, saved));
    AssistantWidget restored(&reopened, nullptr);
    QVERIFY(restored.findChild<QTextBrowser *>()->toPlainText().contains(QStringLiteral("[p. 1: 1, 2]")));
}
