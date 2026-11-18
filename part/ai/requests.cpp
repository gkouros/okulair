/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "assistantwidget.h"
#include "core/document.h"
#include "core/page.h"
#include "core/annotations.h"
#include <KConfigGroup>
#include <KSharedConfig>
#include <KLocalizedString>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QPlainTextEdit>
#include <QTimer>
#include <QUuid>
using namespace OkulAIr;
void AssistantWidget::initRequests()
{
    m_requestPanel = new QWidget(this); m_requestPanel->setObjectName(QStringLiteral("aiRequests"));
    auto *layout = new QVBoxLayout(m_requestPanel); layout->setContentsMargins(0, 0, 0, 0);
    m_requestToggle = new QToolButton(m_requestPanel); m_requestToggle->setCheckable(true); m_requestToggle->setChecked(true);
    m_requestToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_requestToggle->setArrowType(Qt::DownArrow);
    m_requestList = new QListWidget(m_requestPanel); m_requestList->setObjectName(QStringLiteral("aiRequestList")); m_requestList->setMaximumHeight(120);
    layout->addWidget(m_requestToggle); layout->addWidget(m_requestList);
    static_cast<QVBoxLayout *>(this->layout())->insertWidget(1, m_requestPanel);
    connect(m_requestToggle, &QToolButton::toggled, this, [this](bool expanded) { m_requestList->setVisible(expanded); m_requestToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow); });
    m_autoExplain = new QCheckBox(i18n("Explain my new highlights automatically"), m_settings);
    m_autoExplain->setObjectName(QStringLiteral("aiAutoExplainHighlights"));
    m_autoExplain->setToolTip(i18n("Add a brief AI explanation to new manual highlights using the selected service. Sharing permission is required."));
    const KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("OkulAIrHighlights"));
    m_autoExplain->setChecked(config.readEntry("ExplainManual", false));
    auto *settingsLayout = static_cast<QVBoxLayout *>(m_settings->layout()); settingsLayout->insertWidget(settingsLayout->count()-1, m_autoExplain);
    connect(m_autoExplain, &QCheckBox::toggled, this, [this](bool on) { if (!on) { m_requests.removeIf([](const Request &request) { return request.kind == QLatin1String("annotation"); }); if (m_busy && m_kind == QLatin1String("annotation")) stop(); else refreshRequests(); } KConfigGroup c(KSharedConfig::openConfig(), QStringLiteral("OkulAIrHighlights")); c.writeEntry("ExplainManual", on); c.sync(); });
    m_requestPanel->hide();
}
void AssistantWidget::refreshRequests()
{
    m_requestList->clear();
    const auto add = [this](const QString &id, const QString &title, bool running) {
        auto *item = new QListWidgetItem(m_requestList); auto *row = new QWidget(m_requestList);
        auto *layout = new QHBoxLayout(row); layout->setContentsMargins(3, 1, 3, 1);
        auto *label = new QLabel((running ? i18n("Running · ") : i18n("Queued · ")) + title.simplified().left(65), row);
        label->setTextFormat(Qt::PlainText); label->setToolTip(title); layout->addWidget(label, 1);
        auto *cancel = new QToolButton(row); cancel->setText(QStringLiteral("×")); cancel->setToolTip(i18n("Cancel this request"));
        cancel->setObjectName(id == QLatin1String("background-highlights") ? QStringLiteral("aiCancelHighlights") : running ? QStringLiteral("aiCancelRunningRequest") : QStringLiteral("aiCancelRequest_%1").arg(id));
        layout->addWidget(cancel); item->setSizeHint(row->sizeHint()); m_requestList->setItemWidget(item, row);
        connect(cancel, &QToolButton::clicked, this, [this, id, running] { if (id == QLatin1String("background-highlights")) stopHighlights(); else if (running) stop(); else { m_requests.removeIf([&id](const Request &request) { return request.id == id; }); refreshRequests(); } });
    };
    const auto title = [](const QString &kind, const QString &question) { return kind == QLatin1String("highlights") ? i18n("Highlight document") : kind == QLatin1String("summary") ? i18n("Paper overview") : kind == QLatin1String("annotation") ? i18n("Explain highlight") : question; };
    if (m_highlighting) add(QStringLiteral("background-highlights"), i18n("Highlight document · page %1 of %2", m_highlightNextPage, m_highlightPages.size()), true);
    if (m_busy) add(m_runningRequestId, title(m_kind, m_pending), true);
    for (const auto &request : m_requests) add(request.id, title(request.kind, request.question), false);
    m_requestToggle->setText(i18n("Requests (%1)", m_requests.size() + int(m_busy) + int(m_highlighting)));
    m_requestPanel->setVisible(m_busy || m_highlighting || !m_requests.isEmpty());
}
void AssistantWidget::queueRequest(Request request)
{
    if (request.question.trimmed().isEmpty()) return;
    if (m_requests.size() >= 32) { showError(i18n("The request queue is full. Cancel a queued request first.")); return; }
    request.provider = connection(); request.provider.key.clear(); request.account = chatGPTSelected() ? m_auth->activeClient() : QString();
    request.id = QUuid::createUuid().toString(QUuid::WithoutBraces); m_requests.append(request); refreshRequests();
    if (!m_busy && !m_queueScheduled) { m_queueScheduled = true; QTimer::singleShot(0, this, [this] { m_queueScheduled = false; runNextRequest(); }); }
}
void AssistantWidget::runNextRequest()
{
    if (m_busy || m_startingRequest || m_requests.isEmpty() || !m_document->pages()) return;
    auto request = m_requests.takeFirst();
    const auto selected = connection();
    if (request.provider.protocol != selected.protocol || request.provider.endpoint != selected.endpoint || request.provider.model != selected.model || request.account != (chatGPTSelected() ? m_auth->activeClient() : QString())) {
        showError(i18n("The service, account or model changed. The queued request was cancelled; submit it again with the intended connection."));
        refreshRequests(); if (!m_requests.isEmpty()) QTimer::singleShot(0,this,&AssistantWidget::runNextRequest); return;
    }
    if (m_session.workspace(request.workspace).isEmpty()) { refreshRequests(); runNextRequest(); return; }
    if (request.workspace != m_workspace) switchWorkspace(request.workspace);
    m_preparedRequest = request; m_hasPreparedRequest = true;
    m_scope->button(request.scope)->setChecked(true);
    start(request.question, request.kind);
    m_hasPreparedRequest = false;
    refreshRequests();
    if (!m_busy && !m_requests.isEmpty()) QTimer::singleShot(0, this, &AssistantWidget::runNextRequest);
}
void AssistantWidget::checkManualHighlights(bool initialize)
{
    for (uint page = 0; page < m_document->pages(); ++page) for (const auto *annotation : m_document->page(page)->annotations()) {
        if (annotation->subType() != Okular::Annotation::AHighlight || annotation->author().startsWith(QLatin1String("AI assistant")) || annotation->author() == QLatin1String("OkulAIr")) continue;
        const auto id = annotation->uniqueName(); if (id.isEmpty() || m_seenManualHighlights.contains(id)) continue;
        m_seenManualHighlights.insert(id);
        if (initialize || !m_autoExplain->isChecked() || !m_document->isAllowed(Okular::AllowCopy) || !m_document->canModifyPageAnnotation(annotation)) continue;
        m_document->requestTextPage(page);
        Okular::RegularAreaRect area;
        for (const auto &quad : static_cast<const Okular::HighlightAnnotation *>(annotation)->highlightQuads()) {
            double left=1, top=1, right=0, bottom=0;
            for (int i=0; i<4; ++i) { auto point=quad.point(i); left=qMin(left,point.x); top=qMin(top,point.y); right=qMax(right,point.x); bottom=qMax(bottom,point.y); }
            area.append(Okular::NormalizedRect(left,top,right,bottom));
        }
        if (area.isEmpty()) area.append(annotation->boundingRectangle());
        const auto quote = m_document->page(page)->text(&area).simplified(); if (quote.isEmpty()) continue;
        Request request; request.kind = QStringLiteral("annotation"); request.workspace = m_workspace; request.scope = 1; request.annotation = id; request.annotationPage = page; request.highlightedText = quote;
        request.question = i18n("Explain this highlighted passage: %1", quote);
        const auto source = m_document->page(page)->text().simplified(); const int at = source.indexOf(quote);
        request.focus.text = QStringLiteral("Highlighted passage: %1\n--- PAGE %2 (nearby context) ---\n%3").arg(quote).arg(page+1).arg(source.mid(qMax(0,at-4000),10000));
        request.focus.label = i18n("Manual highlight · p. %1", page+1);
        request.focus.regions.append(QJsonObject{{QStringLiteral("page"), int(page)}});
        queueRequest(request);
    }
}
