/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "assistantwidget.h"
#include "core/document.h"
#include <KLocalizedString>
#include <QTabBar>
#include <QPushButton>
#include <QToolButton>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QButtonGroup>
#include <QMenu>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QImage>
#include <QBuffer>
#include <QCryptographicHash>
#include <QUuid>
using namespace OkulAIr;
namespace {
class RenameEditor : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    bool cancelled = false;
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) { cancelled = true; hide(); return; }
        QLineEdit::keyPressEvent(event);
    }
};
class ChatTabStrip : public QWidget {
public:
    QTabBar *tabs;
    QToolButton *add, *menu;
    RenameEditor *editor;
    std::function<void()> create;
    std::function<void(const QString &, const QString &)> rename;
    bool editing = false;
    explicit ChatTabStrip(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("aiTabStrip"));
        tabs = new QTabBar(this); tabs->setObjectName(QStringLiteral("aiWorkspaceTabs"));
        tabs->setTabsClosable(true); tabs->setExpanding(false); tabs->setUsesScrollButtons(true); tabs->setElideMode(Qt::ElideRight);
        add = new QToolButton(this); add->setText(QStringLiteral("+")); add->setObjectName(QStringLiteral("aiNewWorkspace")); add->setToolTip(i18n("New chat"));
        menu = new QToolButton(this); menu->setText(QStringLiteral("⋮")); menu->setObjectName(QStringLiteral("aiWorkspaceMenu")); menu->setToolTip(i18n("Rename, close, restore or delete chats"));
        editor = new RenameEditor(tabs); editor->setObjectName(QStringLiteral("aiTabNameEditor")); editor->setMaxLength(80); editor->hide();
        connect(add, &QToolButton::clicked, this, [this] { if (create) create(); });
        connect(editor, &QLineEdit::editingFinished, this, [this] {
            if (!editing) return;
            editing = false; const auto id = editor->property("chatId").toString(); const auto title = editor->text().simplified();
            editor->hide(); if (!editor->cancelled && !title.isEmpty() && rename) rename(id, title);
        });
        connect(tabs, &QTabBar::tabBarDoubleClicked, this, [this](int index) { beginRename(index); });
    }
    void sync() {
        const int h = qMax(tabs->sizeHint().height(), qMax(add->sizeHint().height(), menu->sizeHint().height()));
        setFixedHeight(h);
        const int addWidth = add->sizeHint().width(), menuWidth = menu->sizeHint().width();
        const int barWidth = qMin(tabs->sizeHint().width(), qMax(0, width() - addWidth - menuWidth - 6));
        tabs->setGeometry(0, 0, barWidth, h); add->setGeometry(barWidth + 2, 0, addWidth, h); menu->setGeometry(width() - menuWidth, 0, menuWidth, h);
    }
    void beginRename(int index) {
        if (index < 0 || !tabs->isEnabled()) return;
        editor->cancelled = false; editing = true;
        editor->setProperty("chatId", tabs->tabData(index)); editor->setText(tabs->tabText(index));
        editor->setGeometry(tabs->tabRect(index).adjusted(2, 2, -2, -2)); editor->show(); editor->raise(); editor->setFocus(); editor->selectAll();
    }
    void resizeEvent(QResizeEvent *event) override { QWidget::resizeEvent(event); sync(); }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && event->position().x() >= tabs->geometry().right() && event->position().x() < menu->x()) {
            if (create) { create(); }
            event->accept(); return;
        }
        QWidget::mouseReleaseEvent(event);
    }
};
}

void AssistantWidget::initWorkspaces()
{
    auto *strip = new ChatTabStrip(this); m_tabStrip = strip;
    m_tabs = strip->tabs; m_newWorkspace = strip->add; m_workspaceActions = strip->menu;
    strip->create = [this] { createWorkspace(); };
    strip->rename = [this](const QString &id, const QString &title) {
        if (m_readOnly || m_changingWorkspace) return;
        const auto old = m_session; snapshotWorkspace(); auto tab = m_session.workspace(id); if (tab.isEmpty()) return;
        tab.insert(QStringLiteral("title"), title); m_session.setWorkspace(tab);
        if (!persist()) m_session = old;
        loadWorkspaceUi();
    };
    connect(m_workspaceActions, &QToolButton::clicked, this, &AssistantWidget::workspaceMenu);
    static_cast<QVBoxLayout *>(layout())->insertWidget(0, strip);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) { if (!m_changingWorkspace && index >= 0) switchWorkspace(m_tabs->tabData(index).toString()); });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, [this](int index) { closeWorkspace(m_tabs->tabData(index).toString()); });
    connect(m_scope, &QButtonGroup::idClicked, this, [this](int scope) {
        auto tab = m_session.workspace(m_workspace);
        if (scope != 1) tab.remove(QStringLiteral("selectionContext"));
        tab.insert(QStringLiteral("scope"), scope); m_session.setWorkspace(tab); updateAttachments();
    });
}

bool AssistantWidget::messageInWorkspace(const QJsonObject &message) const { return Session::workspaceOf(message) == m_workspace; }

void AssistantWidget::snapshotWorkspace()
{
    auto tab = m_session.workspace(m_workspace);
    tab.insert(QStringLiteral("draft"), m_question->toPlainText()); tab.insert(QStringLiteral("attachments"), m_attachments);
    tab.insert(QStringLiteral("scope"), m_scope->checkedId()); m_session.setWorkspace(tab);
}

void AssistantWidget::loadWorkspaceUi()
{
    const QScopedValueRollback<bool> guard(m_changingWorkspace, true);
    // Migrate old special tabs without discarding notes, images or conversations.
    for (qsizetype i = 0; i < m_session.workspaces.size(); ++i) {
        auto tab = m_session.workspaces.at(i).toObject(); const auto id = tab.value(QStringLiteral("id")).toString();
        if (id == QLatin1String("main") && tab.value(QStringLiteral("title")) == QLatin1String("Paper chat")) tab.insert(QStringLiteral("title"), i18n("Main chat"));
        if (tab.value(QStringLiteral("type")) == QLatin1String("scratchpad")) {
            const auto notes = tab.take(QStringLiteral("notes")).toString();
            if (!notes.isEmpty()) { m_session.append(QStringLiteral("user"), i18n("Saved notes:\n\n%1", notes), {}, QStringLiteral("notes")); auto message = m_session.messages.last().toObject(); message.insert(QStringLiteral("workspace"), id); m_session.messages[m_session.messages.size() - 1] = message; }
        }
        tab.insert(QStringLiteral("type"), QStringLiteral("chat"));
        if (tab.value(QStringLiteral("pinned")).toBool()) {
            auto context = tab.value(QStringLiteral("context")).toObject(); context.insert(QStringLiteral("mode"), 1);
            const auto image = context.take(QStringLiteral("image")).toString();
            if (!image.isEmpty()) { const auto key = QString::fromLatin1(QCryptographicHash::hash(image.toLatin1(), QCryptographicHash::Sha256).toHex()); auto assets = tab.value(QStringLiteral("images")).toObject(); assets.insert(key, image); tab.insert(QStringLiteral("images"), assets); context.insert(QStringLiteral("imageRef"), key); }
            tab.insert(QStringLiteral("selectionContext"), context); tab.insert(QStringLiteral("scope"), 1);
        }
        tab.remove(QStringLiteral("pinned")); tab.remove(QStringLiteral("context"));
        m_session.workspaces[i] = tab;
    }
    m_workspace = m_session.activeWorkspace;
    auto tab = m_session.workspace(m_workspace);
    if (tab.isEmpty() || tab.value(QStringLiteral("closed")).toBool()) { m_workspace = QStringLiteral("main"); m_session.activeWorkspace = m_workspace; }
    auto *strip = static_cast<ChatTabStrip *>(m_tabStrip); strip->editing = false; strip->editor->hide();
    while (m_tabs->count()) m_tabs->removeTab(0);
    for (const auto &entry : m_session.workspaces) {
        tab = entry.toObject(); if (tab.value(QStringLiteral("closed")).toBool()) continue;
        const int i = m_tabs->addTab(tab.value(QStringLiteral("title")).toString()); const auto id = tab.value(QStringLiteral("id")).toString(); m_tabs->setTabData(i, id);
        if (id == QLatin1String("main")) { m_tabs->setTabButton(i, QTabBar::RightSide, nullptr); m_tabs->setTabButton(i, QTabBar::LeftSide, nullptr); }
        if (id == m_workspace) m_tabs->setCurrentIndex(i);
    }
    strip->sync(); tab = m_session.workspace(m_workspace);
    m_scope->button(qBound(0, tab.value(QStringLiteral("scope")).toInt(0), 2))->setChecked(true);
    m_question->setPlainText(tab.value(QStringLiteral("draft")).toString()); m_attachments = tab.value(QStringLiteral("attachments")).toArray(); updateAttachments();
    updateWorkspaceControls();
}

void AssistantWidget::updateWorkspaceControls()
{
    m_scopeBar->setEnabled(!m_readOnly); m_summary->setEnabled(!m_readOnly);
    m_newWorkspace->setEnabled(!m_readOnly);
}

void AssistantWidget::switchWorkspace(const QString &id)
{
    if (id == m_workspace || m_session.workspace(id).isEmpty()) return;
    if (m_busy) { stop(); } m_resumeQuestion.clear(); ++m_generation;
    const auto old = m_session; snapshotWorkspace(); m_session.activeWorkspace = id;
    if (!persist()) { m_session = old; loadWorkspaceUi(); return; }
    m_partial.clear(); m_followOutput = true; loadWorkspaceUi(); render();
}

void AssistantWidget::createWorkspace()
{
    if (m_readOnly || !m_document->pages()) { showError(i18n("Open a writable PDF first.")); return; }
    if (m_session.workspaces.size() >= 32) { showError(i18n("This PDF has 32 chats, including closed chats. Delete an unused chat first.")); return; }
    if (m_busy) { stop(); } ++m_generation; m_resumeQuestion.clear();
    const auto old = m_session; snapshotWorkspace(); const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_session.workspaces.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("chat")}, {QStringLiteral("title"), i18n("Chat")}}); m_session.activeWorkspace = id;
    if (!persist()) { m_session = old; return; }
    m_partial.clear(); m_followOutput = true; loadWorkspaceUi(); render(); m_question->setFocus();
}

FocusContext AssistantWidget::workspaceContext() const
{
    const auto tab = m_session.workspace(m_workspace); const auto context = tab.value(QStringLiteral("selectionContext")).toObject();
    const auto image = tab.value(QStringLiteral("images")).toObject().value(context.value(QStringLiteral("imageRef")).toString()).toString();
    return {context.value(QStringLiteral("text")).toString(), context.value(QStringLiteral("label")).toString(), {}, QByteArray::fromBase64(image.toLatin1()), context.value(QStringLiteral("regions")).toArray()};
}

void AssistantWidget::openSelectionChat(bool newChat)
{
    if (!m_contextSource) { showError(i18n("No PDF selection is available.")); return; }
    prepareSelection(m_contextSource(true), newChat);
}

void AssistantWidget::prepareSelection(FocusContext context, bool newChat)
{
    if (m_readOnly || !m_document->isAllowed(Okular::AllowCopy)) { showError(i18n("This document does not permit copying content for AI assistance.")); return; }
    if (!context.error.isEmpty()) { showError(context.error); return; }
    if (context.text.trimmed().isEmpty() && context.image.isEmpty()) { showError(i18n("Select text or capture a screenshot first.")); return; }
    if (context.text.size() > 80000) { showError(i18n("Choose a smaller selection.")); return; }
    if (m_busy) { stop(); }
    if (newChat) { const auto previous = m_workspace; createWorkspace(); if (m_workspace == previous) return; }
    ++m_generation; m_resumeQuestion.clear(); const auto old = m_session; snapshotWorkspace(); auto tab = m_session.workspace(m_workspace);
    QJsonObject selected{{QStringLiteral("mode"), 1}, {QStringLiteral("text"), context.text}, {QStringLiteral("label"), context.label}, {QStringLiteral("regions"), context.regions}};
    if (!context.image.isEmpty()) {
        auto image = QImage::fromData(context.image, "PNG"); if (image.isNull()) { showError(i18n("Could not read the selected screenshot.")); return; }
        if (qMax(image.width(), image.height()) > 1600) image = image.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        while (true) {
            context.image.clear(); QBuffer buffer(&context.image); buffer.open(QIODevice::WriteOnly);
            if (!image.save(&buffer, "PNG")) { showError(i18n("Could not prepare the selected screenshot.")); return; }
            if (context.image.size() <= Session::MaximumImageBytes) break;
            if (qMax(image.width(), image.height()) <= 400) { showError(i18n("This screenshot exceeds the 4 MiB image limit after resizing. Choose a smaller region.")); return; }
            image = image.scaled(qMax(1, image.width() * 3 / 4), qMax(1, image.height() * 3 / 4), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        const auto key = QString::fromLatin1(QCryptographicHash::hash(context.image, QCryptographicHash::Sha256).toHex()); auto assets = tab.value(QStringLiteral("images")).toObject(); assets.insert(key, QString::fromLatin1(context.image.toBase64())); tab.insert(QStringLiteral("images"), assets); selected.insert(QStringLiteral("imageRef"), key);
    }
    tab.insert(QStringLiteral("selectionContext"), selected); tab.insert(QStringLiteral("scope"), 1); m_session.setWorkspace(tab);
    if (!persist()) { m_session = old; return; }
    m_scope->button(1)->setChecked(true); updateAttachments(); showChat();
    m_question->setPlaceholderText(i18n("Ask about %1…\nEnter to send · Shift+Enter for a new line", context.label));
}

void AssistantWidget::closeWorkspace(const QString &id, bool erase)
{
    if (id == QLatin1String("main") || m_readOnly) return;
    if (m_busy) { stop(); } ++m_generation; m_resumeQuestion.clear(); const auto old = m_session; snapshotWorkspace();
    if (erase) {
        QJsonArray tabs, messages; for (const auto &entry : m_session.workspaces) if (entry.toObject().value(QStringLiteral("id")) != id) tabs.append(entry);
        for (const auto &entry : m_session.messages) if (Session::workspaceOf(entry.toObject()) != id) messages.append(entry);
        m_session.workspaces = tabs; m_session.messages = messages;
    } else { auto tab = m_session.workspace(id); tab.insert(QStringLiteral("closed"), true); m_session.setWorkspace(tab); }
    if (id == m_workspace) m_session.activeWorkspace = QStringLiteral("main");
    if (!persist()) { m_session = old; return; } loadWorkspaceUi(); render();
}

void AssistantWidget::workspaceMenu()
{
    QMenu menu(this);
    auto *rename = menu.addAction(i18n("Rename chat")); rename->setEnabled(!m_readOnly);
    connect(rename, &QAction::triggered, this, [this] { static_cast<ChatTabStrip *>(m_tabStrip)->beginRename(m_tabs->currentIndex()); });
    auto *close = menu.addAction(i18n("Close chat (keep contents)")); close->setEnabled(m_workspace != QLatin1String("main")); connect(close, &QAction::triggered, this, [this] { closeWorkspace(m_workspace); });
    auto *restore = menu.addMenu(i18n("Restore closed chat")); bool any = false;
    for (const auto &entry : m_session.workspaces) { auto tab = entry.toObject(); if (!tab.value(QStringLiteral("closed")).toBool()) continue; any = true;
        connect(restore->addAction(tab.value(QStringLiteral("title")).toString()), &QAction::triggered, this, [this, tab]() mutable { const auto old = m_session; snapshotWorkspace(); tab.insert(QStringLiteral("closed"), false); m_session.setWorkspace(tab); m_session.activeWorkspace = tab.value(QStringLiteral("id")).toString(); if (!persist()) m_session = old; loadWorkspaceUi(); render(); }); }
    restore->setEnabled(any && !m_readOnly);
    auto *erase = menu.addAction(i18n("Delete chat and contents…")); erase->setEnabled(m_workspace != QLatin1String("main") && !m_readOnly);
    connect(erase, &QAction::triggered, this, [this] { if (QMessageBox::warning(this, i18n("Delete chat"), i18n("Delete this chat and its contents? Closing it instead keeps the contents."), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes) closeWorkspace(m_workspace, true); });
    menu.exec(m_workspaceActions->mapToGlobal(QPoint(0, m_workspaceActions->height())));
}
