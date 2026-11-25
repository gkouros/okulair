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
#include <QFrame>
#include <QScreen>
#include <QPainter>
using namespace OkulAIr;
void AssistantWidget::initRequests()
{
    m_requestToggle = new QToolButton(this);
    m_requestToggle->setObjectName(QStringLiteral("aiRequestActivity"));
    m_requestToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_requestToggle->setIcon(QIcon::fromTheme(QStringLiteral("view-task")));
    m_requestToggle->setToolTip(i18n("Show active and queued requests"));
    m_requestToggle->setAccessibleName(i18n("Request activity"));
    // Reserve a constant toolbar footprint in every activity state.
    m_requestToggle->setFixedWidth(m_requestToggle->fontMetrics().horizontalAdvance(i18n("Working · 34")) + 40);
    findChild<QHBoxLayout *>(QStringLiteral("aiChatTools"))->addWidget(m_requestToggle);
    auto *popup = new QFrame(this, Qt::Popup);
    popup->setFrameShape(QFrame::StyledPanel);
    popup->setAttribute(Qt::WA_NoMouseReplay);
    m_requestPanel = popup; popup->setObjectName(QStringLiteral("aiRequests"));
    auto *layout = new QVBoxLayout(popup);
    layout->addWidget(new QLabel(i18n("Active requests"), popup));
    m_requestList = new QListWidget(popup);
    m_requestList->setObjectName(QStringLiteral("aiRequestList"));
    m_requestList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_requestList);
    connect(m_requestToggle, &QToolButton::clicked, this, [this] {
        if (m_requestPanel->isVisible()) { m_requestPanel->hide(); return; }
        const auto available = m_requestToggle->screen()->availableGeometry();
        const int rows = qMax(1, m_requestList->count());
        const int height = qMin(280, 65 + rows * qMax(24, m_requestList->sizeHintForRow(0)));
        m_requestPanel->resize(qMin(420, available.width()), qMin(height, available.height()));
        auto point = m_requestToggle->mapToGlobal(QPoint(m_requestToggle->width(), m_requestToggle->height()));
        point.rx() -= m_requestPanel->width();
        if (point.y() + m_requestPanel->height() > available.bottom()) point.setY(m_requestToggle->mapToGlobal(QPoint(0, 0)).y() - m_requestPanel->height());
        point.setX(qBound(available.left(), point.x(), available.right() - m_requestPanel->width() + 1));
        point.setY(qBound(available.top(), point.y(), available.bottom() - m_requestPanel->height() + 1));
        m_requestPanel->move(point); m_requestPanel->show();
    });
    auto *spinner = new QTimer(m_requestToggle); spinner->setObjectName(QStringLiteral("aiActivitySpinner")); spinner->setInterval(100);
    connect(spinner, &QTimer::timeout, this, [this, angle = 0]() mutable {
        const bool working = m_busy || m_highlighting || !m_requests.isEmpty();
        if (!working) return;
        QPixmap pixmap(32, 32); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing); painter.translate(16,16); painter.rotate(angle);
        painter.setPen(QPen(m_requestToggle->palette().color(QPalette::Highlight), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(-11,-11,22,22), 0, 270*16); painter.end();
        m_requestToggle->setIcon(QIcon(pixmap)); angle = (angle + 30) % 360;
    });
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
        auto *details = new QVBoxLayout; details->setSpacing(2);
        auto *label = new QLabel(row);
        label->setText(label->fontMetrics().elidedText(title.simplified(), Qt::ElideRight, 320));
        label->setTextFormat(Qt::PlainText); label->setToolTip(title); details->addWidget(label);
        auto *state = new QLabel(id == QLatin1String("background-highlights") ? i18n("Working in background") : running ? i18n("Running") : i18n("Queued"), row);
        auto font = state->font(); font.setPointSizeF(qMax(8.0, font.pointSizeF() - 1)); state->setFont(font);
        details->addWidget(state); layout->addLayout(details, 1);
        auto *cancel = new QToolButton(row); cancel->setText(QStringLiteral("×")); cancel->setToolTip(i18n("Cancel this request"));
        cancel->setObjectName(id == QLatin1String("background-highlights") ? QStringLiteral("aiCancelHighlights") : running ? QStringLiteral("aiCancelRunningRequest") : QStringLiteral("aiCancelRequest_%1").arg(id));
        layout->addWidget(cancel); item->setSizeHint(row->sizeHint()); m_requestList->setItemWidget(item, row);
        connect(cancel, &QToolButton::clicked, this, [this, id, running] { if (id == QLatin1String("background-highlights")) stopHighlights(true); else if (running) stop(); else { m_requests.removeIf([&id](const Request &request) { return request.id == id; }); refreshRequests(); } });
    };
    const auto title = [](const QString &kind, const QString &question) { return kind == QLatin1String("highlights") ? i18n("Highlight document") : kind == QLatin1String("summary") ? i18n("Paper overview") : kind == QLatin1String("annotation") ? i18n("Explain highlight") : question; };
    if (m_highlighting) add(QStringLiteral("background-highlights"), i18n("Highlight document · page %1 of %2", m_highlightNextPage, m_highlightPages.size()), true);
    if (m_busy) add(m_runningRequestId, title(m_kind, m_pending), true);
    for (const auto &request : m_requests) add(request.id, title(request.kind, request.question), false);
    const int count = m_requests.size() + int(m_busy) + int(m_highlighting);
    m_requestToggle->setEnabled(count > 0);
    m_requestToggle->setText(count ? i18n("Working · %1", count) : QString());
    auto *spinner = m_requestToggle->findChild<QTimer *>(QStringLiteral("aiActivitySpinner"));
    if (count && !spinner->isActive()) spinner->start();
    else if (!count) spinner->stop();
    m_requestToggle->setToolTip(count ? i18n("%1 active or queued requests. Click to view or cancel.", count) : i18n("No active requests"));
    if (!count) { m_requestPanel->hide(); m_requestToggle->setIcon(QIcon::fromTheme(QStringLiteral("view-task"))); }
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
