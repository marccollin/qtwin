#pragma once

#include "dtwinclient.h"
#include "protocol.h"

#include <QBluetoothDeviceInfo>
#include <QList>
#include <QMainWindow>

class QBluetoothDeviceDiscoveryAgent;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void loadSettings();
    void saveSettings() const;

    void searchDevices();
    void onDeviceDiscovered(const QBluetoothDeviceInfo &info);
    void refreshDeviceList();
    void onDeviceSelectionChanged();

    void loadDiveList();
    void downloadSelected();
    void downloadAll();
    void startDownload(const QStringList &diveIds);
    void endPcConnection();
    void chooseOutputFolder();

    void onDiveListEntry(const qtwin::DiveListEntry &entry);
    void onDiveDownloaded(const QString &diveId, const QStringList &responses);

    bool selectTarget();
    void setBusy(bool busy, const QString &status = QString());
    void updateActions();
    void log(const QString &message);

    DTwinClient *m_client = nullptr;
    QBluetoothDeviceDiscoveryAgent *m_discovery = nullptr;
    QList<QBluetoothDeviceInfo> m_devices;
    bool m_busy = false;
    int m_exported = 0;
    int m_exportFailures = 0;

    QPushButton *m_searchButton = nullptr;
    QCheckBox *m_showAllDevices = nullptr;
    QListWidget *m_deviceList = nullptr;
    QSpinBox *m_channel = nullptr;

    QPushButton *m_loadListButton = nullptr;
    QLabel *m_listInfo = nullptr;
    QTableWidget *m_diveTable = nullptr;
    QPushButton *m_downloadSelectedButton = nullptr;
    QPushButton *m_downloadAllButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_stopButton = nullptr;

    QLineEdit *m_outputDir = nullptr;

    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QCheckBox *m_showTraffic = nullptr;
    QPlainTextEdit *m_log = nullptr;
};
