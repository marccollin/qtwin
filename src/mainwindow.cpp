#include "mainwindow.h"

#include "uddfwriter.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothLocalDevice>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

constexpr int DiveIdRole = Qt::UserRole;

QString printable(const QByteArray &data)
{
    QString text;
    for (const char c : data) {
        if (c == '\r')
            text += QLatin1String("\\r");
        else if (c == '\n')
            text += QLatin1String("\\n");
        else if (c >= 32 && c < 127)
            text += QLatin1Char(c);
        else
            text += QStringLiteral("\\x%1").arg(uchar(c), 2, 16, QLatin1Char('0'));
    }
    return text;
}

bool isDTwin(const QBluetoothDeviceInfo &info)
{
    return info.name().contains(QLatin1String("D-Twin"), Qt::CaseInsensitive);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_client(new DTwinClient(this))
    , m_discovery(new QBluetoothDeviceDiscoveryAgent(this))
{
    buildUi();
    loadSettings();

    connect(m_discovery, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this, &MainWindow::onDeviceDiscovered);
    connect(m_discovery, &QBluetoothDeviceDiscoveryAgent::finished, this, [this] {
        setBusy(false);
        int found = 0;
        for (const QBluetoothDeviceInfo &info : std::as_const(m_devices))
            found += isDTwin(info) ? 1 : 0;
        log(found ? tr("Search finished: %1 D-Twin device(s) found.").arg(found)
                  : tr("Search finished: no D-Twin found. Enable PC Connect on the computer "
                       "(Option → System → PC Connect → Connect!) and search again."));
        if (m_deviceList->count() > 0 && !m_deviceList->currentItem())
            m_deviceList->setCurrentRow(0);
    });
    connect(m_discovery, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this, [this] {
        setBusy(false);
        log(tr("Bluetooth search error: %1").arg(m_discovery->errorString()));
    });

    connect(m_client, &DTwinClient::traffic, this, [this](const QString &direction, const QByteArray &data) {
        if (m_showTraffic->isChecked())
            log(QStringLiteral("%1 %2").arg(direction, printable(data)));
    });
    connect(m_client, &DTwinClient::failed, this, [this](const QString &message) {
        setBusy(false);
        log(tr("Error: %1").arg(message));
    });
    connect(m_client, &DTwinClient::diveListInfo, this, [this](const QString &response) {
        m_listInfo->setText(tr("Computer answered: %1").arg(response));
        setBusy(true, tr("Reading the dive list…"));
    });
    connect(m_client, &DTwinClient::diveListEntryReceived, this, &MainWindow::onDiveListEntry);
    connect(m_client, &DTwinClient::diveListFinished, this, [this](int count) {
        setBusy(false);
        log(tr("Dive list loaded: %1 dive(s).").arg(count));
    });
    connect(m_client, &DTwinClient::diveDownloadStarted, this, [this](const QString &diveId, int index, int total) {
        m_progress->setRange(0, total);
        m_progress->setValue(index);
        setBusy(true, tr("Downloading dive %1 (%2 of %3)…").arg(diveId).arg(index + 1).arg(total));
    });
    connect(m_client, &DTwinClient::diveRecordsReceived, this, [this](const QString &diveId, int records) {
        m_status->setText(tr("Downloading dive %1: %2 record(s) received…").arg(diveId).arg(records));
    });
    connect(m_client, &DTwinClient::diveDownloaded, this, &MainWindow::onDiveDownloaded);
    connect(m_client, &DTwinClient::downloadsFinished, this, [this] {
        setBusy(false);
        m_progress->setRange(0, 1);
        m_progress->setValue(1);
        log(tr("Download finished: %1 dive(s) exported, %2 failed, in %3")
                .arg(m_exported)
                .arg(m_exportFailures)
                .arg(QDir::toNativeSeparators(m_outputDir->text())));
    });
    connect(m_client, &DTwinClient::stopSent, this, [this] {
        setBusy(false);
        log(tr("STOP sent: PC connection ended."));
    });

    updateActions();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    setWindowTitle(tr("QTwin %1 — D-Twin dive log download").arg(QStringLiteral(QTWIN_VERSION)));

    auto *deviceGroup = new QGroupBox(tr("1. Dive computer"));
    m_searchButton = new QPushButton(tr("Search devices"));
    m_showAllDevices = new QCheckBox(tr("Show all Bluetooth devices"));
    m_deviceList = new QListWidget;
    m_deviceList->setMaximumHeight(90);
    m_channel = new QSpinBox;
    m_channel->setRange(1, 30);
    m_channel->setToolTip(tr("RFCOMM channel. Chosen automatically: 5 if the device name contains ':', otherwise 1."));
    auto *deviceButtons = new QHBoxLayout;
    deviceButtons->addWidget(m_searchButton);
    deviceButtons->addWidget(m_showAllDevices);
    deviceButtons->addStretch();
    deviceButtons->addWidget(new QLabel(tr("Channel:")));
    deviceButtons->addWidget(m_channel);
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    deviceLayout->addLayout(deviceButtons);
    deviceLayout->addWidget(m_deviceList);

    auto *diveGroup = new QGroupBox(tr("2. Dives"));
    m_loadListButton = new QPushButton(tr("Load dive list"));
    m_listInfo = new QLabel;
    m_diveTable = new QTableWidget(0, 5);
    m_diveTable->setHorizontalHeaderLabels({tr("Dive"), tr("Date"), tr("Time"), tr("Duration"), tr("Max depth")});
    m_diveTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_diveTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_diveTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diveTable->verticalHeader()->setVisible(false);
    m_diveTable->horizontalHeader()->setStretchLastSection(true);
    m_downloadSelectedButton = new QPushButton(tr("Download selected"));
    m_downloadAllButton = new QPushButton(tr("Download all"));
    m_cancelButton = new QPushButton(tr("Cancel"));
    m_stopButton = new QPushButton(tr("End PC connection"));
    m_stopButton->setToolTip(tr("Sends STOP to the dive computer, like closing the original log window."));
    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(m_loadListButton);
    listButtons->addWidget(m_listInfo, 1);
    auto *diveButtons = new QHBoxLayout;
    diveButtons->addWidget(m_downloadSelectedButton);
    diveButtons->addWidget(m_downloadAllButton);
    diveButtons->addWidget(m_cancelButton);
    diveButtons->addStretch();
    diveButtons->addWidget(m_stopButton);
    auto *diveLayout = new QVBoxLayout(diveGroup);
    diveLayout->addLayout(listButtons);
    diveLayout->addWidget(m_diveTable);
    diveLayout->addLayout(diveButtons);

    auto *exportGroup = new QGroupBox(tr("Export"));
    m_outputDir = new QLineEdit;
    auto *browseButton = new QPushButton(tr("Browse…"));
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(m_outputDir);
    folderRow->addWidget(browseButton);
    auto *exportLayout = new QFormLayout(exportGroup);
    exportLayout->addRow(tr("Output folder:"), folderRow);

    m_showTraffic = new QCheckBox(tr("Show Bluetooth traffic"));
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    auto *logWidget = new QWidget;
    auto *logLayout = new QVBoxLayout(logWidget);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->addWidget(m_showTraffic);
    logLayout->addWidget(m_log);

    auto *top = new QWidget;
    auto *topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->addWidget(deviceGroup);
    topLayout->addWidget(diveGroup, 1);
    topLayout->addWidget(exportGroup);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(top);
    splitter->addWidget(logWidget);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    auto *central = new QWidget;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->addWidget(splitter);
    setCentralWidget(central);

    m_status = new QLabel;
    m_progress = new QProgressBar;
    m_progress->setMaximumWidth(220);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    statusBar()->addWidget(m_status, 1);
    statusBar()->addPermanentWidget(m_progress);

    resize(720, 760);

    connect(m_searchButton, &QPushButton::clicked, this, &MainWindow::searchDevices);
    connect(m_showAllDevices, &QCheckBox::toggled, this, &MainWindow::refreshDeviceList);
    connect(m_deviceList, &QListWidget::currentRowChanged, this, &MainWindow::onDeviceSelectionChanged);
    connect(m_deviceList, &QListWidget::itemDoubleClicked, this, [this] {
        if (!m_busy)
            loadDiveList();
    });
    connect(m_loadListButton, &QPushButton::clicked, this, &MainWindow::loadDiveList);
    connect(m_downloadSelectedButton, &QPushButton::clicked, this, &MainWindow::downloadSelected);
    connect(m_downloadAllButton, &QPushButton::clicked, this, &MainWindow::downloadAll);
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_discovery->isActive()) {
            m_discovery->stop();
            setBusy(false);
        }
        m_client->cancel();
    });
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::endPcConnection);
    connect(browseButton, &QPushButton::clicked, this, &MainWindow::chooseOutputFolder);
    connect(m_diveTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateActions);
}

void MainWindow::loadSettings()
{
    const QSettings settings;
    const QString defaultDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("QTwin"));
    m_outputDir->setText(settings.value(QStringLiteral("outputDir"), defaultDir).toString());
    m_showTraffic->setChecked(settings.value(QStringLiteral("showTraffic"), false).toBool());
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
}

void MainWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("outputDir"), m_outputDir->text());
    settings.setValue(QStringLiteral("showTraffic"), m_showTraffic->isChecked());
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_discovery->stop();
    m_client->cancel();
    saveSettings();
    event->accept();
}

void MainWindow::searchDevices()
{
    const QBluetoothLocalDevice adapter;
    if (!adapter.isValid()) {
        log(tr("No Bluetooth adapter found."));
        return;
    }
    if (adapter.hostMode() == QBluetoothLocalDevice::HostPoweredOff) {
        log(tr("Bluetooth is turned off. Turn it on and search again."));
        return;
    }

    m_devices.clear();
    m_deviceList->clear();
    setBusy(true, tr("Searching for Bluetooth devices…"));
    m_progress->setRange(0, 0);
    m_discovery->start(QBluetoothDeviceDiscoveryAgent::ClassicMethod);
}

void MainWindow::onDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
    for (const QBluetoothDeviceInfo &known : std::as_const(m_devices)) {
        if (known.address() == info.address())
            return;
    }
    m_devices << info;
    if (isDTwin(info))
        log(tr("Found %1 (%2)").arg(info.name(), info.address().toString()));
    refreshDeviceList();

    // No need to wait for the full 30 s inquiry once the dive computer is visible.
    if (isDTwin(info) && !m_showAllDevices->isChecked() && m_discovery->isActive()) {
        m_discovery->stop();
        setBusy(false);
        for (int row = 0; row < m_deviceList->count(); ++row) {
            if (m_deviceList->item(row)->data(Qt::UserRole).toString() == info.address().toString()) {
                m_deviceList->setCurrentRow(row);
                break;
            }
        }
        log(tr("Search stopped: D-Twin found. Click \"Load dive list\" or double-click the device."));
    }
}

void MainWindow::refreshDeviceList()
{
    const QString current = m_deviceList->currentItem() ? m_deviceList->currentItem()->data(Qt::UserRole).toString()
                                                        : QString();
    m_deviceList->clear();
    for (const QBluetoothDeviceInfo &info : std::as_const(m_devices)) {
        if (!m_showAllDevices->isChecked() && !isDTwin(info))
            continue;
        const QString name = info.name().isEmpty() ? tr("(no name)") : info.name();
        auto *item = new QListWidgetItem(QStringLiteral("%1  —  %2").arg(name, info.address().toString()));
        item->setData(Qt::UserRole, info.address().toString());
        item->setData(Qt::UserRole + 1, info.name());
        m_deviceList->addItem(item);
        if (item->data(Qt::UserRole).toString() == current)
            m_deviceList->setCurrentItem(item);
    }
    updateActions();
}

void MainWindow::onDeviceSelectionChanged()
{
    if (const QListWidgetItem *item = m_deviceList->currentItem())
        m_channel->setValue(DTwinClient::channelForDeviceName(item->data(Qt::UserRole + 1).toString()));
    updateActions();
}

bool MainWindow::selectTarget()
{
    const QListWidgetItem *item = m_deviceList->currentItem();
    if (!item) {
        log(tr("Select a dive computer first."));
        return false;
    }
    m_client->setTarget(QBluetoothAddress(item->data(Qt::UserRole).toString()), quint16(m_channel->value()));
    return true;
}

void MainWindow::loadDiveList()
{
    if (!selectTarget())
        return;
    m_diveTable->setRowCount(0);
    m_listInfo->clear();
    setBusy(true, tr("Requesting the dive list…"));
    m_progress->setRange(0, 0);
    m_client->fetchDiveList();
}

void MainWindow::onDiveListEntry(const qtwin::DiveListEntry &entry)
{
    const int row = m_diveTable->rowCount();
    m_diveTable->insertRow(row);
    auto *idItem = new QTableWidgetItem(entry.id);
    idItem->setData(DiveIdRole, entry.id);
    m_diveTable->setItem(row, 0, idItem);
    m_diveTable->setItem(row, 1, new QTableWidgetItem(entry.start.isValid() ? entry.start.date().toString(Qt::ISODate)
                                                                            : QString()));
    m_diveTable->setItem(row, 2, new QTableWidgetItem(entry.start.isValid() ? entry.start.time().toString(QStringLiteral("HH:mm:ss"))
                                                                            : QString()));
    m_diveTable->setItem(row, 3, new QTableWidgetItem(entry.durationSeconds >= 0
                                                          ? tr("%1 min").arg(entry.durationSeconds / 60)
                                                          : QString()));
    m_diveTable->setItem(row, 4, new QTableWidgetItem(entry.maxDepth >= 0
                                                          ? tr("%1 m").arg(entry.maxDepth)
                                                          : QString()));
    idItem->setToolTip(entry.raw);
    m_status->setText(tr("Reading the dive list: %1 dive(s)…").arg(row + 1));
}

void MainWindow::downloadSelected()
{
    QStringList ids;
    for (int row = 0; row < m_diveTable->rowCount(); ++row) {
        if (m_diveTable->item(row, 0)->isSelected())
            ids << m_diveTable->item(row, 0)->data(DiveIdRole).toString();
    }
    startDownload(ids);
}

void MainWindow::downloadAll()
{
    QStringList ids;
    for (int row = 0; row < m_diveTable->rowCount(); ++row)
        ids << m_diveTable->item(row, 0)->data(DiveIdRole).toString();
    startDownload(ids);
}

void MainWindow::startDownload(const QStringList &diveIds)
{
    if (diveIds.isEmpty() || !selectTarget())
        return;
    m_exported = 0;
    m_exportFailures = 0;
    saveSettings();
    setBusy(true);
    m_client->downloadDives(diveIds);
}

void MainWindow::onDiveDownloaded(const QString &diveId, const QStringList &responses)
{
    const qtwin::ExportResult result = qtwin::exportDive(diveId, responses, m_outputDir->text());
    if (result.ok) {
        ++m_exported;
        log(tr("Dive %1: saved %2").arg(diveId, QDir::toNativeSeparators(result.uddfPath)));
    } else {
        ++m_exportFailures;
        log(tr("Dive %1: export failed: %2").arg(diveId, result.error));
        if (!result.rawPath.isEmpty())
            log(tr("  Raw data kept in %1").arg(QDir::toNativeSeparators(result.rawPath)));
    }
    for (const QString &warning : result.warnings)
        log(tr("  Warning: %1").arg(warning));
}

void MainWindow::endPcConnection()
{
    if (!selectTarget())
        return;
    setBusy(true, tr("Sending STOP…"));
    m_progress->setRange(0, 0);
    m_client->sendStop();
}

void MainWindow::chooseOutputFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Output folder"), m_outputDir->text());
    if (!dir.isEmpty())
        m_outputDir->setText(dir);
}

void MainWindow::setBusy(bool busy, const QString &status)
{
    m_busy = busy;
    m_status->setText(status);
    if (!busy) {
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
    }
    updateActions();
}

void MainWindow::updateActions()
{
    const bool hasDevice = m_deviceList->currentItem() != nullptr;
    const bool hasDives = m_diveTable->rowCount() > 0;
    m_searchButton->setEnabled(!m_busy);
    m_deviceList->setEnabled(!m_busy);
    m_channel->setEnabled(!m_busy);
    m_loadListButton->setEnabled(!m_busy && hasDevice);
    m_downloadSelectedButton->setEnabled(!m_busy && hasDevice && !m_diveTable->selectedItems().isEmpty());
    m_downloadAllButton->setEnabled(!m_busy && hasDevice && hasDives);
    m_stopButton->setEnabled(!m_busy && hasDevice);
    m_cancelButton->setEnabled(m_busy);
    m_outputDir->setEnabled(!m_busy);
}

void MainWindow::log(const QString &message)
{
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message));
}
