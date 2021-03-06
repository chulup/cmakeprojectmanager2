/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "cmakebuildsettingswidget.h"

#include "configmodel.h"
#include "cmakeproject.h"
#include "cmakebuildconfiguration.h"
#include "cmakeinlineeditordialog.h"

#include <coreplugin/coreicons.h>
#include <coreplugin/icore.h>
#include <coreplugin/find/itemviewfind.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/target.h>

#include <utils/detailswidget.h>
#include <utils/headerviewstretcher.h>
#include <utils/pathchooser.h>
#include <utils/itemviews.h>

#include <QBoxLayout>
#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSpacerItem>
#include <QGroupBox>
#include <QRadioButton>
#include <QFileDialog>
#include <QFormLayout>

namespace CMakeProjectManager {
namespace Internal {

// --------------------------------------------------------------------
// CMakeBuildSettingsWidget:
// --------------------------------------------------------------------

CMakeBuildSettingsWidget::CMakeBuildSettingsWidget(CMakeBuildConfiguration *bc) :
    m_buildConfiguration(bc),
    m_configModel(new ConfigModel(this)),
    m_configFilterModel(new QSortFilterProxyModel)
{
    QTC_CHECK(bc);

    setDisplayName(tr("CMake"));

    auto vbox = new QVBoxLayout(this);
    vbox->setMargin(0);
    auto container = new Utils::DetailsWidget;
    container->setState(Utils::DetailsWidget::NoSummary);
    vbox->addWidget(container);

    auto details = new QWidget(container);
    container->setWidget(details);

    auto mainLayout = new QGridLayout(details);
    mainLayout->setMargin(0);
    mainLayout->setColumnStretch(1, 10);

    auto project = static_cast<CMakeProject *>(bc->target()->project());

    auto buildDirChooser = new Utils::PathChooser;
    buildDirChooser->setBaseFileName(project->projectDirectory());
    buildDirChooser->setFileName(bc->buildDirectory());
    connect(buildDirChooser, &Utils::PathChooser::rawPathChanged, this,
            [this](const QString &path) {
                m_configModel->flush(); // clear out config cache...
                m_buildConfiguration->setBuildDirectory(Utils::FileName::fromString(path));
            });

    int row = 0;
    mainLayout->addWidget(new QLabel(tr("Build directory:")), row, 0);
    mainLayout->addWidget(buildDirChooser->lineEdit(), row, 1);
    mainLayout->addWidget(buildDirChooser->buttonAtIndex(0), row, 2);

    ++row;
    mainLayout->addItem(new QSpacerItem(20, 10), row, 0);

    ++row;
    m_errorLabel = new QLabel;
    m_errorLabel->setPixmap(Core::Icons::ERROR.pixmap());
    m_errorLabel->setVisible(false);
    m_errorMessageLabel = new QLabel;
    m_errorMessageLabel->setVisible(false);
    auto boxLayout = new QHBoxLayout;
    boxLayout->addWidget(m_errorLabel);
    boxLayout->addWidget(m_errorMessageLabel);
    mainLayout->addLayout(boxLayout, row, 0, 1, 3, Qt::AlignHCenter);

    ++row;
    mainLayout->addItem(new QSpacerItem(20, 10), row, 0);

    ++row;
    auto tree = new Utils::TreeView;
    connect(tree, &Utils::TreeView::activated,
            tree, [tree](const QModelIndex &idx) { tree->edit(idx); });
    m_configView = tree;
    m_configFilterModel->setSourceModel(m_configModel);
    m_configFilterModel->setFilterKeyColumn(2);
    m_configFilterModel->setFilterFixedString(QLatin1String("0"));
    m_configView->setModel(m_configFilterModel);
    m_configView->setMinimumHeight(300);
    m_configView->setRootIsDecorated(false);
    m_configView->setUniformRowHeights(true);
    auto stretcher = new Utils::HeaderViewStretcher(m_configView->header(), 1);
    m_configView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_configView->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_configView->setFrameShape(QFrame::NoFrame);
    m_configView->hideColumn(2); // Hide isAdvanced column
    QFrame *findWrapper = Core::ItemViewFind::createSearchableWrapper(m_configView, Core::ItemViewFind::LightColored);
    findWrapper->setFrameStyle(QFrame::StyledPanel);

    m_progressIndicator = new Utils::ProgressIndicator(Utils::ProgressIndicator::Large, findWrapper);
    m_progressIndicator->attachToWidget(findWrapper);
    m_progressIndicator->raise();
    m_progressIndicator->hide();
    m_showProgressTimer.setSingleShot(true);
    m_showProgressTimer.setInterval(50); // don't show progress for < 50ms tasks
    connect(&m_showProgressTimer, &QTimer::timeout, [this]() { m_progressIndicator->show(); });

    mainLayout->addWidget(findWrapper, row, 0, 1, 2);

    auto buttonLayout = new QVBoxLayout;
    m_editButton = new QPushButton(tr("&Edit"));
    buttonLayout->addWidget(m_editButton);
    m_resetButton = new QPushButton(tr("&Reset"));
    m_resetButton->setEnabled(false);
    buttonLayout->addWidget(m_resetButton);
    buttonLayout->addItem(new QSpacerItem(10, 10, QSizePolicy::Fixed, QSizePolicy::Fixed));
    m_showAdvancedCheckBox = new QCheckBox(tr("Advanced"));
    buttonLayout->addWidget(m_showAdvancedCheckBox);
    buttonLayout->addItem(new QSpacerItem(10, 10, QSizePolicy::Minimum, QSizePolicy::Expanding));

    mainLayout->addLayout(buttonLayout, row, 2);

    // Toolchain settings
    {
        ++row;
        m_toolchainGroupBox = new QGroupBox(this);
        m_toolchainGroupBox->setCheckable(true);
        m_toolchainGroupBox->setTitle(tr("Override toolchain:"));

        QFormLayout *toolchainLayout = new QFormLayout;
        toolchainLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        m_toolchainGroupBox->setLayout(toolchainLayout);

        //m_toolchainComboBox = new QComboBox(this);
        m_toolchainLineEdit = new Utils::FancyLineEdit(this);

        m_toolchainFileSelectPushButton = new QPushButton(this);
        m_toolchainFileSelectPushButton->setText(tr("Browse..."));

        m_toolchainEditPushButton = new QPushButton(this);
        m_toolchainEditPushButton->setText(tr("Edit"));

        m_fileToolchainRadioButton = new QRadioButton(tr("Toolchain file:"), this);
        m_inlineToolchainRadioButton = new QRadioButton(tr("Inline Toolchain:"), this);

        auto hbox = new QHBoxLayout;
        hbox->addWidget(m_fileToolchainRadioButton);
        hbox->addWidget(m_toolchainLineEdit);
        hbox->addWidget(m_toolchainFileSelectPushButton);
        toolchainLayout->addRow(hbox);

        hbox = new QHBoxLayout;
        hbox->addWidget(m_inlineToolchainRadioButton);
        hbox->addStretch(10);
        hbox->addWidget(m_toolchainEditPushButton);
        toolchainLayout->addRow(hbox);

        mainLayout->addWidget(m_toolchainGroupBox, row, 0, 1, 3);
    }

    ++row;
    m_reconfigureButton = new QPushButton(tr("Apply Configuration Changes"));
    m_reconfigureButton->setEnabled(false);
    mainLayout->addWidget(m_reconfigureButton, row, 0, 1, 3);

    updateAdvancedCheckBox();
    setError(bc->error());

    connect(project, &CMakeProject::parsingStarted, this, [this]() {
        updateButtonState();
        m_showProgressTimer.start();
    });

    if (m_buildConfiguration->isParsing())
        m_showProgressTimer.start();
    else
        m_configModel->setConfiguration(m_buildConfiguration->completeCMakeConfiguration());

    connect(m_buildConfiguration, &CMakeBuildConfiguration::dataAvailable,
            this, [this, buildDirChooser, stretcher]() {
        updateButtonState();
        m_configModel->setConfiguration(m_buildConfiguration->completeCMakeConfiguration());
        stretcher->stretch();
        buildDirChooser->triggerChanged(); // refresh valid state...
        m_showProgressTimer.stop();
        m_progressIndicator->hide();
    });

    connect(m_configModel, &QAbstractItemModel::dataChanged,
            this, &CMakeBuildSettingsWidget::updateButtonState);
    connect(m_configModel, &QAbstractItemModel::modelReset,
            this, &CMakeBuildSettingsWidget::updateButtonState);

    connect(m_showAdvancedCheckBox, &QCheckBox::stateChanged,
            this, &CMakeBuildSettingsWidget::updateAdvancedCheckBox);

    connect(m_resetButton, &QPushButton::clicked, m_configModel, &ConfigModel::resetAllChanges);
    connect(m_reconfigureButton, &QPushButton::clicked, this, [this]() {
        m_buildConfiguration->setCurrentCMakeConfiguration(m_configModel->configurationChanges(), currentToolchainInfo());
    });
    connect(m_editButton, &QPushButton::clicked, [this]() {
        QModelIndex idx = m_configView->currentIndex();
        if (idx.column() != 1)
            idx = idx.sibling(idx.row(), 1);
        m_configView->setCurrentIndex(idx);
        m_configView->edit(idx);
    });

    connect(bc, &CMakeBuildConfiguration::errorOccured, this, &CMakeBuildSettingsWidget::setError);

    connect(m_toolchainGroupBox, &QGroupBox::clicked, [this](bool) {
        updateButtonState();
    });
    connect(m_toolchainEditPushButton, &QAbstractButton::clicked, [this](bool) {
        toolchainEdit();
    });
    connect(m_toolchainFileSelectPushButton, &QAbstractButton::clicked, [this](bool) {
        toolchainFileSelect();
    });
    connect(m_fileToolchainRadioButton, &QAbstractButton::toggled, this, &CMakeBuildSettingsWidget::toolchainRadio);
    connect(m_inlineToolchainRadioButton, &QAbstractButton::toggled, this, &CMakeBuildSettingsWidget::toolchainRadio);

    auto info = bc->cmakeToolchainInfo();
    m_toolchainInlineCurrent = info.toolchainInline;

    m_toolchainGroupBox->setChecked(false);
    m_toolchainLineEdit->setDisabled(true);
    m_toolchainEditPushButton->setDisabled(true);
    m_toolchainFileSelectPushButton->setDisabled(true);

    if (info.toolchainOverride != CMakeToolchainOverrideType::Disabled) {
        m_toolchainGroupBox->setChecked(true);
        if (info.toolchainOverride == CMakeToolchainOverrideType::File) {
            m_fileToolchainRadioButton->setChecked(true);
        } else if (info.toolchainOverride == CMakeToolchainOverrideType::Inline) {
            m_inlineToolchainRadioButton->setChecked(true);
        }
    }
    m_toolchainLineEdit->setText(info.toolchainFile);
}

void CMakeBuildSettingsWidget::setError(const QString &message)
{
    bool showWarning = !message.isEmpty();
    m_errorLabel->setVisible(showWarning);
    m_errorLabel->setToolTip(message);
    m_errorMessageLabel->setVisible(showWarning);
    m_errorMessageLabel->setText(message);
    m_errorMessageLabel->setToolTip(message);

    m_configView->setVisible(!showWarning);
    m_editButton->setVisible(!showWarning);
    m_resetButton->setVisible(!showWarning);
    m_showAdvancedCheckBox->setVisible(!showWarning);
    m_reconfigureButton->setVisible(!showWarning);
}

void CMakeBuildSettingsWidget::updateButtonState()
{
    const bool isParsing = m_buildConfiguration->isParsing();
    const bool hasChanges = m_configModel->hasChanges();
    m_resetButton->setEnabled(hasChanges && !isParsing);

    auto const& prev = m_buildConfiguration->cmakeToolchainInfo();
    auto const  curr = currentToolchainInfo();

    // If toolchain changed we need full tree regeneration
    bool hasToolchainChanges = (curr.toolchainOverride != prev.toolchainOverride ||
                                curr.toolchainFile != prev.toolchainFile ||
                                curr.toolchainInline != prev.toolchainInline);

    m_reconfigureButton->setEnabled((hasChanges || hasToolchainChanges || m_configModel->hasCMakeChanges()) && !isParsing);
}

void CMakeBuildSettingsWidget::updateAdvancedCheckBox()
{
    // Switch between Qt::DisplayRole (everything is "0") and Qt::EditRole (advanced is "1").
    m_configFilterModel->setFilterRole(m_showAdvancedCheckBox->isChecked() ? Qt::EditRole : Qt::DisplayRole);
}

CMakeToolchainInfo CMakeBuildSettingsWidget::currentToolchainInfo() const
{
    auto curr = m_buildConfiguration->cmakeToolchainInfo();
    curr.toolchainOverride = CMakeToolchainOverrideType::Disabled;
    curr.toolchainFile     = m_toolchainLineEdit->text();
    curr.toolchainInline   = m_toolchainInlineCurrent;
    // toolchainInline already filled
    if (m_toolchainGroupBox->isChecked()) {
        if (m_fileToolchainRadioButton->isChecked()) {
            curr.toolchainOverride = CMakeToolchainOverrideType::File;
        } else if (m_inlineToolchainRadioButton->isChecked()) {
            curr.toolchainOverride = CMakeToolchainOverrideType::Inline;
        }
    }
    return curr;
}

void CMakeBuildSettingsWidget::toolchainFileSelect()
{
    QFileDialog openToolchainDialog(this,
                                    tr("Select CMake toolchain"),
                                    m_buildConfiguration->target()->project()->projectDirectory().toString(),
                                    QLatin1String("CMake files (*.cmake);; All (*)"));

    openToolchainDialog.setFileMode(QFileDialog::ExistingFile);
    openToolchainDialog.setAcceptMode(QFileDialog::AcceptOpen);

    QList<QUrl> urls;
    urls << QUrl::fromLocalFile(m_buildConfiguration->target()->project()->projectDirectory().toString());
    openToolchainDialog.setSidebarUrls(urls);

    if (!m_toolchainLineEdit->text().isEmpty()) {
        QFileInfo fi(m_toolchainLineEdit->text());
        openToolchainDialog.setDirectory(fi.absolutePath());
    }

    if (openToolchainDialog.exec()) {
        auto files = openToolchainDialog.selectedFiles();
        if (!files.isEmpty())
            m_toolchainLineEdit->setText(files[0]);
    }

    updateButtonState();
}

void CMakeBuildSettingsWidget::toolchainEdit()
{
    bool ok;
    QString current = m_toolchainInlineCurrent;

    if (current.isEmpty()) {
        QFile sampleToolchain(QLatin1String(":/cmakeproject/inlinetoolchainexample.cmake"));
        sampleToolchain.open(QIODevice::ReadOnly | QIODevice::Text);
        auto data = sampleToolchain.readAll();
        current = QLatin1String(data);
    }

    QString content = CMakeInlineEditorDialog::getContent(this, current, &ok);
    if (ok)
        m_toolchainInlineCurrent = std::move(content);

    updateButtonState();
}

void CMakeBuildSettingsWidget::toolchainRadio(bool /*toggled*/)
{
    m_toolchainLineEdit->setEnabled(m_fileToolchainRadioButton->isChecked());
    m_toolchainFileSelectPushButton->setEnabled(m_fileToolchainRadioButton->isChecked());
    m_toolchainEditPushButton->setEnabled(m_inlineToolchainRadioButton->isChecked());
    updateButtonState();
}

} // namespace Internal
} // namespace CMakeProjectManager
