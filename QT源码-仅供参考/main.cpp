#include <QApplication>
#include <QCoreApplication>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QTgame"));
    QApplication::setOrganizationName(QStringLiteral("QTgameThunder"));

    auto *mainWin = new MainWindow();
    QObject::connect(&app, &QApplication::aboutToQuit, mainWin, &MainWindow::shutdownGameTimers);
    mainWin->show();
    const int rc = app.exec();

    QObject::disconnect(&app, nullptr, mainWin, nullptr);
    mainWin->hide();
    mainWin->shutdownGameTimers();
    delete mainWin;

    return rc;
}
