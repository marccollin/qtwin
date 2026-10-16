#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("qtwin"));
    QApplication::setApplicationName(QStringLiteral("qtwin"));
    QApplication::setApplicationVersion(QStringLiteral(QTWIN_VERSION));

    MainWindow window;
    window.show();
    return QApplication::exec();
}
