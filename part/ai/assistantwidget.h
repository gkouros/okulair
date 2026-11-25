/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "core/observer.h"
#include "chatgptauth.h"
#include "provider.h"
#include "session.h"
#include <QWidget>
#include <functional>
#include <QPointer>
#include <QSet>

class QMainWindow;
class QTabBar;
class QToolButton;
class QDialog;
class QListWidget;
class QCheckBox;
class QButtonGroup;
class QTimer;
class QMimeData;
class QImage;
class QDockWidget;
class KMessageWidget;
class QFormLayout;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
namespace Okular { class Document; }
namespace OkulAIr
{
struct FocusContext {
    QString text;
    QString label;
    QString error;
    QByteArray image;
    QJsonArray regions;
};
class AssistantWidget : public QWidget, public Okular::DocumentObserver
{
    Q_OBJECT
public:
    AssistantWidget(Okular::Document *document, QWidget *parent);
    ~AssistantWidget() override;
    QWidget *settingsWidget() const { return m_settings; }
    void attachChatDock(QMainWindow *host);
    void showChat(bool selection = false);
    void openSelectionChat(bool newChat);
    void prepareSelection(FocusContext context, bool newChat = false);
    void setContextSource(std::function<FocusContext(bool)> source) { m_contextSource = std::move(source); }
    void notifySetup(const QList<Okular::Page *> &pages, int flags) override;
    void notifyPageChanged(int page, int flags) override;
Q_SIGNALS:
    void settingsRequested();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    struct Request {
        QString id, question, kind, workspace, annotation, highlightedText, account;
        Connection provider;
        int scope = 0, annotationPage = -1;
        FocusContext focus;
    };
    QList<Request> m_requests;
    Request m_preparedRequest;
    bool m_hasPreparedRequest = false, m_queueScheduled = false, m_startingRequest = false;
    QString m_runningRequestId;
    QListWidget *m_requestList;
    QToolButton *m_requestToggle;
    QWidget *m_requestPanel;
    QCheckBox *m_autoExplain;
    QSet<QString> m_seenManualHighlights;
    QString m_annotationTarget, m_annotationBefore, m_annotationQuote;
    int m_annotationPage = -1;
    QList<int> m_highlightBatchPages;
    void initRequests();
    void refreshRequests();
    void queueRequest(Request request);
    void runNextRequest();
    void checkManualHighlights(bool initialize = false);
    void editHighlightCategory(bool add, const QString &category = {});
    void updateHighlightCategorySelection();
    void deleteHighlightCategory(const QString &id);
    void rebuildHighlightCategories(bool recolor = false);
    void initWorkspaces();
    void loadWorkspaceUi();
    void snapshotWorkspace();
    void switchWorkspace(const QString &id);
    void createWorkspace();
    void workspaceMenu();
    void closeWorkspace(const QString &id, bool erase = false);
    void updateWorkspaceControls();
    FocusContext workspaceContext() const;
    bool messageInWorkspace(const QJsonObject &message) const;
    QString evidenceBody(const QString &text);
    void showEvidence(const QJsonObject &evidence);
    void clearEvidence();
    void initHighlightTools();
    void updateHighlightFilters();
    void generateHighlights(bool replace);
    void sendHighlightBatch();
    void startBackgroundHighlights();
    void extractHighlights(int page, quint64 generation);
    void finishHighlights(const QString &text, const QString &error);
    void stopHighlights(bool cancelled = false);
    Provider m_highlightProvider;
    bool m_highlighting = false;
    quint64 m_highlightGeneration = 0;
    Connection m_highlightConnection;
    int m_highlightScope = 0;
    FocusContext m_highlightFocus;
    QStringList m_highlightText;
    QString m_highlightContext;

    int highlightsPerPage() const;
    QString sharingKey() const;
    bool hasSharingPermission() const;
    bool confirmSharing();
    void submit();
    void sendRequest();
    void load(bool recoverJournal = false);
    void render();
    void renderStream();
    void addFiles();
    void attachFile(const QString &path);
    void attachImage(const QImage &image, const QString &name);
    void updateAttachments();
    void clearChat();
    void clearChats(bool keepMain);
    void clearHighlights(bool aiHighlights);
    void serviceChanged();
    void updateAccountUi();
    void connectService();
    void showError(const QString &text);
    bool chatGPTSelected() const;
    void setBusy(bool busy);
    void start(const QString &question, const QString &kind);
    void extract(int page, quint64 generation);
    void stop();
    bool persist();
    void finish(const QString &text, const QString &error);
    void proposeHighlights(const QString &text);
    Connection connection() const;
    void updateEffortUi();
    QString effortSettingsKey() const;
    QComboBox *m_effort = nullptr;
    QString journalPath() const;
    void journal();
    void recover();
    Okular::Document *m_document;
    QTabBar *m_tabs;
    QToolButton *m_newWorkspace;
    QToolButton *m_workspaceActions;
    QWidget *m_tabStrip;
    QWidget *m_chatActions;
    QString m_workspace = QStringLiteral("main");
    QString m_recoveryBase;
    bool m_changingWorkspace = false;
    QHash<QString, QJsonObject> m_evidence;
    quint64 m_evidenceGeneration = 0;
    QDialog *m_highlightPanel;
    QWidget *m_categoryLegend;
    QWidget *m_categoryEditorControls;
    QComboBox *m_density;
    QSet<QString> m_highlightCategories;
    QList<QCheckBox *> m_categoryFilters;
    QPushButton *m_generateHighlights;
    QPushButton *m_regenerateHighlights;
    bool m_replaceHighlights = false;
    QList<int> m_highlightPages;
    int m_highlightNextPage = 0;
    QJsonArray m_highlightCandidates;
    QPointer<QWidget> m_settings;
    QPointer<QMainWindow> m_dockHost;
    QPointer<QDockWidget> m_chatDock;
    QByteArray m_dockStateBeforeHide;
    bool m_floatingHiddenForTab = false;
    QButtonGroup *m_scope;
    QWidget *m_scopeBar;
    QJsonArray m_attachments;
    QWidget *m_attachmentBar;
    QPushButton *m_attach;
    QPushButton *m_clearChat;
    QPushButton *m_clearHighlights;
    QPushButton *m_clearUserHighlights;
    QTimer *m_streamTimer;
    QString m_renderedPartial;
    int m_streamPosition = -1;
    int m_streamStart = -1;
    QString formattedAnswer(const QString &text);
    std::function<FocusContext(bool)> m_contextSource;
    FocusContext m_focus;
    int m_activeScope = 0;
    Provider m_provider;
    Session m_session;
    ChatGPTAuth *m_auth;
    QFormLayout *m_connectionForm;
    QComboBox *m_modelPicker;
    QComboBox *m_accounts;
    QPushButton *m_connect;
    QPushButton *m_addAccount;
    QPushButton *m_signOut;
    QPushButton *m_usage;
    QLabel *m_connectionHint;
    bool m_waitingForModels = false;
    QString m_resumeQuestion, m_resumeKind;
    QString m_lastAuthClient;
    quint64 m_resumeGeneration = 0;
    QComboBox *m_protocol;
    QLineEdit *m_endpoint;
    QLineEdit *m_model;
    QLineEdit *m_key;
    QPushButton *m_resetSharing;
    QTextBrowser *m_history;
    QPlainTextEdit *m_question;
    QPushButton *m_send;
    QPushButton *m_summary;
    QPushButton *m_highlight;
    QPushButton *m_stop;
    KMessageWidget *m_status;
    QStringList m_pages;
    QString m_context;
    QString m_pending;
    QString m_kind;
    QString m_partial;
    Connection m_active;
    bool m_followOutput = true;
    bool m_updatingHistory = false;
    bool m_busy = false;
    bool m_writing = false;
    bool m_readOnly = false;
    quint64 m_generation = 0;
};
}
