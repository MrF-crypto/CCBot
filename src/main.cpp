#include <QApplication>
#include <QFont>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#include <QMessageBox>
#include "gui/main_window.h"

int main(int argc, char* argv[]) {
    QApplication::setStyle("Fusion");  // 让深色 QSS 完全接管，避免下拉框/勾选框等控件
                                       // 回退到系统原生浅色渲染，出现"一亮一暗"的效果
    QApplication app(argc, argv);
    app.setApplicationName("CCG合约监控");
    app.setOrganizationName("CCGMonitor");

    // 单实例锁：同一台机器双开会各自独立决策、对同一账户重复下单。
    // QLockFile 自带陈旧锁检测（崩溃残留的锁会被自动接管），正常运行的实例则拒绝二开。
    // 锁放数据目录（便携模式=程序目录data/）：同一份数据目录只允许一个实例
    QString lockDir = ccg::MainWindow::portable_data_dir();
    QLockFile instanceLock(lockDir + "/ccbot.lock");
    if (!instanceLock.tryLock(100)) {
        QMessageBox::critical(nullptr, "CCG 合约监控",
            "程序已经在运行中（同一账户双开会重复下单）。\n"
            "如果确认没有其他实例，删除锁文件后重试:\n" + lockDir + "/ccbot.lock");
        return 1;
    }

#ifdef Q_OS_MAC
    QFont f("Menlo", 12);      // macOS 没有 Consolas，Menlo 是系统等宽字体
#else
    QFont f("Consolas", 10);
#endif
    app.setFont(f);

    ccg::MainWindow w;
    w.show();
    return app.exec();
}
