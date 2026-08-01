#include <QApplication>
#include <QFont>
#include "gui/main_window.h"

int main(int argc, char* argv[]) {
    QApplication::setStyle("Fusion");  // 让深色 QSS 完全接管，避免下拉框/勾选框等控件
                                       // 回退到系统原生浅色渲染，出现"一亮一暗"的效果
    QApplication app(argc, argv);
    app.setApplicationName("CCG合约监控");
    app.setOrganizationName("CCGMonitor");

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
