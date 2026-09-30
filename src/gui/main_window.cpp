#include "gui/main_window.h"
#include "version.h"
#include "core/key_store.h"
#include "net/alert.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QSplitter>
#include <QMessageBox>
#include <QScrollArea>
#include <QStackedWidget>
#include <QDateTime>
#include <QColor>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QScrollBar>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenu>
#include <QAction>
#include <QListWidgetItem>
#include <QFormLayout>

#include <thread>
#include <future>
#include <set>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdlib>          // std::_Exit

namespace ccg {

// 退出时等下单线程池排空的预算。按【单个下单任务的最坏耗时】定：
// 一次 place_market 最坏是 POST 超时 10s → 空响应 → 查单恢复 3×(600ms+10s) ≈ 42s，
// 再叠上 -1111 精度重试的外层四轮可到约 168s。取 60 秒覆盖常见的坏网络情形，
// 同时把"关窗口像卡死"的时长控制在可接受范围；真超时了走 _Exit 而不是硬等到底
static constexpr int kShutdownDrainMs = 60000;

// ── 暗色主题 ──────────────────────────────────────────────────────────────────
// 统一走深色调色板，避免任何控件（下拉框弹出层、勾选框等）回退到系统默认的
// 浅色原生样式，跟深色背景撞出"一亮一暗"的效果
static const char* DARK_QSS = R"(
QMainWindow,QWidget{background:#0d1117;color:#e6edf3;font-family:Consolas;font-size:12px;}
QGroupBox{border:1px solid #30363d;border-radius:6px;margin-top:10px;padding-top:10px;}
QGroupBox::title{color:#58a6ff;subcontrol-origin:margin;left:8px;padding:0 4px;font-weight:bold;}
QLabel{background:transparent;}

QPushButton{background:#21262d;color:#e6edf3;border:1px solid #30363d;
            border-radius:4px;padding:4px 12px;}
QPushButton:hover{background:#2d333b;border-color:#484f58;}
QPushButton:pressed{background:#1c2128;}
QPushButton:disabled{background:#161b22;color:#484f58;border-color:#21262d;}

QLineEdit,QComboBox{background:#161b22;color:#e6edf3;border:1px solid #30363d;
                    border-radius:4px;padding:3px 6px;selection-background-color:#1f6feb;}
QLineEdit:hover,QComboBox:hover{border-color:#484f58;}
QLineEdit:focus,QComboBox:focus{border-color:#58a6ff;}
QLineEdit:disabled{color:#484f58;background:#0d1117;}
QComboBox::drop-down{border:none;width:20px;}
QComboBox::down-arrow{width:8px;height:8px;}
QComboBox QAbstractItemView{background:#161b22;color:#e6edf3;border:1px solid #30363d;
                            outline:none;selection-background-color:#1f6feb;
                            selection-color:#ffffff;padding:2px;}

QCheckBox{spacing:6px;background:transparent;}
QCheckBox::indicator{width:14px;height:14px;border:1px solid #30363d;border-radius:3px;
                     background:#161b22;}
QCheckBox::indicator:hover{border-color:#58a6ff;}
QCheckBox::indicator:checked{background:#1f6feb;border-color:#1f6feb;}

QTableWidget{background:#0d1117;color:#8b949e;gridline-color:#161b22;
            border:1px solid #30363d;border-radius:6px;}
QHeaderView::section{background:#161b22;color:#8b949e;border:none;
                     border-bottom:1px solid #30363d;padding:5px 4px;font-weight:bold;}
QHeaderView::section:hover{background:#1c2128;color:#e6edf3;}
QTableWidget::item{border:none;padding:2px;}
QTableWidget::item:selected{background:#1c2128;color:#e6edf3;}
QTableCornerButton::section{background:#161b22;border:none;}

QTextEdit{background:#0d1117;color:#8b949e;border:1px solid #30363d;border-radius:6px;}

QSplitter::handle{background:#0d1117;}
QSplitter::handle:horizontal{width:6px;}
QSplitter::handle:vertical{height:6px;}
QSplitter::handle:hover{background:#21262d;}

QScrollBar:vertical{background:transparent;width:9px;border:none;margin:0;}
QScrollBar::handle:vertical{background:#30363d;border-radius:4px;min-height:24px;}
QScrollBar::handle:vertical:hover{background:#484f58;}
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}
QScrollBar:horizontal{background:transparent;height:9px;border:none;margin:0;}
QScrollBar::handle:horizontal{background:#30363d;border-radius:4px;min-width:24px;}
QScrollBar::handle:horizontal:hover{background:#484f58;}
QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;}

QToolTip{background:#1c2128;color:#e6edf3;border:1px solid #30363d;padding:4px 6px;}
)";

// ─────────────────────────────────────────────────────────────────────────────
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    // 引擎专用：下单/平仓/灾难止损单的挂撤。与 headless 对齐为 4——
    // 此前 GUI 是 2 而 headless 是 4，两边【正好写反了】，而 GUI 才是跑几十个
    // bot 的那一端。注释写着"绝不排队"，但 2 个线程配 47 个 bot 做不到：
    // 这个池还兼跑灾难止损单的挂/撤（每次补仓成交都要撤旧挂新两次 HTTP），
    // 全市场同时止盈时手动平仓会排在后面，而一次 place_market 最坏 42 秒
    // （POST 超时 10s + 查单恢复 3×10s）。
    // 上限来自签名连接池 kCurlPoolSize=6：取 4 仍留 2 个槽给 tick 循环的同步调用
    , pool_(std::make_shared<ThreadPool>(4))
    , fetchPool_(std::make_shared<ThreadPool>(4))   // 数据拉取专用：慢任务全在这
{
    setWindowTitle(QString("TradingBot  %1").arg(ccbot::kVersion));
    resize(1200, 800);
    qApp->setStyleSheet(DARK_QSS);
    buildUi();
    migrate_appdata_if_needed();   // 老版本AppData数据一次性搬到程序目录data/
    load_credentials();
    load_trades();
    load_settings();
    refreshStats();

    // 若本地已保存有效凭证，启动后自动连接，无需再手动点击
    if (!apiKey_.trimmed().isEmpty() && !apiSecret_.trimmed().isEmpty()) {
        QTimer::singleShot(0, this, &MainWindow::onConnect);
    }
}

MainWindow::~MainWindow() {
    // ① 先切断新任务的来源，否则下面等排空时还在源源不断地进新任务
    if (ticker_)     ticker_->stop();
    if (tick_timer_) tick_timer_->stop();
    if (ob_timer_)   ob_timer_->stop();
    if (header_timer_) header_timer_->stop();

    // ② 等在途任务跑完【再落盘】。这一步同时解决两个问题：
    //
    //   a) use-after-free：任务的 lambda 捕获的是 this 和引擎的裸指针，而引擎里
    //      pool_ 的声明位置在 mtx_/bots_ 之前 —— 意味着线程池 join 的时候那两个
    //      成员已经析构，在途任务一访问就是 UAF。等排空之后再让成员开始销毁，
    //      这条路径就不存在了。
    //
    //   b) 在途订单的结果能被落盘：原先是先 save_bots() 再让成员销毁，所以
    //      关窗口瞬间正在成交的那笔本地没有记录，只能靠下次启动对账去认领——
    //      而认领会丢掉层数信息（按交易所均价重建成单层）。现在等它落地再存。
    //
    // 预算必须按【单个下单任务的最坏耗时】定，不是按单次 HTTP。原先取 20 秒、
    // 注释写的理由是"单笔 HTTP 最长 15 秒"——两处都不对：下单走签名连接池，
    // 超时是 10 秒；而一次 place_market 会串起很多次 HTTP：
    //     POST 超时 10s → 空响应 → 查单恢复 3×(600ms+10s) ≈ 42s
    // 坏网络下 42 秒是很平常的情形，20 秒远远不够。
    const bool drained = (!pool_ || pool_->wait_idle(kShutdownDrainMs));
    if (fetchPool_) fetchPool_->wait_idle(5000);   // 数据拉取不影响资金，等短一点

    // ③ 此刻状态最完整，落盘
    if (tradesDirty_) save_trades(true);
    if (trend_engine_) save_trend_bots();

    // ④ 没排空就【不要】走正常析构路径。
    //
    // 在途任务捏着引擎的裸指针，而成员逆序析构会先销毁 bots_/mtx_——
    // 任务一访问就是 use-after-free。v4.0.1 加"等排空"正是为了消除它，但超时
    // 分支只打了条日志就继续往下走，等于把那个窗口原样放了回来。
    //
    // 此刻状态已经尽力保存了，剩下唯一还能做对的事就是别再碰内存：
    // 直接结束进程，不跑析构、不跑 atexit。丢掉的只是优雅退出，换来的是
    // 绝不在已释放的对象上执行代码
    if (!drained) {
        log("退出时仍有下单任务未完成，跳过清理直接结束进程（避免访问已释放内存）；"
            "在途成交由下次启动的对账兜底", "WARN");
        std::_Exit(0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 路径辅助（便携模式）
// ─────────────────────────────────────────────────────────────────────────────
// 数据目录 = 程序所在目录下的 data 子目录：策略配置、交易明细、API凭证、日志
// 全部集中在这里——整个程序文件夹拷走就是完整备份（API凭证仍是DPAPI加密，换机
// 需重新输入）。程序目录不可写时（如装进 Program Files）自动回退到系统 AppData
QString MainWindow::portable_data_dir() {
    static QString cached;
    if (!cached.isEmpty()) return cached;

    QString portable = QCoreApplication::applicationDirPath() + "/data";
    if (QDir().mkpath(portable)) {
        QFile probe(portable + "/.write_test");
        if (probe.open(QIODevice::WriteOnly)) {
            probe.close();
            probe.remove();
            cached = portable;
            return cached;
        }
    }
    QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(fallback);
    cached = fallback;
    return cached;
}

// 一次性迁移：老版本数据在系统 AppData 里，新数据目录还是空的话自动搬过来，
// 升级用户的策略/明细/凭证无感继承
void MainWindow::migrate_appdata_if_needed() {
    QString dst = portable_data_dir();
    QString old = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dst == old) return;                          // 回退模式，无需迁移
    if (QFile::exists(dst + "/.migrated")) return;   // 专用标记：全部文件迁移成功才写

    // 逐文件"缺了才拷"：部分失败（文件被锁/杀软拦截）时下次启动会重试剩余文件，
    // 不会出现"密钥拷过来了、仓位跟踪永久丢失"的半截迁移
    int n = 0;
    bool all_ok = true;
    for (const char* f : {"ccg_creds.dat", "ccg_bots.json", "ccg_trades.json",
                          "ccg_settings.json"}) {
        QString src = old + "/" + f;
        QString to  = dst + "/" + f;
        if (!QFile::exists(src) || QFile::exists(to)) continue;
        if (QFile::copy(src, to)) ++n;
        else all_ok = false;
    }
    if (all_ok) {
        // 标记文件建不出来（目录只读/磁盘满）必须说出来：它是"迁移已完成"的唯一
        // 凭据，建不上的话每次启动都会把整个旧目录重新拷一遍，而用户看不到原因
        QFile marker(dst + "/.migrated");
        if (!marker.open(QIODevice::WriteOnly))
            log("迁移完成标记写入失败，下次启动会重复迁移一次（不影响数据）", "WARN");
    } else {
        log("旧数据目录部分文件迁移失败，下次启动将重试剩余文件", "WARN");
    }
    if (n > 0)
        log(QString("已从旧数据目录迁移 %1 个文件到程序目录 data/（原文件保留在 %2）")
            .arg(n).arg(old), "OK");
}

std::string MainWindow::cred_path() const {
    return (portable_data_dir() + "/ccg_creds.dat").toStdString();
}

std::string MainWindow::bot_cfg_path() const {
    return (portable_data_dir() + "/ccg_bots.json").toStdString();
}

std::string MainWindow::trade_path() const {
    return (portable_data_dir() + "/ccg_trades.json").toStdString();
}

std::string MainWindow::settings_path() const {
    return (portable_data_dir() + "/ccg_settings.json").toStdString();
}

std::string MainWindow::funding_path() const {
    return (portable_data_dir() + "/ccg_funding.json").toStdString();
}

std::string MainWindow::log_path() const {
    QString dir = portable_data_dir() + "/logs";
    QDir().mkpath(dir);
    QString file = QDateTime::currentDateTime().toString("yyyyMMdd");
    return (dir + "/ccg_" + file + ".log").toStdString();
}

// ─────────────────────────────────────────────────────────────────────────────
// 持久化：API Key
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::save_credentials() {
    KeyStore::Creds c;
    c.api_key    = apiKey_.trimmed().toStdString();
    c.api_secret = apiSecret_.trimmed().toStdString();
    c.testnet    = testnet_;
    c.account_mode = accountMode_;
    if (c.api_key.empty() || c.api_secret.empty()) return;
    if (!KeyStore::save(c, cred_path()))
        log("API Key 本地保存失败（DPAPI 加密或写盘出错）", "ERR");
}

void MainWindow::load_credentials() {
    KeyStore::Creds c;
    if (!KeyStore::load(c, cred_path())) return;
    apiKey_    = QString::fromStdString(c.api_key);
    apiSecret_ = QString::fromStdString(c.api_secret);
    testnet_     = c.testnet;
    accountMode_ = c.account_mode;
    log(accountMode_ == 1 ? "已自动载入保存的 API Key（统一账户）"
                          : "已自动载入保存的 API Key", "OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// 持久化：全局设置（账户级总保证金上限 / 警报 webhook）
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::save_settings() {
    QJsonObject o;
    o["max_total_margin"] = maxTotalMargin_;
    o["alert_webhook"]    = alertWebhook_;
    QSaveFile f(QString::fromStdString(settings_path()));   // 原子保存
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson());
        f.commit();
    }
}

void MainWindow::load_settings() {
    QFile f(QString::fromStdString(settings_path()));
    if (!f.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return;
    auto o = doc.object();
    maxTotalMargin_ = o["max_total_margin"].toDouble(0.0);
    alertWebhook_   = o["alert_webhook"].toString();
}

// ─────────────────────────────────────────────────────────────────────────────
// 持久化：时间戳互转
// ─────────────────────────────────────────────────────────────────────────────
// 原先这一段是 save_bots / load_and_restore_bots（网格DCA 的配置与仓位落盘）。
// DCA 移除后只剩这两个小工具还有人用（成交明细落盘），趋势策略的落盘走
// trend_panel.cpp 里的 save_trend_bots / load_and_restore_trend。
static qint64 tp_to_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}
static std::chrono::system_clock::time_point ms_to_tp(qint64 ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}


// ─────────────────────────────────────────────────────────────────────────────
// 持久化：交易明细
// ─────────────────────────────────────────────────────────────────────────────
// 成交明细落盘。
// 两处长跑保护：
//  ① trades_ 有上限（kMaxTrades），超出丢弃最早的——否则向量无限增长，
//     而且这里是【全量重写】，1万条以后每平一笔仓都要把1万条重新序列化一遍
//  ② 写盘去抖：密集平仓（多品种同时止盈）时合并成一次写，不要每笔都落盘
void MainWindow::save_trades(bool force) {
    if (trades_.size() > kMaxTrades) {
        const size_t drop = trades_.size() - kMaxTrades;
        trades_.erase(trades_.begin(), trades_.begin() + drop);
        log(QString("成交记录已达 %1 条上限，丢弃最早的 %2 条（完整历史见日志文件）")
            .arg(kMaxTrades).arg(drop), "WARN");
    }

    if (!force) {
        // 距上次落盘不足 kTradeSaveMinMs 就只置脏，交给后面的写入合并
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (lastTradeSaveMs_ != 0 && now - lastTradeSaveMs_ < kTradeSaveMinMs) {
            tradesDirty_ = true;
            return;
        }
        lastTradeSaveMs_ = now;
    }
    tradesDirty_ = false;

    QJsonArray arr;
    for (const auto& t : trades_) {
        QJsonObject o;
        o["symbol"]      = QString::fromStdString(t.symbol);
        // "side" 而不是原来的 "direction"：值域也变了（0=Flat 1=Long 2=Short，
        // 来自 trend::Pos），沿用旧键名会让老文件的 0/1/2 被按新含义读出来——
        // 多空显示整体错位。换个键名，老值自然落到 read 的默认分支上
        o["side"]        = (int)t.side;
        o["entry_price"] = t.entry_price;
        o["exit_price"]  = t.exit_price;
        o["qty"]         = t.qty;
        o["pnl"]         = t.pnl;
        o["reversed"]    = t.reversed;
        o["reason"]      = QString::fromStdString(t.reason);
        o["close_time_ms"] = tp_to_ms(t.close_time);
        arr.append(o);
    }
    QSaveFile f(QString::fromStdString(trade_path()));   // 原子保存，崩溃不损坏交易明细
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(arr).toJson());
        f.commit();
    }
}

void MainWindow::load_trades() {
    QFile f(QString::fromStdString(trade_path()));
    if (!f.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;
    trades_.clear();
    for (const auto& v : doc.array()) {
        auto o = v.toObject();
        TrendTrade t;
        t.symbol      = o["symbol"].toString().toStdString();
        // 老文件（网格DCA 时代）只有 "direction"，没有 "side"。这里刻意【不去
        // 迁移】那个值：两者值域不同（0/1/2 分别是 Long/Short/Both 与
        // Flat/Long/Short），硬翻译只会把历史记录的多空标错。缺 side 时落到
        // Flat，界面显示为 "--"——"不知道"比"标错"好
        t.side        = (trend::Pos)o["side"].toInt((int)trend::Pos::Flat);
        t.entry_price = o["entry_price"].toDouble();
        t.exit_price  = o["exit_price"].toDouble();
        t.qty         = o["qty"].toDouble();
        t.pnl         = o["pnl"].toDouble();
        t.reversed    = o["reversed"].toBool(false);
        t.reason      = o["reason"].toString().toStdString();
        t.close_time  = ms_to_tp((qint64)o["close_time_ms"].toDouble());
        trades_.push_back(t);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 盈利统计
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshStats() {
    if (!statsLabel_) return;
    double totalPnl = 0;
    int wins = 0;
    for (const auto& t : trades_) {
        totalPnl += t.pnl;
        if (t.pnl > 0) ++wins;
    }
    double winRate = trades_.empty() ? 0.0 : (double)wins / trades_.size() * 100.0;
    QColor c = (totalPnl >= 0) ? QColor("#3fb950") : QColor("#f85149");
    statsLabel_->setText(
        QString("盈利统计：共 %1 笔 | 胜率 %2% | 累计盈亏 %3$%4")
        .arg(trades_.size())
        .arg(winRate, 0, 'f', 1)
        .arg(totalPnl >= 0 ? "+" : "")
        .arg(std::abs(totalPnl), 0, 'f', 2));
    statsLabel_->setStyleSheet(QString("color:%1;font-size:11px;").arg(c.name()));

    if (pnlBadge_) {
        pnlBadge_->setText(QString("盈亏 %1$%2").arg(totalPnl >= 0 ? "+" : "")
                            .arg(std::abs(totalPnl), 0, 'f', 2));
        QString bg = (totalPnl > 0) ? "#1a3d1a" : (totalPnl < 0) ? "#3d1a1a" : "#21262d";
        pnlBadge_->setStyleSheet(QString(
            "QLabel{color:%1;font-size:11px;background:%2;border-radius:8px;padding:2px 8px;}")
            .arg(c.name(), bg));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 交易明细弹窗
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::openTradeHistoryDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("交易明细与周期统计");
    dlg.resize(860, 620);
    auto* dv = new QVBoxLayout(&dlg);

    auto mkc = [](const QString& s, const QColor& c) {
        auto* it = new QTableWidgetItem(s);
        it->setTextAlignment(Qt::AlignCenter);
        it->setForeground(c);
        return it;
    };

    // ── 周期统计（按品种汇总，验证闭环的四个核心指标一目了然）────────────────
    auto* statsTitle = new QLabel("周期统计（按品种）");
    statsTitle->setStyleSheet("color:#58a6ff;font-size:11px;font-weight:bold;");
    dv->addWidget(statsTitle);

    // 统计口径随策略改了：原先是"平均层数/最大层数"（网格DCA 的调参指标），
    // 现在是【盈亏比】与【反手次数】。趋势策略的典型画像是"胜率低、盈亏比高"，
    // 单看胜率会得出完全相反的结论——一条 35% 胜率的趋势线可能比 70% 胜率的
    // 更赚钱，判据必须是 胜率 × 盈亏比
    struct SymStat {
        int cycles = 0; int wins = 0; double pnl = 0;
        double win_sum = 0, loss_sum = 0;   // 盈亏比的分子分母
        int reversals = 0;
        std::chrono::system_clock::time_point first_close{}, last_close{};
    };
    std::map<std::string, SymStat> stats;
    for (const auto& t : trades_) {
        auto& s = stats[t.symbol];
        if (s.cycles == 0) s.first_close = t.close_time;
        s.cycles++;
        if (t.pnl > 0) { s.wins++; s.win_sum += t.pnl; }
        else           { s.loss_sum += -t.pnl; }
        s.pnl += t.pnl;
        if (t.reversed) s.reversals++;
        s.last_close = t.close_time;
    }

    auto* statTable = new QTableWidget((int)stats.size(), 7);
    statTable->setHorizontalHeaderLabels(
        {"品种","笔数","笔/周","胜率","累计盈亏","盈亏比","反手"});
    statTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    statTable->verticalHeader()->setVisible(false);
    statTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    statTable->setMaximumHeight(46 + (int)stats.size() * 30);
    int row = 0;
    for (const auto& [sym, s] : stats) {
        double weeks = std::max(1.0 / 7.0,
            std::chrono::duration_cast<std::chrono::hours>(s.last_close - s.first_close).count()
                / 24.0 / 7.0);
        double perWeek  = (s.cycles > 1) ? (s.cycles - 1) / weeks : 0;
        double winRate  = s.cycles ? 100.0 * s.wins / s.cycles : 0;
        // 平均盈利 / 平均亏损。分母为 0（还没亏过）时给不出有意义的比值，
        // 显示 "--" 而不是填一个 inf 或 0 冒充结论
        const int losses = s.cycles - s.wins;
        const bool pr_ok = (s.wins > 0 && losses > 0 && s.loss_sum > 0);
        const double payoff = pr_ok ? (s.win_sum / s.wins) / (s.loss_sum / losses) : 0.0;
        statTable->setItem(row, 0, mkc(QString::fromStdString(sym), QColor("#e6edf3")));
        statTable->setItem(row, 1, mkc(QString::number(s.cycles), QColor("#8b949e")));
        statTable->setItem(row, 2, mkc(s.cycles > 1 ? QString::number(perWeek, 'f', 1) : "--",
                                        QColor("#8b949e")));
        statTable->setItem(row, 3, mkc(QString("%1%").arg(winRate, 0, 'f', 0), QColor("#8b949e")));
        statTable->setItem(row, 4, mkc(QString("%1$%2").arg(s.pnl >= 0 ? "+" : "")
                                        .arg(std::abs(s.pnl), 0, 'f', 2),
                                        s.pnl >= 0 ? QColor("#3fb950") : QColor("#f85149")));
        statTable->setItem(row, 5, mkc(pr_ok ? QString::number(payoff, 'f', 2) : "--",
                                        !pr_ok              ? QColor("#8b949e")
                                        : (payoff >= 2.0)   ? QColor("#3fb950")
                                        : (payoff >= 1.0)   ? QColor("#d29922")
                                                            : QColor("#f85149")));
        statTable->setItem(row, 6, mkc(s.reversals > 0 ? QString::number(s.reversals) : "--",
                                        QColor("#8b949e")));
        ++row;
    }
    dv->addWidget(statTable);

    auto* statHint = new QLabel(
        "调参提示：趋势策略的典型画像是【胜率低、盈亏比高】——单看胜率会得出相反的结论，"
        "35% 胜率配 3.0 盈亏比比 70% 胜率配 0.4 赚得多。判据是 胜率 × 盈亏比 > 1。"
        "盈亏比长期 <1 → k×ATR 止损太紧（被噪音打掉）或周期太短；"
        "反手次数占比高 → 当前是震荡市，这套策略在震荡市本来就该少做。"
        "笔/周 × 平均盈亏 = 该品种的真实产能，而笔数越多手续费拖累越大。");
    statHint->setWordWrap(true);
    statHint->setStyleSheet("color:#8b949e;font-size:10px;");
    dv->addWidget(statHint);

    // ── 交易明细 ─────────────────────────────────────────────────────────────
    auto* histTitle = new QLabel("交易明细");
    histTitle->setStyleSheet("color:#58a6ff;font-size:11px;font-weight:bold;padding-top:6px;");
    dv->addWidget(histTitle);

    auto* table = new QTableWidget(0, 9);
    table->setHorizontalHeaderLabels(
        {"时间","品种","方向","开仓价","平仓价","数量","盈亏","反手","原因"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    dv->addWidget(table);

    table->setRowCount((int)trades_.size());
    for (int i = 0; i < (int)trades_.size(); ++i) {
        // 最新的排在最上面
        const auto& t = trades_[trades_.size() - 1 - i];
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(tp_to_ms(t.close_time));
        table->setItem(i, 0, mkc(dt.toString("MM-dd HH:mm:ss"), QColor("#8b949e")));
        table->setItem(i, 1, mkc(QString::fromStdString(t.symbol), QColor("#e6edf3")));
        // 方向来自本笔的运行时持仓方向。Flat 只会出现在【旧版文件】读进来的
        // 记录上（那时这个字段叫 direction、值域也不同，见 load_trades），
        // 显示 "--" 而不是猜一个方向
        table->setItem(i, 2, mkc(t.side == trend::Pos::Short ? "空"
                                 : t.side == trend::Pos::Long ? "多" : "--",
                                  t.side == trend::Pos::Short ? QColor("#f85149")
                                  : t.side == trend::Pos::Long ? QColor("#3fb950")
                                                             : QColor("#8b949e")));
        table->setItem(i, 3, mkc(QString("$%1").arg(t.entry_price, 0, 'f', 4), QColor("#8b949e")));
        table->setItem(i, 4, mkc(QString("$%1").arg(t.exit_price,  0, 'f', 4), QColor("#8b949e")));
        table->setItem(i, 5, mkc(QString::number(t.qty, 'f', 4), QColor("#8b949e")));
        table->setItem(i, 6, mkc(QString("%1$%2").arg(t.pnl >= 0 ? "+" : "").arg(std::abs(t.pnl), 0, 'f', 2),
                                  t.pnl >= 0 ? QColor("#3fb950") : QColor("#f85149")));
        table->setItem(i, 7, mkc(t.reversed ? "是" : "--",
                                  t.reversed ? QColor("#d29922") : QColor("#8b949e")));
        table->setItem(i, 8, mkc(QString::fromStdString(t.reason), QColor("#8b949e")));
    }

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close);
    dv->addWidget(btnBox);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox->button(QDialogButtonBox::Close), &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

// ─────────────────────────────────────────────────────────────────────────────
// 全局设置弹窗：账户级总保证金上限 + 关键事件外部提醒 webhook
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::openSettingsDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("设置");
    dlg.resize(480, 460);
    auto* dv = new QVBoxLayout(&dlg);

    // ── 账户凭证 ─────────────────────────────────────────────────────────────
    auto* credBox = new QGroupBox("账户凭证");
    credBox->setStyleSheet("QGroupBox{color:#58a6ff;font-size:11px;font-weight:bold;}");
    auto* credForm = new QFormLayout(credBox);
    credForm->setSpacing(8);

    auto* apiKeyEdit = new QLineEdit(apiKey_);
    apiKeyEdit->setEchoMode(QLineEdit::Password);
    apiKeyEdit->setPlaceholderText("Binance API Key");
    credForm->addRow("API Key:", apiKeyEdit);

    auto* secretEdit = new QLineEdit(apiSecret_);
    secretEdit->setEchoMode(QLineEdit::Password);
    secretEdit->setPlaceholderText("API Secret");
    credForm->addRow("Secret:", secretEdit);

    auto* modeCombo = new QComboBox();
    modeCombo->addItem("普通合约账户（fapi）");
    modeCombo->addItem("统一账户 / Portfolio Margin（papi）");
    modeCombo->setCurrentIndex(accountMode_ == 1 ? 1 : 0);
    credForm->addRow("账户类型:", modeCombo);

    auto* testnetBox = new QCheckBox("测试网（币安合约测试网，需要单独申请测试用Key）");
    testnetBox->setChecked(testnet_);
    credForm->addRow("", testnetBox);

    auto* modeHint = new QLabel();
    modeHint->setWordWrap(true);
    modeHint->setStyleSheet("color:#8b949e;font-size:10px;");
    credForm->addRow("", modeHint);

    // 统一账户只有主网——币安没有为它开测试网。选中时把测试网勾选强制关掉并禁用，
    // 免得用户以为可以先空跑验证，实际却拿主网的 Key 打到一个不存在的测试域名上
    auto syncModeUi = [modeCombo, testnetBox, modeHint]() {
        const bool pm = (modeCombo->currentIndex() == 1);
        if (pm) testnetBox->setChecked(false);
        testnetBox->setEnabled(!pm);
        modeHint->setText(pm
            ? "统一账户走 papi.binance.com，**没有测试网**，连上就是真金白银。"
              "开通统一账户后，普通合约的 API 端点会失效，两种类型不能混用同一个账户的 Key。"
            : "普通合约账户走 fapi.binance.com。若该账户已在币安开通统一账户，请改选上面那项。");
    };
    syncModeUi();
    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            &dlg, [syncModeUi](int) { syncModeUi(); });

    auto* credHint = new QLabel(
        connState_ == ConnState::Disconnected || connState_ == ConnState::Failed
            ? "改完保存后，回到主界面点【连接】生效。"
            : "已经连着的话，改完 Key 不会自动重连——保存后手动点一次【连接】才会用新的 Key。");
    credHint->setWordWrap(true);
    credHint->setStyleSheet("color:#8b949e;font-size:10px;");
    credForm->addRow(credHint);

    dv->addWidget(credBox);

    auto* form = new QFormLayout();
    form->setSpacing(10);

    auto* marginEdit = new QLineEdit(QString::number(maxTotalMargin_, 'f', 0));
    marginEdit->setPlaceholderText("0 = 不限");
    form->addRow("账户总保证金上限(USDT):", marginEdit);
    auto* marginHint = new QLabel("所有品种加起来占用的保证金超过这个数，就暂缓开新的首仓（已有仓位不受影响）");
    marginHint->setWordWrap(true);
    marginHint->setStyleSheet("color:#8b949e;font-size:10px;");
    form->addRow("", marginHint);

    auto* webhookEdit = new QLineEdit(alertWebhook_);
    webhookEdit->setPlaceholderText("https://api.telegram.org/bot<TOKEN>/sendMessage?chat_id=<ID>");
    form->addRow("警报 Webhook URL:", webhookEdit);
    auto* webhookHint = new QLabel(
        "以下事件会 POST 一条消息过去（{\"text\":\"...\"}）：账户连接失败、网络异常、"
        "对账发现本地与交易所不一致（含【交易所侧灾难止损被触发】——它是本策略唯一的"
        "止损，触发时本地收不到成交回调，只能由对账发现）。"
        "留空则不发送。支持 Telegram Bot / 企业微信・飞书自定义机器人等接受 JSON text 字段的 webhook。");
    webhookHint->setWordWrap(true);
    webhookHint->setStyleSheet("color:#8b949e;font-size:10px;");
    form->addRow("", webhookHint);

    auto* testBtn = new QPushButton("发送测试消息");
    form->addRow("", testBtn);
    connect(testBtn, &QPushButton::clicked, [this, webhookEdit]() {
        QString url = webhookEdit->text().trimmed();
        if (url.isEmpty()) { log("请先填写 Webhook URL 再测试", "WARN"); return; }
        run_async([this, url]() {
            std::string err;
            bool ok = send_webhook(url.toStdString(), "TradingBot 测试消息：webhook 配置成功", &err);
            QMetaObject::invokeMethod(this, [this, ok, err]() {
                log(ok ? "测试消息发送成功" : ("测试消息发送失败：" + QString::fromStdString(err)),
                    ok ? "OK" : "ERR");
            }, Qt::QueuedConnection);
        });
    });

    dv->addLayout(form);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    dv->addWidget(btnBox);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    apiKey_      = apiKeyEdit->text().trimmed();
    apiSecret_   = secretEdit->text().trimmed();
    accountMode_ = (modeCombo->currentIndex() == 1) ? 1 : 0;
    testnet_     = (accountMode_ == 1) ? false : testnetBox->isChecked();
    save_credentials();

    bool ok;
    double m = marginEdit->text().toDouble(&ok);
    maxTotalMargin_ = (ok && m > 0) ? m : 0.0;
    alertWebhook_   = webhookEdit->text().trimmed();

    if (trend_engine_) trend_engine_->set_max_total_margin(maxTotalMargin_);
    save_settings();
    log(QString("设置已保存：总保证金上限=%1  警报=%2")
        .arg(maxTotalMargin_ > 0 ? QString("$%1").arg(maxTotalMargin_,0,'f',0) : "不限")
        .arg(alertWebhook_.isEmpty() ? "未配置" : "已配置"), "OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// 日志
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::log(const QString& msg, const QString& level) {
    QString ts = QDateTime::currentDateTime().toString("[HH:mm:ss]");

    if (logBox_) {
        QString color = (level == "OK")   ? "#3fb950"
                      : (level == "WARN") ? "#d29922"
                      : (level == "ERR")  ? "#f85149"
                                          : "#8b949e";
        // appendHtml 保留按级别着色；滚动到底只在用户本来就贴着底部时做，
        // 否则往回翻日志时会被不断拽回去
        auto* sb = logBox_->verticalScrollBar();
        const bool atBottom = (sb->value() >= sb->maximum() - 4);
        logBox_->appendHtml(QString("<font color='%1'>%2 %3</font>")
                            .arg(color).arg(ts).arg(msg.toHtmlEscaped()));
        if (atBottom) sb->setValue(sb->maximum());
    }

    // 落盘，重启/崩溃后能复盘——按天分文件，QFile 原生支持中文路径，不会碰到
    // std::ofstream 那个 ANSI codepage 坑
    QFile lf(QString::fromStdString(log_path()));
    if (lf.open(QIODevice::Append | QIODevice::Text)) {
        QString line = QString("%1 [%2] %3\n").arg(ts, level, msg);
        lf.write(line.toUtf8());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 关键事件外部提醒：后台线程发送，绝不阻塞 GUI；没配置 webhook 时直接跳过
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::sendAlert(const QString& text) {
    if (alertWebhook_.trimmed().isEmpty()) return;
    QString url = alertWebhook_;
    run_async([url, text]() {
        send_webhook(url.toStdString(), text.toStdString());
    });
}

// 把对账不一致拼成【带明细】的告警正文。
// 只报条数是没用的：收到"发现 1 处不一致"还得去翻日志，而这条不一致可能就是
// 灾难止损被打掉了。最多列 5 条，超出的折叠成计数——webhook 有长度上限，
// 而真出大事时前几条已经足够说明性质
QString MainWindow::alert_text(const QString& what, const std::vector<std::string>& issues) {
    QString s = QString("[TradingBot] %1 发现 %2 处不一致：").arg(what).arg(issues.size());
    const int shown = std::min<int>((int)issues.size(), 5);
    for (int i = 0; i < shown; ++i)
        s += "\n· " + QString::fromStdString(issues[i]);
    if ((int)issues.size() > shown)
        s += QString("\n· …另有 %1 处，详见日志").arg((int)issues.size() - shown);
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// 异步执行（线程池）
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::run_async(std::function<void()> fn) {
    // 数据拉取走独立池——引擎的 pool_ 只跑下单/平仓，保证手动平仓点下去立即执行，
    // 不会排在指标/趋势/SR雷达这些几百毫秒~几秒的慢任务后面
    fetchPool_->submit(std::move(fn));
}

// ─────────────────────────────────────────────────────────────────────────────
// 构建 UI
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::buildUi() {
    auto* central = new QWidget();
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setSpacing(5);
    root->setContentsMargins(8, 8, 8, 8);

    // ── 顶部连接栏 ────────────────────────────────────────────────────────────
    // API Key/Secret/测试网挪进了"设置"弹窗，这里只留连接状态和几个一眼扫过去
    // 就想看到的账户数字
    {
        auto* row = new QHBoxLayout();

        btnConnect_ = new QPushButton("连接");
        btnConnect_->setFixedWidth(68);
        btnConnect_->setStyleSheet(
            "QPushButton{background:#1f3d6b;color:#58a6ff;font-weight:bold;}");
        connect(btnConnect_, &QPushButton::clicked, this, &MainWindow::onConnect);
        row->addWidget(btnConnect_);

        breatheDot_ = new QLabel();
        breatheDot_->setFixedSize(10, 10);
        breatheDot_->setStyleSheet("background:#484f58;border-radius:5px;");
        row->addWidget(breatheDot_);
        row->addSpacing(4);

        connLabel_ = new QLabel("未连接");
        connLabel_->setStyleSheet("color:#484f58;font-size:11px;");
        row->addWidget(connLabel_);
        row->addSpacing(16);

        equityLabel_ = new QLabel("权益: --   可用: --");
        equityLabel_->setStyleSheet("color:#8b949e;font-size:11px;");
        row->addWidget(equityLabel_);

        // 统一账户维持保证金率：统一账户按【全账户】算强平，只看子账户会低估风险。
        // 它和权益是同一类东西（账户级健康度），所以并排放、同样的字号
        mmrLabel_ = new QLabel();
        mmrLabel_->setStyleSheet("color:#8b949e;font-size:11px;");
        mmrLabel_->setVisible(false);          // 普通合约账户不适用，直接不占位
        row->addWidget(mmrLabel_);

        // 限流状态：只在被限速或被封禁时出现
        rateLabel_ = new QLabel();
        rateLabel_->setStyleSheet("color:#8b949e;font-size:11px;");
        rateLabel_->setVisible(false);
        row->addWidget(rateLabel_);

        // 账户累计资金费：真实划走的现金（非浮亏），只在非零时显示，避免挤占顶部栏
        fundLabel_ = new QLabel();
        fundLabel_->setStyleSheet("color:#8b949e;font-size:11px;");
        fundLabel_->setVisible(false);
        row->addWidget(fundLabel_);

        pnlBadge_ = new QLabel("盈亏 $0.00");
        pnlBadge_->setStyleSheet(
            "QLabel{color:#8b949e;font-size:11px;background:#21262d;"
            "border-radius:8px;padding:2px 8px;}");
        row->addSpacing(10);
        row->addWidget(pnlBadge_);

        row->addStretch();

        auto* btnSettings = new QPushButton("⚙ 设置");
        btnSettings->setFixedWidth(72);
        btnSettings->setStyleSheet(
            "QPushButton{background:#21262d;color:#8b949e;font-size:11px;}");
        connect(btnSettings, &QPushButton::clicked, this, &MainWindow::openSettingsDialog);
        row->addWidget(btnSettings);

        root->addLayout(row);
    }

    // ── 主体分割器（上：监控区 | 下：日志）────────────────────────────────────
    auto* vSplit = new QSplitter(Qt::Vertical);
    vSplit->setChildrenCollapsible(false);

    // ── 上层：单栏——加品种/统计 + 实盘监控表 ──────────────────────────────────
    {
        auto* tw = new QWidget();
        auto* tv = new QVBoxLayout(tw);
        tv->setSpacing(4);
        tv->setContentsMargins(0, 0, 0, 0);

        // 加品种（只需代币符号，自动补 USDT 永续） + 盈利统计 + 交易明细，同一行
        {
            auto* row = new QHBoxLayout();
            addSymbolEdit_ = new QLineEdit();
            addSymbolEdit_->setPlaceholderText("输入代币，如 BTC");
            addSymbolEdit_->setFixedWidth(160);
            connect(addSymbolEdit_, &QLineEdit::returnPressed, this, &MainWindow::onAddWatchSymbol);
            row->addWidget(addSymbolEdit_);

            auto* btnAddSym = new QPushButton("添加");
            connect(btnAddSym, &QPushButton::clicked, this, &MainWindow::onAddWatchSymbol);
            row->addWidget(btnAddSym);
            row->addStretch();

            statsLabel_ = new QLabel("盈利统计：共 0 笔 | 胜率 --% | 累计盈亏 $0.00");
            statsLabel_->setStyleSheet(
                "QLabel{color:#8b949e;font-size:11px;padding:4px 8px;"
                "background:#161b22;border:1px solid #21262d;border-radius:3px;}");
            row->addWidget(statsLabel_);

            auto* btnHistory = new QPushButton("交易明细");
            btnHistory->setStyleSheet("QPushButton{padding:3px 10px;}");
            connect(btnHistory, &QPushButton::clicked, this, &MainWindow::openTradeHistoryDialog);
            row->addWidget(btnHistory);
            tv->addLayout(row);
        }

        // 汇总标签 + 全部停止/清除已停
        {
            auto* row = new QHBoxLayout();
            summaryLabel_ = new QLabel(
                "运行中: 0   冷却: 0   已停止: 0   |   未实现: $0.00   已实现: $0.00");
            summaryLabel_->setStyleSheet(
                "QLabel{color:#8b949e;font-size:11px;padding:4px 8px;"
                "background:#161b22;border:1px solid #21262d;border-radius:3px;}");
            row->addWidget(summaryLabel_, 1);

            auto* btnStop = new QPushButton("全部停止");
            btnStop->setStyleSheet(
                "QPushButton{background:#3d1a1a;color:#f85149;padding:3px 10px;}");
            connect(btnStop, &QPushButton::clicked, this, &MainWindow::onStopAll);
            row->addWidget(btnStop);

            auto* btnClear = new QPushButton("清除已停");
            btnClear->setStyleSheet("QPushButton{padding:3px 8px;}");
            connect(btnClear, &QPushButton::clicked, this, &MainWindow::onClearStopped);
            row->addWidget(btnClear);
            tv->addLayout(row);
        }

        auto* monLbl = new QLabel("实盘监控  (右键品种进行策略配置)");
        monLbl->setStyleSheet("color:#58a6ff;font-size:11px;font-weight:bold;padding:2px 0;");
        tv->addWidget(monLbl);

        // Bot 表格。列号全部走 Col 枚举，不写字面量——理由见 main_window.h 的定义处。
        //
        // 资金费不进这张表：它是【账户级慢变量】（8小时才结算一次），而这张表是
        // 逐品种的实时行。混在一起既挤掉实时数据的宽度，也不符合它的性质——
        // 账户合计放顶部栏，逐品种细节放"开仓价"列的悬停提示
        //
        // ⚠ 两个止损【必须分成两列】。它们语义完全不同：
        //     移动止损  活在进程里，每 tick 棘轮推进，进程死了就没了
        //     委托止损  开仓时在交易所挂一次、永不移动，进程死了它还在
        //   塞进一格没法同时表达，而"我的保命单还在不在"恰恰是必须一眼可见的
        botTable_ = new QTableWidget(0, ColCount);
        botTable_->setHorizontalHeaderLabels(
            {"#","品种","方向","周期·模式","委托止损",
             "开仓价","标记价","24h涨跌","延迟","浮动P&L","保证金","收益率","移动止损",
             "已实现","状态","操作"});
        botTable_->horizontalHeaderItem(ColHardStop)->setToolTip(
            "委托止损 —— 交易所侧的 STOP_MARKET + closePosition 单（\"保命单\"）\n"
            "开仓成交后挂一次，此后【永不移动】，只管最大风险兜底。\n"
            "进程崩溃、断电、断网之后它依然有效，是唯一不依赖本程序存活的保护。\n\n"
            "  $价格     已挂上，显示触发价\n"
            "  重试 N/13 挂单失败中，正在按退避重试\n"
            "  ⚠ 无保护  已连续失败 10 次，再失败 3 次会立即平掉该仓位\n"
            "  已熔断    连续 2 次因挂不上而平仓，该 bot 已停止\n"
            "  —        未开启（配置里的 use_disaster_stop）");
        botTable_->horizontalHeaderItem(ColTrailStop)->setToolTip(
            "移动止损 —— 本地棘轮止损线，只朝有利方向移动\n"
            "显示 触发价(距现价%)。距离才是可操作的信息：一眼知道还有多少余地。\n"
            "【越过成本价后转绿】= 这笔已锁定盈利。\n\n"
            "⚠ 它活在本进程里。程序不在了，守着仓位的就只剩上面那张委托止损单");
        auto* hdr = botTable_->horizontalHeader();
        hdr->setSectionResizeMode(QHeaderView::Stretch);
        for (int c : {(int)ColIdx, (int)ColChg24, (int)ColLatency, (int)ColState})
            hdr->setSectionResizeMode(c, QHeaderView::Fixed);
        hdr->resizeSection(ColIdx,      26);
        hdr->resizeSection(ColChg24,    72);
        hdr->resizeSection(ColLatency,  60);
        hdr->resizeSection(ColState,    96);
        hdr->setSectionResizeMode(ColOps, QHeaderView::Fixed);
        hdr->resizeSection(ColOps, 175);

        botTable_->verticalHeader()->setVisible(false);
        botTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        botTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        botTable_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        botTable_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(botTable_, &QTableWidget::customContextMenuRequested,
                this, &MainWindow::onWatchlistContextMenu);
        tv->addWidget(botTable_, 1);

        // ── 两套策略同表 ────────────────────────────────────────────────────
        // v4.5.0 之前这里是两个标签页，理由是"两张表的列语义完全不同，挤在一个
        // 视野里只会让两边都读不清"。改成同表之后那个问题靠三件事化解：
        //   ① 16 列里有 9 列本就同义（品种/标记价/24h/延迟/浮动P&L/保证金/
        //      收益率/已实现/状态），跨策略直接复用
        //   ② 3 列语义相近，复用列位并改列头（见上面的表格构造）
        //   ③ SAR 独有的 ATR 数据灯、胜率、连续反手、决策文字全部收进悬停
        // 换来的是：一个品种只能有一套策略，这件事从 4 处手写检查变成配置弹窗里
        // 单选框的天然性质，再也不可能从某个入口绕过去
        vSplit->addWidget(tw);
    }

    // ── 底部日志区 ─────────────────────────────────────────────────────────────
    {
        auto* logW   = new QWidget();
        auto* logLay = new QVBoxLayout(logW);
        logLay->setContentsMargins(0, 0, 0, 0);
        logLay->setSpacing(2);

        auto* logHdr = new QLabel("运行日志");
        logHdr->setStyleSheet("color:#8b949e;font-size:11px;padding:2px 0;");
        logLay->addWidget(logHdr);

        // QPlainTextEdit 而不是 QTextEdit：前者有原生的 setMaximumBlockCount，
        // 超出自动丢弃最早的行。原先用 QTextEdit + append() 从不裁剪，3 秒一个 tick
        // 连跑几周会把几十万条富文本节点常驻内存——这台机器上的仓位要持有几个月，
        // 这是必然会撞上的增长点。完整日志照常按天落盘，界面只留最近的
        logBox_ = new QPlainTextEdit();
        logBox_->setReadOnly(true);
        logBox_->setMaximumHeight(160);
        logBox_->setMaximumBlockCount(kLogMaxLines);
        logBox_->setStyleSheet("QPlainTextEdit{font-size:11px;font-family:Consolas;}");
        logLay->addWidget(logBox_);

        vSplit->addWidget(logW);
    }

    vSplit->setStretchFactor(0, 1);
    vSplit->setStretchFactor(1, 0);
    vSplit->setSizes({580, 160});
    root->addWidget(vSplit, 1);

    // ── 定时器接线 ─────────────────────────────────────────────────────────────
    tick_timer_ = new QTimer(this);
    tick_timer_->setInterval(3000);
    connect(tick_timer_, &QTimer::timeout, this, &MainWindow::onTick);

    // 100ms 定时器只刷新"最新成交价/延迟"两个文本单元格，不重建整行/按钮，
    // 否则操作列按钮会在点击的瞬间被销毁重建，导致点击事件丢失（点了没反应）
    ob_timer_ = new QTimer(this);
    ob_timer_->setInterval(100);
    connect(ob_timer_, &QTimer::timeout, this, &MainWindow::refreshLiveQuotes);
    ob_timer_->start();

    // 呼吸灯相位 + 顶部连接计时，50ms 刷新一次，够平滑又不费资源
    breathe_clock_.start();
    header_timer_ = new QTimer(this);
    header_timer_->setInterval(50);
    connect(header_timer_, &QTimer::timeout, this, &MainWindow::updateHeader);
    header_timer_->start();
}

// ─────────────────────────────────────────────────────────────────────────────
// 连接 Binance
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onConnect() {
    // 已连接/连接中时禁止重入：重连会替换 engine_/client_，而线程池里在途的
    // 下单任务持有旧引擎的裸指针——旧引擎被析构后任务执行就是悬空访问，
    // 且已发出的真实订单成交结果会写进"孤儿引擎"，本地跟踪与交易所永久脱节
    if (connState_ == ConnState::Connected || connState_ == ConnState::Connecting ||
        connState_ == ConnState::NetworkError) {
        log("已处于连接状态，如需重连请重启程序（防止在途订单状态丢失）", "WARN");
        return;
    }

    QString key    = apiKey_.trimmed();
    QString secret = apiSecret_.trimmed();
    if (key.isEmpty() || secret.isEmpty()) {
        connLabel_->setText("请先在【设置】里填写 API Key 和 Secret");
        connLabel_->setStyleSheet("color:#d29922;font-size:11px;");
        setConnState(ConnState::Failed);
        return;
    }

    TradingClient::Config cfg;
    cfg.api_key    = key.toStdString();
    cfg.api_secret = secret.toStdString();
    cfg.account_mode = (accountMode_ == 1) ? TradingClient::AccountMode::PortfolioMargin
                                           : TradingClient::AccountMode::Futures;
    // 统一账户没有测试网，这里再兜一次底（设置弹窗已经禁用了勾选，但配置文件可能是手改的）
    cfg.testnet    = (accountMode_ == 1) ? false : testnet_;

    // 无论本次连接是否成功都先保存，避免限流/网络失败导致密钥丢失、下次仍需手动输入
    save_credentials();

    connLabel_->setText("连接中...");
    connLabel_->setStyleSheet("color:#d29922;font-size:11px;");
    setConnState(ConnState::Connecting);
    btnConnect_->setEnabled(false);

    run_async([this, cfg]() {
        auto client = std::make_shared<TradingClient>(cfg);
        const auto tsync = client->sync_server_time();
        // 探测账户的持仓模式（单向 / 双向）。此前这个查询【从未被调用】，
        // dual_mode_ 从进程启动到结束一直是默认的 false，等于把"单向持仓"写死了：
        //   · 账户是单向     → 恰好正确，一直没暴露问题
        //   · 账户是双向     → 每笔单都缺 positionSide 参数，交易所一律拒单 -4061
        // 同时 add_bot 那道"单向模式下不许同品种双向 bot"的检查也依赖它，
        // 读到假的 false 会把一个合法配置拦掉
        const bool dual = client->fetch_position_mode();
        auto info = client->fetch_account();

        QMetaObject::invokeMethod(this, [this, client, info, cfg, dual, tsync]() {
            btnConnect_->setEnabled(true);
            if (!info.ok) {
                connLabel_->setText("连接失败: " + QString::fromStdString(info.error));
                connLabel_->setStyleSheet("color:#f85149;font-size:11px;");
                setConnState(ConnState::Failed);
                log("连接失败: " + QString::fromStdString(info.error), "ERR");
                if (!alertedDisconnect_) {
                    alertedDisconnect_ = true;
                    sendAlert(QString("[TradingBot] 账户连接失败: %1")
                              .arg(QString::fromStdString(info.error)));
                }
                return;
            }

            client_ = client;
            // 把探测结果打出来：持仓模式决定下单参数，配错了是【每笔单都被拒】，
            // 而错误码 -4061 光看字面很难联想到是这里
            log(dual ? "账户持仓模式: 双向持仓（下单将带 positionSide）"
                     : "账户持仓模式: 单向持仓", "OK");
            // 首次对时的结果：偏移量是 -1021 的直接成因，连接时就该让人看见
            log(QString::fromStdString(tsync.to_log()), tsync.accepted ? "OK" : "WARN");
            trend_engine_ = std::make_shared<TrendEngine>(client_, pool_);
            trend_engine_->set_max_total_margin(maxTotalMargin_);
            trend_engine_->set_log_cb([this](const std::string& msg) {
                QMetaObject::invokeMethod(this, [this, msg]() {
                    log(QString::fromStdString(msg));
                    refreshBotTable();
                    save_trend_bots();
                }, Qt::QueuedConnection);
            });
            trend_engine_->set_trade_cb([this](const TrendTrade& tr) {
                QMetaObject::invokeMethod(this, [this, tr]() {
                    // 直接存 TrendTrade，不再转成中间结构：v4.7.1 之前这里要把它
                    // 翻译成 DCA 的 TradeRecord（层数填 0、原因加 "SAR " 前缀），
                    // 那层适配随 DCA 一起没了
                    trades_.push_back(tr);
                    save_trades();
                    refreshStats();
                    save_trend_bots();
                    // 交易所侧灾难止损触发时本地【收不到】这个回调——它在周期
                    // 对账里表现为一条"交易所已无此仓位"，告警走那条路
                }, Qt::QueuedConnection);
            });

            account_info_       = info;
            connNetName_        = cfg.testnet ? "测试网"
                                  : (cfg.account_mode == TradingClient::AccountMode::PortfolioMargin
                                     ? "统一账户" : "主网");
            alertedDisconnect_  = false;
            setConnState(ConnState::Connected);   // 顶部计时从此刻开始，文本由 updateHeader() 接管

            // 启动盘口 WebSocket
            ticker_ = std::make_unique<BookTickerStream>(cfg.testnet);
            // 订阅被拒之类的服务端消息此前被静默丢弃，某条流没订上时界面
            // 只是空白、无从查起。回调跑在 WS 线程，转回 GUI 线程再写日志
            ticker_->on_server_msg([this](const std::string& m) {
                QMetaObject::invokeMethod(this, [this, m]() {
                    log(QString::fromStdString(m), "WARN");
                }, Qt::QueuedConnection);
            });
            ticker_->start();

            slowTickCount_  = 0;   // 保证"首tick立即对账/对时"在（罕见的）重连后依然成立
            tick_timer_->start();

            log(QString("连接成功 | %1 | 权益 $%2 | 可用 $%3%4")
                .arg(connNetName_)
                .arg(info.total_equity, 0, 'f', 2)
                .arg(info.available,    0, 'f', 2)
                .arg(info.uni_mmr > 0
                     ? QString(" | uniMMR %1").arg(info.uni_mmr, 0, 'f', 2)
                     : QString()), "OK");

            // 恢复上次保存的 Bot
            load_and_restore_trend();
            funding_.load(funding_path());   // 资金费账本（品种级，与 bot 生命周期无关）

            // 启动对账：本地跟踪的仓位 vs 交易所实际持仓。外部手动平过仓/强平过的话，
            // 本地状态是错的，带着错误均价继续跑会把止盈止损全算错
            run_async([this]() {
                if (!client_ || !trend_engine_) return;
                auto ex_pos = client_->fetch_positions();
                QMetaObject::invokeMethod(this, [this, ex_pos]() {
                    if (!trend_engine_) return;
                    std::vector<TrendEngine::ExchangePos> ex;
                    ex.reserve(ex_pos.size());
                    for (const auto& p : ex_pos)
                        ex.push_back({p.symbol, p.direction, p.qty, p.entry_price});
                    auto issues = trend_engine_->reconcile_positions(ex);
                    if (!issues.empty()) {
                        for (const auto& i : issues)
                            log("对账: " + QString::fromStdString(i), "WARN");
                        save_trend_bots();   // 收敛后的状态立刻落盘
                        refreshBotTable();
                        sendAlert(alert_text("启动对账", issues));
                    }
                }, Qt::QueuedConnection);
            });
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 资金费账本刷新。
// 这是【账本】不是风控：只记录和展示持有成本，不参与任何交易决策。
// 永续合约每 8 小时结算一次资金费，这笔钱是真实划走的现金——价格涨回来也拿不回，
// 所以它和"浮亏"性质完全不同，必须单独看得见。
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshFunding() {
    if (!client_ || !trend_engine_) return;
    if (fundFetchBusy_.exchange(true)) return;   // 上一批没跑完就跳过

    // 收集所有品种，以及补历史的起点。
    // 起点取【持仓 bot 的启动时间】而不是"建仓时刻"：SAR 会反手、会多次进出，
    // 没有单一的建仓时刻可取。往前多取一段是安全的——账本按 (品种, 结算时间)
    // 去重，重复拉到的流水不会记两次，代价只是多翻一页
    std::vector<std::string> syms;
    int64_t earliest = 0;
    std::set<std::string> seen;
    for (const auto& b : trend_engine_->get_bots()) {
        if (seen.insert(b.cfg.symbol).second) syms.push_back(b.cfg.symbol);
        if (b.qty > 0) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          b.start_time.time_since_epoch()).count();
            if (ms > 0 && (earliest == 0 || ms < earliest)) earliest = ms;
        }
    }
    if (syms.empty()) { fundFetchBusy_.store(false); return; }

    const bool do_history = !fundingBackfilled_ || (fundTickCount_ % 1200 == 1);
    const int64_t backfill = fundingBackfilled_ ? 0 : earliest;

    run_async([this, syms, do_history, backfill]() {
        auto now_ms = QDateTime::currentMSecsSinceEpoch();
        // ① 当前费率（公开接口，不占签名限流）
        for (const auto& s : syms) {
            auto pi = client_->fetch_premium(s);
            if (pi.ok) funding_.set_rate(s, pi.funding_rate, pi.next_ms, now_ms);
        }
        // ② 历史流水（签名接口，权重较高，所以低频）
        int added = 0;
        if (do_history)
            added = sync_funding_ledger(*client_, funding_, syms, backfill);

        QMetaObject::invokeMethod(this, [this, added, do_history]() {
            if (do_history) {
                if (!fundingBackfilled_) {
                    fundingBackfilled_ = true;
                    double sum = 0;
                    for (const auto& s : funding_.symbols()) sum += funding_.get(s).total;
                    if (added > 0)
                        log(QString("资金费账本已同步 %1 条流水，累计 %2 USDT")
                            .arg(added).arg(sum, 0, 'f', 2),
                            sum < 0 ? "WARN" : "OK");
                }
                funding_.save(funding_path());
            }
            refreshBotTable();
            fundFetchBusy_.store(false);
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 权益/可用：随 onTick 异步刷新，不再只在连接那一刻查询一次
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshAccount() {
    if (!client_ || accFetchBusy_.load()) return;
    accFetchBusy_.store(true);
    run_async([this]() {
        auto info = client_->fetch_account();
        accFetchBusy_.store(false);
        QMetaObject::invokeMethod(this, [this, info]() {
            if (info.ok) {
                account_info_ = info;
                if (netFailCount_ > 0) netFailCount_ = 0;
                if (connState_ == ConnState::NetworkError) {
                    // 网络恢复：不重置 connect_elapsed_（那是"这次登录会话"的计时，
                    // 不是"这次网络在线"的计时），只是把状态和提醒去重标记切回来
                    connState_ = ConnState::Connected;
                    alertedDisconnect_ = false;
                    log("网络已恢复正常", "OK");
                }
                return;
            }
            // -1021 = 本机时钟漂移超窗，不是网络问题——立即重新对时自愈，不等每小时定时
            if (info.error.find("-1021") != std::string::npos) {
                run_async([this]() {
                    if (!client_) return;
                    const auto ts = client_->sync_server_time();
                    // 只有异常才吭声。这条路径每次 -1021 都会走到，v3.9.6 里它一天
                    // 打了近百条，把交易信息全淹了——而正常对时本来就是常态
                    if (!ts.noteworthy()) return;
                    QMetaObject::invokeMethod(this, [this, ts]() {
                        log(QString::fromStdString(ts.to_log()), "WARN");
                    }, Qt::QueuedConnection);
                });
            }
            // 单次失败不覆盖上一次的有效账户数据，但要计数——连续失败才判定为网络异常，
            // 避免偶尔一次超时就报警
            ++netFailCount_;
            if (netFailCount_ >= 3 && connState_ == ConnState::Connected) {
                connState_ = ConnState::NetworkError;
                log(QString("检测到网络异常（账户接口连续 %1 次拉取失败）: %2")
                    .arg(netFailCount_).arg(QString::fromStdString(info.error)), "ERR");
                if (!alertedDisconnect_) {
                    alertedDisconnect_ = true;
                    sendAlert(QString("[TradingBot] 检测到网络异常，账户接口连续拉取失败: %1")
                              .arg(QString::fromStdString(info.error)));
                }
            }
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 顶部连接状态：呼吸灯 + 已连接计时，50ms 刷新一次
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::setConnState(ConnState s) {
    connState_ = s;
    if (s == ConnState::Connected) connect_elapsed_.start();
}

void MainWindow::updateHeader() {
    if (!breatheDot_ || !connLabel_) return;

    QColor base;
    bool   breathing = true;
    switch (connState_) {
        case ConnState::Disconnected: base = QColor("#484f58"); breathing = false; break;
        case ConnState::Connecting:   base = QColor("#d29922"); breathing = true;  break;
        case ConnState::Connected:    base = QColor("#3fb950"); breathing = true;  break;
        case ConnState::Failed:       base = QColor("#f85149"); breathing = false; break;
        case ConnState::NetworkError: base = QColor("#f85149"); breathing = true;  break;
    }

    double alpha = 1.0;
    if (breathing) {
        double t = breathe_clock_.elapsed() / 1000.0;
        constexpr double kPi = 3.14159265358979323846;
        alpha = 0.35 + 0.65 * (std::sin(t * 2.0 * kPi / 2.0) + 1.0) / 2.0;   // 2s 一次呼吸
    }
    breatheDot_->setStyleSheet(QString("background:rgba(%1,%2,%3,%4);border-radius:5px;")
        .arg(base.red()).arg(base.green()).arg(base.blue()).arg(alpha));

    if (connState_ == ConnState::Connected || connState_ == ConnState::NetworkError) {
        qint64 totalSec = connect_elapsed_.elapsed() / 1000;
        int hh = int(totalSec / 3600), mm = int((totalSec % 3600) / 60), ss = int(totalSec % 60);
        QString timeStr = QString("%1 | %2:%3:%4")
            .arg(connNetName_)
            .arg(hh, 2, 10, QChar('0')).arg(mm, 2, 10, QChar('0')).arg(ss, 2, 10, QChar('0'));
        // 行情链路徽标。这和左边那个"已连接"是【两件不同的事】：那个指账户与
        // 下单通道，这个指行情 WS。此前界面上完全没有地方能看出行情链路的状态，
        // 而 WS 半开时账户通道照常绿着、REST 兜底照常有价，整个界面没有任何
        // 一处会变色——这正是"降级但不可见"最后一块拼图
        if (ticker_ && (feed_hc_++ % 20) == 0) {     // 50ms × 20 = 1 秒重算一次
            const auto fh = ticker_->health();
            feedBadge_ = fh.healthy() ? QStringLiteral("行情✓") : QStringLiteral("⚠行情异常");
            feedTip_   = QString::fromStdString(fh.summary());
        }
        const QString badge = feedBadge_.isEmpty() ? QString() : " | " + feedBadge_;
        const bool feed_bad = feedBadge_.startsWith(QChar(0x26A0));   // ⚠

        if (connState_ == ConnState::NetworkError) {
            connLabel_->setText("⚠ 网络异常(重试中) | " + timeStr + badge);
            connLabel_->setStyleSheet("color:#f85149;font-size:11px;");
        } else {
            connLabel_->setText("已连接 | " + timeStr + badge);
            // 账户通道好、行情链路坏时也要变色。只看账户通道的话，
            // 行情死了这里依旧是一片绿——那就等于没有指示
            connLabel_->setStyleSheet(feed_bad ? "color:#d29922;font-size:11px;"
                                              : "color:#3fb950;font-size:11px;");
        }
        connLabel_->setToolTip(feedTip_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 实盘监控：添加品种 / 右键菜单
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onAddWatchSymbol() {
    if (!addSymbolEdit_) return;
    QString raw = addSymbolEdit_->text().trimmed().toUpper();
    addSymbolEdit_->clear();
    if (raw.isEmpty()) return;

    if (!trend_engine_) { log("请先点击【连接】", "WARN"); return; }

    // 只需要输入代币符号（不区分大小写），默认按 USDT 永续合约补全后缀
    if (raw.endsWith("USDT")) raw.chop(4);
    if (raw.isEmpty()) return;
    std::string symbol = (raw + "USDT").toStdString();

    for (const auto& b : trend_engine_->get_bots())
        if (b.cfg.symbol == symbol) {
            log(QString::fromStdString(symbol) + " 已经在列表里了", "WARN");
            return;
        }

    TrendConfig cfg;              // 全部用默认参数，具体配置留给右键弹窗
    cfg.symbol = symbol;
    auto id = trend_engine_->add_bot(cfg);
    if (id.empty()) return;
    trend_engine_->stop_bot(id);  // 初始状态为停止，需要手动配置 + 开启监控
    if (ticker_) ticker_->subscribe(symbol);

    log(QString::fromStdString(symbol) + " 已添加（已停止），右键进行策略配置", "OK");
    refreshBotTable();
    save_trend_bots();
}

void MainWindow::onWatchlistContextMenu(const QPoint& pos) {
    if (!botTable_) return;
    auto* item = botTable_->itemAt(pos);
    if (!item) return;
    auto* symItem = botTable_->item(item->row(), ColSym);
    if (!symItem) return;
    std::string sym = symItem->text().toStdString();

    // 右键菜单只留"配置策略"——原先紧挨着的"从列表中删除"太容易误点，删掉的是
    // 整个 bot（含已配好的策略和仓位跟踪），代价太高。要删除品种：先【停止】该
    // bot，再点顶部的【清除已停止】按钮，两步操作天然防误触
    QMenu menu(this);
    QAction* actConfig = menu.addAction("配置策略...");
    QAction* chosen = menu.exec(botTable_->viewport()->mapToGlobal(pos));
    if (chosen == actConfig) openStrategyDialog(sym);
}

// ─────────────────────────────────────────────────────────────────────────────
// 策略配置弹窗：新建或编辑一个品种的 bot，保存后回到监控页面
//
// v4.5.0~v4.7.1 这里是"策略二选一"的双页弹窗（顶部单选 + QStackedWidget，
// 一页网格DCA 一页趋势SAR，另有带仓锁定与切换确认共约 800 行）。网格DCA 移除后
// 只剩一套策略，整个选择/切换机制连同 DCA 那一页一并删除——表单本体一直住在
// trend_panel.cpp（buildTrendForm / collectTrendForm / applyTrendConfig），这里只负责
// 把它装进一个带滚动区的对话框
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::openStrategyDialog(const std::string& symbol) {
    auto sar_bots = trend_engine_ ? trend_engine_->get_bots() : std::vector<TrendBot>{};
    const TrendBot* trendBot = nullptr;
    for (const auto& b : sar_bots)
        if (b.cfg.symbol == symbol) { trendBot = &b; break; }

    QDialog dlg(this);
    dlg.setWindowTitle(QString("趋势策略配置 - %1").arg(QString::fromStdString(symbol)));
    dlg.resize(860, 760);

    // 外层：滚动区 + 固定在底部的按钮。分组之后内容比一屏高，小屏笔记本上
    // 固定高度会把"保存"顶出屏幕外
    auto* outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* canvas = new QWidget();
    auto* dv = new QVBoxLayout(canvas);
    dv->setContentsMargins(14, 12, 14, 12);
    dv->setSpacing(14);
    scroll->setWidget(canvas);
    outer->addWidget(scroll, 1);

    TrendConfig trendCfg;
    trendCfg.symbol = symbol;
    if (trendBot) trendCfg = trendBot->cfg;
    auto trendForm = buildTrendForm(dv, trendCfg);
    dv->addStretch(1);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    // 按钮固定在滚动区外：内容一长，放在里面会被滚出可视范围
    { auto* bw = new QWidget(); auto* bl = new QHBoxLayout(bw);
      bl->setContentsMargins(14, 6, 14, 12); bl->addWidget(btnBox);
      outer->addWidget(bw); }
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    if (!trend_engine_) {
        log("请先点击【连接】", "WARN");
        return;
    }

    TrendConfig sc;
    if (!collectTrendForm(trendForm, sc)) return;   // 表单级校验没过，状态没动
    sc.symbol = symbol;
    applyTrendConfig(sc, trendBot);                 // 校验 + 建/改 bot + 订阅 + 落盘
}

// ─────────────────────────────────────────────────────────────────────────────
// 危险操作的二次确认。
// 默认按钮刻意设成【取消】：这些按钮和常用按钮挨着，误点之后再顺手敲一下
// 回车/空格就等于确认了，那和没有确认框一样
bool MainWindow::confirmDanger(const QString& title, const QString& body,
                               const QString& okText) {
    QMessageBox box(this);
    box.setWindowTitle(title);
    box.setText(body);
    box.setIcon(QMessageBox::Warning);
    auto* okBtn     = box.addButton(okText, QMessageBox::AcceptRole);
    auto* cancelBtn = box.addButton("取消",  QMessageBox::RejectRole);
    box.setDefaultButton(cancelBtn);
    box.setEscapeButton(cancelBtn);
    okBtn->setStyleSheet("QPushButton{background:#3d1a1a;color:#f85149;padding:4px 14px;}");
    box.exec();
    return box.clickedButton() == okBtn;
}

void MainWindow::onStopAll() {
    if (!trend_engine_) return;
    // ⚠ 下面会关掉 tick 定时器，而引擎【全靠它驱动】。漏停任何一个 bot 的后果是
    //   它在界面上仍显示"运行中"、实际却再也收不到价格——追踪止损线永远不会被
    //   触发，仓位静默裸奔。这正是单行【继续】按钮里那道"定时器没开就拉起来"
    //   守卫在防的同一件事
    int running = 0, withPos = 0;
    for (const auto& b : trend_engine_->get_bots()) {
        if (b.state != TrendBot::State::Stopped) ++running;
        if (b.st.pos != trend::Pos::Flat && b.qty > 0) ++withPos;
    }
    if (!confirmDanger("确认全部停止",
            QString("将停止 %1 个运行中的 Bot，并关闭 Tick 定时器。\n\n"
                    "持仓【不会】被平掉，但追踪止损、反手、金字塔加仓全部暂停——"
                    "当前有 %2 个品种持仓，停止期间它们不再受任何本地策略管理"
                    "（只剩交易所侧的灾难止损单还有效）。\n\n"
                    "确定要停止吗？")
                .arg(running).arg(withPos),
            "全部停止")) return;

    trend_engine_->stop_all();
    tick_timer_->stop();
    log("所有Bot已停止，Tick定时器已关闭", "WARN");
    refreshBotTable();
    save_trend_bots();
}

void MainWindow::onClearStopped() {
    if (!trend_engine_) return;
    int n = 0, withPos = 0;
    for (const auto& b : trend_engine_->get_bots()) {
        if (b.state != TrendBot::State::Stopped) continue;
        ++n;
        if (b.qty > 0) ++withPos;
    }
    if (n == 0) { log("没有已停止的 Bot 可清除"); return; }

    QString warn = withPos > 0
        ? QString("\n\n⚠ 其中 %1 个仍有持仓！删除后本地不再跟踪这些仓位，"
                  "它们会变成交易所上无人管理的孤儿仓位（不会被平掉）。").arg(withPos)
        : QString();
    if (!confirmDanger("确认清除已停止的 Bot",
            QString("将删除 %1 个已停止的 Bot 配置。%2\n\n确定要清除吗？").arg(n).arg(warn),
            "清除")) return;

    int done = 0;
    std::set<std::string> touched;
    for (const auto& b : trend_engine_->get_bots())
        if (b.state == TrendBot::State::Stopped) {
            touched.insert(b.cfg.symbol);
            trend_engine_->remove_bot(b.bot_id);
            ++done;
        }
    // 全部删完之后再统一退订：边删边退会误判"还有别的 bot 在用"
    for (const auto& s : touched) unsubscribeIfUnused(s);
    if (done > 0) log(QString("已清除 %1 个已停止Bot").arg(done), withPos > 0 ? "WARN" : "INFO");
    refreshBotTable();
    save_trend_bots();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick（3s 引擎驱动）
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onTick() {
    if (!trend_engine_ || !client_) return;

    refreshPositions();
    refreshAccount();

    // ── SAR 信号拉取（ATR + 唐奇安通道），每 20 个 tick 约 60 秒 ───────────────
    // 止损线的推进【不靠这个】：它在每个 tick 用实时价推进，只有 ATR 的数值
    // 来自这里。所以这一批慢一点不影响保护，只影响新仓位的入场判定
    if (trend_engine_ && !trendSigBusy_.load()) {
        // 每个 bot 按自己周期该有的节奏拉：4h 的每 60 秒一次就够，3m 的要 45 秒
        // 一次才不会错过收盘。tick 是 3 秒一拍，所以除以 3 换算成拍数
        const bool forced = trendSigForce_.exchange(false);
        std::vector<TrendBot> need;
        for (const auto& b : trend_engine_->get_bots()) {
            if (b.state != TrendBot::State::Running) continue;
            const int ticks = std::max(1,
                TrendEngine::signal_period_sec(b.cfg.interval) / 3);
            if (forced || (slowTickCount_ % ticks) == 1) need.push_back(b);
        }
        if (!need.empty()) {
            trendSigBusy_.store(true);
            run_async([this, need]() {
                // 失败的品种要报出来。原先这里是 `if (!snap.ok) continue;`——
                // 完全静默，于是"信号拉不到"和"拉到了但没突破"在界面上长得
                // 一模一样，都是"等信号"。用户唯一能做的就是干等
                QStringList failed;
                int okn = 0;
                for (const auto& b : need) {
                    if (!client_ || !trend_engine_) break;
                    if (b.cfg.rule.strategy != trend::Strategy::Turtle) {
                        auto bp = client_->fetch_bar_pattern(
                            b.cfg.symbol, b.cfg.interval, b.cfg.rule.swing_bars);
                        if (!bp.ok) { failed << QString::fromStdString(b.cfg.symbol); continue; }
                        ++okn;
                        // 裸K 与 抛物线SAR 只要原始高低点，【不需要 ATR】——
                        // 少拉一次 REST。update_bars 自己设 sig_ok，所以不必
                        // 像 v5.1.0 之前那样成对调用两个接口（漏一次的表现是
                        // "配置正常、日志正常、一单不开"）
                        TrendEngine::BarSnap s;
                        s.open        = bp.cur_open;
                        s.prev_close  = bp.prev_close;
                        s.prev_high   = bp.prev_high;
                        s.prev_low    = bp.prev_low;
                        s.prev2_high  = bp.prev2_high;
                        s.prev2_low   = bp.prev2_low;
                        s.swing_low   = bp.swing_low;
                        s.swing_high  = bp.swing_high;
                        s.bar_open_ms = bp.bar_open_ms;
                        trend_engine_->update_bars(b.bot_id, s);
                        continue;
                    }
                    auto snap = client_->fetch_trend_signal(
                        b.cfg.symbol, b.cfg.interval,
                        b.cfg.rule.donchian_period, b.cfg.rule.atr_period);
                    if (!snap.ok) {
                        failed << QString::fromStdString(b.cfg.symbol);
                        continue;
                    }
                    ++okn;
                    trend_engine_->update_signal(b.bot_id, snap.atr, snap.atr_pct,
                                               snap.dc_ok, snap.dc_up, snap.dc_dn,
                                               snap.bar_open_ms);
                }
                trendSigBusy_.store(false);
                QMetaObject::invokeMethod(this, [this, failed, okn]() {
                    // 只在【集合变化】时报。拉取每分钟一轮，而一个拼错的品种会
                    // 永远失败——每轮都报就是每小时 60 条同样的告警，把真正有用
                    // 的日志全冲走（本项目踩过这个：150 行日志里 120 行是噪音）
                    std::set<std::string> now;
                    for (const auto& f : failed) now.insert(f.toStdString());

                    QStringList fresh;
                    for (const auto& s : now)
                        if (!trendSigFailed_.count(s))
                            fresh << QString::fromStdString(s);
                    QStringList healed;
                    for (const auto& s : trendSigFailed_)
                        if (!now.count(s))
                            healed << QString::fromStdString(s);
                    trendSigFailed_ = std::move(now);

                    if (!fresh.isEmpty())
                        log(QString("⚠ SAR 信号拉取失败：%1（本轮成功 %2 个）。"
                                    "拉不到 K 线 = 没有 ATR = 没有止损线，"
                                    "这些品种不会开新仓。"
                                    "最常见的原因是品种名不对——币安合约的代码形如 "
                                    "BTCUSDT，不是 BTC")
                                .arg(fresh.join(", ")).arg(okn), "WARN");
                    if (!healed.isEmpty())
                        log(QString("SAR 信号已恢复：%1").arg(healed.join(", ")), "OK");
                    refreshBotTable();
                }, Qt::QueuedConnection);
            });
        }
    }

    // 布林带/RSI 指标批次随网格DCA 一并移除：那批数据只服务 DCA 的首单信号。
    // 趋势策略要的 ATR 与唐奇安通道走上面那条独立的信号拉取，节奏也不同
    // （按各 bot 自己的 K 线周期，而不是固定 5 分钟）

    ++slowTickCount_;

    // 资金费：费率每 100 tick（约5分钟）刷一次，历史流水每 1200 tick（约1小时）同步一次。
    // 结算本身 8 小时才一次，再密没有意义，纯属浪费限流额度
    if (fundTickCount_++ % 100 == 0) refreshFunding();

    // 成交明细的延迟落盘：save_trades 做了去抖，被压下的写在这里补上
    if (tradesDirty_) save_trades(true);

    // 每 15 分钟重新对时一次（tick=3s，300×3s=900s）：时钟漂移超过 recvWindow(5s)
    // 会让所有签名请求集体失败。
    //
    // 原为每小时。改短的实测依据：实盘那台被抓到本机时钟【瞬间步进】——
    // 采样间隔固定 30 秒，服务器时间每步稳定 +30096ms，而本机某一步只走了
    // +29024ms（少 1072ms），90 秒后又走了 +31043ms（多 947ms）补回来，
    // 期间往返一直是正常的 80ms 左右，排除测量假象。
    //
    // 步进之后，下一次对时会正确测出新偏移，所以真正的暴露窗口是
    // 【步进发生 → 下次对时】这一段：原先最坏一小时都带着 1 秒误差在发单，
    // 而超前侧预算只有 2000ms，一口气吃掉一半。缩到 15 分钟把这个窗口砍掉 4 倍。
    //
    // 成本可以忽略：/fapi/v1/time 权重 1，每小时 4 次 vs 限额 2400/分钟；
    // 且公开行情改走连接池后单次对时从冷连接 747ms 降到热连接 84ms
    if (slowTickCount_ % 300 == 0) {
        run_async([this]() {
            if (!client_) return;
            const auto ts = client_->sync_server_time();
            // 常规对时静默；只有测量被丢弃、或偏移大幅跳变（本机时钟被系统步进）
            // 才值得占用一行——那两种情况都意味着有东西不对劲
            if (!ts.noteworthy()) return;
            QMetaObject::invokeMethod(this, [this, ts]() {
                log(QString::fromStdString(ts.to_log()), "WARN");
            }, Qt::QueuedConnection);
        });
    }

    // 周期对账：每 20 个 tick（约1分钟）拿最新持仓和本地跟踪核对一次。
    //
    // 起因是一个实际反馈：在手机上手动平掉仓位后，程序界面仍然显示着持仓，
    // 要关掉重开才会发现——因为对账此前【只在连接成功时跑一次】。
    // 期间 bot 拿着一个不存在的仓位继续算止盈止损，还会继续补仓。
    //
    // 不额外请求：refreshPositions() 每个 tick 都在拉持仓填 pos_cache_，
    // 此前那份数据只喂给了界面显示，这里直接复用，权重成本为零。
    //
    // 用 Periodic 模式——它会跳过在途和刚成交的 bot，否则正在止盈的那笔
    // 会被当成"外部平仓"清掉（详见 reconcile_positions 的说明）
    // ⚠ 判据用【快照新鲜度】，不能用 pos_cache_ 非空：所有仓位都被外部平掉时
    // 缓存本来就是空的，而那恰恰是最需要对账的时刻。反过来，拉取失败时缓存
    // 同样是空的（或陈旧的），此时若当成"交易所无持仓"就会凭空清掉真实仓位。
    // refreshPositions 只在【真正成功】时才更新 posCacheMs_，所以这里
    // 只要求它足够新；拿不到新数据就这一轮不对账，宁可晚一分钟发现
    const qint64 posAge = QDateTime::currentMSecsSinceEpoch() - posCacheMs_;
    if (slowTickCount_ % 20 == 0 && trend_engine_ && posCacheMs_ > 0 && posAge < 30000) {
        std::vector<TrendEngine::ExchangePos> sex;
        sex.reserve(pos_cache_.size());
        for (const auto& [k, p] : pos_cache_)
            sex.push_back({p.symbol, p.direction, p.qty, p.entry_price});
        auto issues = trend_engine_->reconcile_positions(sex);
        if (!issues.empty()) {
            for (const auto& i : issues)
                log("对账: " + QString::fromStdString(i), "WARN");
            save_trend_bots();          // 收敛后的状态立刻落盘
            refreshBotTable();
            // 明细要进告警正文，不能只报个数：交易所侧灾难止损触发时本地
            // 【收不到成交回调】，它在这里表现为一条"交易所已无此仓位"的
            // 对账不一致。只发个数字等于止损被打掉了也只收到一句
            // "发现 1 处不一致，详见日志"
            sendAlert(alert_text("运行中对账", issues));
        }
    }

    // 24h 滚动涨幅：来自 @ticker 推送流，读一次缓存就行，不发任何请求——
    // 所以每个 tick 都喂，不必搭 5 分钟那班车。@ticker 每秒推一次，
    // 让引擎拿到的始终是最新值
    if (ticker_) {
        // v4.6.0 起 24h 涨幅【只用于显示】那一列——涨幅拦截闸门已移除。
        // 成本几乎为零：@ticker 本就已订阅，REST 兜底是全市场一次取回
        // （权重 40，90 秒一次），与品种数无关
        //
        // ⚠ 这里【不能筛掉 Stopped】。它服务的是"表格那一列要有数"，而表格显示
        //   所有行，不只运行中的。原先筛了 Stopped，后果是：刚添加的品种默认就是
        //   停止的，若此刻 WS 还没首包（或断流），这个循环一个都不进、兜底永不触发，
        //   24h 涨跌 一直空着而且看不出为什么——而"加了品种还没开启"恰恰是最常见的状态。
        //   喂价给引擎那条路才该筛状态（见下面的 syms），两件事必须分开
        bool need_rest_chg = false;
        for (const auto& b : trend_engine_->get_bots()) {
            double pct = 0;
            bool stale = false;
            chg24Of(b.cfg.symbol, pct, stale);
            if (stale) need_rest_chg = true;
        }
        // 一次 REST 拿回全市场（权重 40），不是逐品种——47 个品种逐个查是
        // 47 次往返，而全取只要 1 次，权重也更省
        if (need_rest_chg && client_ && !chg24FetchBusy_.exchange(true)) {
            run_async([this]() {
                auto m = client_->fetch_all_24h_changes();
                if (!m.empty()) {
                    std::lock_guard<std::mutex> lk(chg24Mtx_);
                    chg24Rest_   = std::move(m);
                    chg24RestMs_ = BookTickerStream::now_ms();
                }
                chg24FetchBusy_.store(false);
            });
        }
    }

    // ── 两个品种集合，刻意分开 ────────────────────────────────────────────────
    //   engine_syms —— 要【喂价给引擎】的：必须筛掉 Stopped，停止的 bot 不该产生
    //                  任何决策
    //   disp_syms   —— 表格那几列要【显示】的：所有行都要，不看状态
    // 原先只有一个集合，两件事绑在一起，后果是：全部 bot 都停止时 disp 也空了，
    // markPrice 的 REST 兜底整条路径不执行 → 标记价/延迟一直是 "--"。
    // 而"添加了品种还没开启"正好就是全停状态
    std::set<std::string> engine_syms, disp_syms;
    for (const auto& b : trend_engine_->get_bots()) {
        disp_syms.insert(b.cfg.symbol);
        if (b.state != TrendBot::State::Stopped) engine_syms.insert(b.cfg.symbol);
    }
    if (disp_syms.empty()) { refreshBotTable(); return; }

    std::set<std::string> need_rest;
    for (const auto& sym : disp_syms) {
        // ⚠ 要不要走 REST，判据是【WS 有没有在喂这个品种】，不是"缓存里有没有价"。
        //
        //   原先只在 mark_price() 返回 0（即缓存已超过 kStaleMs=10 秒）时才补拉，
        //   而 REST 补回来的价会写进同一个缓存并刷新 mark_ms。于是 WS 断流时的
        //   节奏变成：拉一次 → 10 秒内都"有价"不再拉 → 过期 → 再拉。
        //   兜底价的粒度因此是【10 秒】，而 tick 是 3 秒一拍——中间那几拍
        //   引擎反复拿到同一个冻结价。markPrice@1s 正常时是 1 秒一包，
        //   所以 WS 一死，价格新鲜度直接掉到十分之一，界面和策略都会明显"变钝"。
        //
        //   ws_mark_ms 是 v5 专门为这件事留的字段：它【只】被 WS 包更新，
        //   REST 写回时刻意不碰。所以用它判断"WS 是否还在喂"是准确的，
        //   不会被自己的兜底写回骗过去。
        const auto tk = ticker_ ? ticker_->get(sym) : ccbot::BookTickerStream::Tick{};
        const bool ws_feeding =
            tk.ws_mark_ms > 0 &&
            (QDateTime::currentMSecsSinceEpoch() - tk.ws_mark_ms) <= ccbot::BookTickerStream::kStaleMs;

        // ⚠ 一拍只能喂引擎【一次】，而且喂的必须是这一拍最新的那个价。
        //
        //   WS 在喂 ⇒ 用缓存里的 WS 价，这一拍就到此为止。
        //   WS 没在喂 ⇒ 这里【什么都不做】，交给下面的 REST 那一批去喂。
        //
        //   不这么分的话会有一个很别扭的状态：WS 刚死、缓存里还躺着上一轮
        //   REST 写回的价（未超 10 秒），于是这一拍先拿那个旧价喂一次引擎，
        //   紧接着 REST 回来又用新价喂第二次 —— 引擎在同一拍里看到两个价。
        //   多数时候无害（棘轮是单调的，旧价上一轮已经喂过），但对
        //   ③ 裸K·立即顺势 不是：它的入场判据是"实时价 vs 本根开盘价"，
        //   一个我们【已经知道不是当前值】的价可能触发一笔新开仓
        if (ws_feeding) {
            // 只有运行中的才驱动引擎；停止的品种拿到价格仅供界面显示
            if (engine_syms.count(sym) && tk.mark_price > 0)
                trend_engine_->tick(sym, tk.mark_price);
        } else {
            need_rest.insert(sym);
        }
    }

    if (need_rest.empty()) { refreshBotTable(); return; }

    // 防重入：这一批是【串行】遍历所有缺价的品种，47 个品种要跑几十秒，
    // 而引擎 tick 是 3 秒一次。没有这道闸的话每 3 秒就再投递一批，
    // 任务在只有 4 个线程的 fetchPool_ 里无限堆积——而高周期指标拉取用的是
    // 同一个池，会被直接饿死，表现为"%B 永远缺失、一单开不出来"。
    // WS 正常时 need_rest 基本为空，这条路径根本走不到；一旦 WS 断了，
    // 品种数越多雪崩得越快，恰恰是最需要它撑住的时候
    if (restFetchBusy_.exchange(true)) { refreshBotTable(); return; }

    run_async([this, need_rest = std::move(need_rest), engine_syms]() {
        // 一次 REST 拿回全市场（权重 10），不是逐品种 N 次往返。
        // 逐品种时一轮的耗时随品种数线性增长——10 个品种、每次往返 200ms
        // 就是 2 秒，47 个品种是十几秒，而这一整轮里最后那个品种拿到的价
        // 已经比第一个旧了十几秒。全取则所有品种共享同一个时间戳。
        // 24h 涨跌那条兜底本来就是这么做的，标记价这条一直是逐品种，属于遗漏
        auto all = client_->fetch_all_mark_prices();
        for (const auto& sym : need_rest) {
            auto it = all.find(sym);
            // 全取失败（空表）或该品种不在返回里 ⇒ 退回单品种查询。
            // 不退回的话，一次网络抖动会让所有品种这一轮都没价
            const double price = (it != all.end()) ? it->second
                                                   : client_->fetch_mark_price(sym);
            if (price > 0) {
                // 同上：只有运行中的才驱动引擎。停止的品种走到这里是为了让
                // 标记价那一列有数
                if (engine_syms.count(sym) && trend_engine_) trend_engine_->tick(sym, price);
                // 写回缓存：界面那一列读的是缓存，不写回就会出现
                // "引擎有价在跑、标记价列却一直空着"
                if (ticker_) ticker_->set_mark_price(sym, price);
            }
        }
        restFetchBusy_.store(false);
        QMetaObject::invokeMethod(this, [this]() {
            refreshBotTable();
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 价格的小数位数。取【tick_size 推的位数】与【价格数量级需要的位数】的较大者。
//
// 为什么不能只信 tick_size：它是精确答案，但有两种拿不到正确值的情况，而且
// 第二种是【静默】的——
//   ① 异步预取还没回来（见 ensureSymbolInfoAsync），传进来是 0
//   ② SymbolInfo::tick_size 的默认值是 0.01，而 info.valid 只取决于 LOT_SIZE
//      （trading_client.cpp: info.valid = lot_found）。PRICE_FILTER 没解析到时
//      valid 照样为真，tick_size 就静默停在 0.01 —— 推出 dp=2
// 两种情况下 0.001 都会显示成 "0.00"，看着像行情挂了。v4.0.7 只修了 ①。
//
// 取 max 之后无论 tick_size 是缺失、正确还是错误，显示都不会丢掉有效数字：
//   ETH  3000  tick 0.01     → max(2, 2) = 2  "3000.00"    尊重 tick
//   某币 0.001 tick 0.0000001→ max(7, 6) = 7  "0.0010000"  尊重 tick
//   某币 0.001 tick 0.01(错) → max(2, 5) = 5  "0.00100"    数量级兜住
// ─────────────────────────────────────────────────────────────────────────────
static int price_decimals(double p, double tick) {
    int dp_tick = 2;
    if (tick > 0 && tick < 1.0) {
        double t = tick; dp_tick = 0;
        while (t < 1.0 - 1e-9 && dp_tick < 8) { t *= 10; ++dp_tick; }
    } else if (tick >= 1.0) {
        dp_tick = 0;
    }
    // 数量级下限：保证至少留住约 4 位有效数字
    const int dp_mag = p >= 100 ? 2 : p >= 1 ? 4 : p >= 0.01 ? 5
                     : p >= 0.0001 ? 6 : p >= 0.000001 ? 8 : 10;
    return std::max(dp_tick, dp_mag);
}

static QString fmt_tick_px(double p, double tick) {
    if (p <= 0) return "--";
    return QString::number(p, 'f', price_decimals(p, tick));
}

// ─────────────────────────────────────────────────────────────────────────────
// 「标记价」单元格。
//
// v4.0.9 起标记价是【唯一】的价格口径：显示是它，引擎决策也是它。
// 此前显示中间价、决策也用中间价，而强平价那一列是币安按标记价给的——
// 两套体系并排放着，剧烈波动时会分叉，恰恰是最需要看准的时候。
//
// 强平、未实现盈亏、强平触发，币安全部按标记价算。既然"离强平多远"是这个策略
// 最关心的问题（套住长持、靠保证金预规划扛），那就让全系统只认这一个价。
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// 「24h涨跌」单元格。红绿口径与旁边的浮动P&L/收益率一致。
//
// 数据有两个来源，界面不关心用的哪个：@ticker 推送流优先，拿不到走全市场
// REST 快照（一次调用取回所有品种，权重 40）。两者都没有才显示 "--"。
// ─────────────────────────────────────────────────────────────────────────────
static QTableWidgetItem* make_chg24_cell(bool has, double pct) {
    auto* it = new QTableWidgetItem(
        has ? QString("%1%2%").arg(pct >= 0 ? "+" : "").arg(pct, 0, 'f', 2) : "--");
    it->setTextAlignment(Qt::AlignCenter);
    it->setForeground(!has ? QColor("#484f58")
                    : pct >= 0 ? QColor("#3fb950") : QColor("#f85149"));
    return it;
}

static QTableWidgetItem* make_mark_cell(const BookTickerStream::Tick& tick, double tick_size) {
    const bool has_mark = tick.mark_price > 0;
    // 陈旧判定必须和引擎用同一把尺子（BookTickerStream::kStaleMs）：超过阈值时
    // mark_price() 对引擎返回 0，界面却还在照常显示那个数字——一个【冻结的价格
    // 长得和实时价一模一样】是最危险的显示方式。
    // 停止的 bot 尤其容易撞上：它被排除在喂价循环之外，REST 兜底也不会跑，
    // 于是这一格就永远停在平仓那一刻的价位上
    const int64_t age = has_mark ? (BookTickerStream::now_ms() - tick.mark_ms) : -1;
    const bool stale  = has_mark && age > BookTickerStream::kStaleMs;

    auto* it = new QTableWidgetItem(has_mark ? fmt_tick_px(tick.mark_price, tick_size) : "--");
    it->setTextAlignment(Qt::AlignCenter);
    it->setForeground(!has_mark ? QColor("#8b949e")
                     : stale    ? QColor("#6e7681")     // 灰掉：这不是实时价
                                : QColor("#e6edf3"));

    QString tip;
    if (!has_mark) {
        tip = "标记价尚未到达（markPrice@1s 每秒一次，刚订阅时会有约 1 秒空窗）。\n"
              "此时引擎也没有价格可用，不会做任何开仓/止盈判定。\n\n";
    } else if (stale) {
        tip = QString("⚠ 这不是实时价：已 %1 秒没有新的标记价，显示的是最后一次收到的值。\n"
                      "引擎侧已判定为陈旧（超过 %2 秒即返回 0），不会拿它做任何决策。\n\n"
                      "常见原因：该 bot 已停止（停止的 bot 不参与喂价循环），\n"
                      "或 markPrice 推送流断了/未订阅成功。\n\n")
                  .arg(age / 1000).arg(BookTickerStream::kStaleMs / 1000);
    }
    if (tick.chg_ms > 0) {
        tip += QString("24h 涨幅 %1%2%\n")
                   .arg(tick.chg_24h >= 0 ? "+" : "").arg(tick.chg_24h, 0, 'f', 2);
    }
    tip += "强平价、浮动盈亏、强平触发都按标记价计算，\n"
           "引擎的开仓/止盈判定同样用它——全系统单一价格口径。";
    it->setToolTip(tip);
    return it;
}

// ─────────────────────────────────────────────────────────────────────────────
// 100ms 高频刷新：只更新"最新成交价"与"延迟"两列的文本，不touch行/按钮
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshLiveQuotes() {
    if (!botTable_ || !trend_engine_ || !ticker_) return;

    // 行序必须与 refreshBotTable 用的【完全一致】，否则这一路 100ms 的快刷会把
    // 价格写到别的品种那一行上。两处都按品种字典序排，口径同源
    auto bots = trend_engine_->get_bots();
    std::sort(bots.begin(), bots.end(),
              [](const TrendBot& a, const TrendBot& b) { return a.cfg.symbol < b.cfg.symbol; });
    // 行数不一致时交给下次完整刷新。⚠ 这道校验一旦判错，表现是不报错、不崩，
    // 只是标记价从 100ms 刷新悄悄退化成 3 秒——最难发现的那一类故障
    if ((int)bots.size() != botTable_->rowCount()) return;

    auto mkc = [](const QString& s, const QColor& c,
                  Qt::Alignment align = Qt::AlignCenter) {
        auto* it = new QTableWidgetItem(s);
        it->setTextAlignment(align);
        it->setForeground(c);
        return it;
    };

    int64_t now_ms = BookTickerStream::now_ms();

    // 这三列（标记价/24h涨跌/延迟）全是【品种级】数据，与策略状态无关
    for (int i = 0; i < (int)bots.size(); ++i) {
        const std::string& sym = bots[(size_t)i].cfg.symbol;
        auto tick = ticker_->get(sym);
        const double tick_size = tickSizeOf(sym);
        botTable_->setItem(i, ColMark, make_mark_cell(tick, tick_size));
        {
            // 必须分两步：把 chg24Of(...) 和 pct 写在同一个实参列表里，
            // 等于在一个表达式内既通过引用【写】pct 又【读】pct，而 C++ 的
            // 函数实参求值顺序是【未指定】的——MSVC 从右往左，先把还是初始值
            // 的 pct(0) 拷进参数，chg24Of 之后才执行，于是永远显示 +0.00%。
            // 引擎侧走的是另一条分支所以数据是对的，只有界面错，极难对上号
            double pct = 0; bool stale = false;
            const bool has = chg24Of(sym, pct, stale);
            botTable_->setItem(i, ColChg24, make_chg24_cell(has, pct));
        }

        // 延迟必须测【引擎实际使用的那条流】。此前测的是 bookTicker，而 v4.0.9
        // 之后引擎决策用的是标记价——markPrice 停了、bookTicker 还在的时候，
        // 这一格显示绿色，引擎却已经在走 REST 兜底。健康指示器指错了对象，
        // 这也是标记价那次故障全程没有任何征兆的原因
        // ⚠ 用 ws_mark_ms 而不是 mark_ms：后者会被 REST 兜底的 set_mark_price
        //   刷新，于是 WS 死掉、引擎已经全靠 REST 在跑的时候，这一格照样显示
        //   绿色的几十毫秒——上面那段注释警告的"指错对象"换了个形式又犯一次。
        //   这一列的全部意义就是"那条流还活着吗"，只有流来的包能回答
        int64_t latency = (tick.ws_mark_ms > 0) ? (now_ms - tick.ws_mark_ms) : -1;
        // 阈值按 markPrice@1s 的节奏定：正常包龄在 0~1000ms 之间均匀分布，
        // 用旧的 100/500ms 会一直显示红色。超过 3 秒说明丢了两三包，
        // 超过 kStaleMs(10s) 引擎就当它断流转 REST 了
        QColor lat_c = (latency < 0)    ? QColor("#484f58")
                     : (latency < 1500) ? QColor("#3fb950")
                     : (latency < 3000) ? QColor("#d29922")
                                        : QColor("#f85149");
        botTable_->setItem(i, ColLatency, mkc(latency >= 0 ? QString("%1ms").arg(latency) : "--", lat_c));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 品种精度信息异步预取——GUI 线程只读缓存，缺失时丢到后台线程取，绝不同步等待
// ─────────────────────────────────────────────────────────────────────────────
// 24h 涨跌：推送流优先，回落到全市场 REST 快照。
// out_stale 回填"缓存该续期了"，由调用方在合适的时机去发那一次全市场请求
// （引擎喂数与界面显示都会调这里，续期只应触发一次）
bool MainWindow::chg24Of(const std::string& symbol, double& out_pct, bool& out_stale) {
    out_stale = false;
    if (!ticker_) return false;
    if (ticker_->change_24h(symbol, out_pct)) return true;

    std::lock_guard<std::mutex> lk(chg24Mtx_);
    auto it = chg24Rest_.find(symbol);
    const int64_t age = BookTickerStream::now_ms() - chg24RestMs_;
    // 先续期、后过期：超过 soft 就该刷新，但旧值继续可用到 hard。
    // 单一阈值会在每次过期时制造一个"没有数据"的空窗——v4.0.14 的实盘日志里
    // 是精确的 90 秒周期、3 秒空窗，闸门会在那 3 秒里假拦截并刷一条噪音
    if (it != chg24Rest_.end() && age < kChg24HardMs) {
        out_pct = it->second;
        if (age > kChg24SoftMs) out_stale = true;
        return true;
    }
    out_stale = true;
    return false;
}

double MainWindow::tickSizeOf(const std::string& symbol) {
    if (!client_) return 0.0;
    TradingClient::SymbolInfo info;
    if (client_->try_get_symbol_info(symbol, info) && info.valid) {
        // 精度自检：PRICE_FILTER 没解析到时 tick_size 会静默停在默认的 0.01。
        // 这不只影响显示（那一列已改成取数量级兜底，看不出问题），更会让
        // round_price(0.0015, 0.01) 取整成 0，便宜品种的限价单/止损单被拒。
        // 每品种只报一次：有就是有，没有就永久沉默
        if (!info.tick_found && tickWarned_.insert(symbol).second)
            log(QString("⚠ %1 未取到 PRICE_FILTER，tick_size 用的是默认 0.01"
                        "——该品种的限价单/交易所侧止损单价格可能被错误取整")
                    .arg(QString::fromStdString(symbol)), "WARN");
        return info.tick_size;
    }
    ensureSymbolInfoAsync(symbol);
    return 0.0;
}

void MainWindow::unsubscribeIfUnused(const std::string& symbol) {
    if (!ticker_ || !trend_engine_) return;
    // 还有任何 bot 在用这个品种就不能退订。退错了的后果是那个 bot 从此收不到
    // 价格——止损线永远不会被触发，仓位静默裸奔
    for (const auto& b : trend_engine_->get_bots())
        if (b.cfg.symbol == symbol) return;
    ticker_->unsubscribe(symbol);
}

void MainWindow::ensureSymbolInfoAsync(const std::string& symbol) {
    if (!client_ || pendingSymbolFetch_.count(symbol)) return;
    pendingSymbolFetch_.insert(symbol);
    run_async([this, symbol]() {
        client_->get_symbol_info(symbol);   // 结果进入 TradingClient 自己的缓存
        QMetaObject::invokeMethod(this, [this, symbol]() {
            pendingSymbolFetch_.erase(symbol);
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 拉取真实持仓（强平价来源）——每次 onTick 一次，1 个请求覆盖所有 symbol
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshPositions() {
    if (!client_ || posFetchBusy_.load()) return;
    posFetchBusy_.store(true);
    run_async([this]() {
        bool ok = false;
        auto positions = client_->fetch_positions(&ok);
        posFetchBusy_.store(false);
        QMetaObject::invokeMethod(this, [this, ok, positions = std::move(positions)]() {
            // 拉取失败时【保留上一份缓存、不更新时间戳】：空的返回值有歧义——
            // 既可能是"账户确实没有持仓"，也可能是这次请求失败了。
            // 周期对账靠 posCacheMs_ 的新鲜度来区分，误判的代价是凭空清掉真实持仓
            if (!ok) return;
            pos_cache_.clear();
            for (const auto& p : positions) {
                pos_cache_[p.symbol + (p.direction > 0 ? "_L" : "_S")] = p;
            }
            posCacheMs_ = QDateTime::currentMSecsSinceEpoch();
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Bot 表格刷新
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshBotTable() {
    if (!trend_engine_) { botTable_->setRowCount(0); return; }

    // 行序 = 品种字典序。v4.5.0~v4.7.1 这里要把两个引擎的 bot 合成一个 BotRow
    // 序列（裸指针 + 排序键），DCA 移除后只有一个来源，直接按品种排就是最终行序
    auto bots = trend_engine_->get_bots();
    std::sort(bots.begin(), bots.end(),
              [](const TrendBot& a, const TrendBot& b) { return a.cfg.symbol < b.cfg.symbol; });

    botTable_->setRowCount((int)bots.size());
    // 行数变了就把键表整体作废——行与 bot 的对应关系已经错位
    if (opRowKeys_.size() != bots.size()) opRowKeys_.assign(bots.size(), QString());

    RowTotals rt;
    int&    running      = rt.running;
    int&    cooling      = rt.cooling;
    int&    stopped      = rt.stopped;
    double& total_unreal = rt.unreal;
    double& total_real   = rt.real;

    auto mkc = [](const QString& s, const QColor& c,
                  Qt::Alignment align = Qt::AlignCenter) {
        auto* it = new QTableWidgetItem(s);
        it->setTextAlignment(align);
        it->setForeground(c);
        return it;
    };

    // fmt_price 随 DCA 填充体一并删除（均价/强平价/保证金那几列由 fillTrendRow 自己格式化）。
    // ⚠ MSVC 对未使用的 lambda 不报警，只有 Clang 的 -Wunused-variable 会——
    //   本机编得过，推上去才发现

    int64_t now_ms = BookTickerStream::now_ms();

    for (int i = 0; i < (int)bots.size(); ++i) {
        // ── 品种级列（6 标记价 / 7 24h涨跌 / 8 延迟）────────────────────────
        // 这三列和策略无关，单独填在这里而不是塞进 fillTrendRow：
        // refreshLiveQuotes 每 100ms 会再刷一遍同样的三列，两处口径必须一致
        const std::string& sym = bots[(size_t)i].cfg.symbol;
        const auto   tick      = ticker_ ? ticker_->get(sym) : BookTickerStream::Tick{};
        const double tick_size = tickSizeOf(sym);
        {
            botTable_->setItem(i, ColMark, make_mark_cell(tick, tick_size));
            // 必须分两步：把 chg24Of(...) 和 pct 写在同一个实参列表里，等于在一个
            // 表达式内既通过引用【写】pct 又【读】pct，而 C++ 的函数实参求值顺序是
            // 【未指定】的——MSVC 从右往左，先把还是初始值的 pct(0) 拷进参数，
            // chg24Of 之后才执行，于是永远显示 +0.00%。引擎侧走的是另一条分支所以
            // 数据是对的，只有界面错，极难对上号
            double pct = 0; bool stale = false;
            const bool has = chg24Of(sym, pct, stale);
            botTable_->setItem(i, ColChg24, make_chg24_cell(has, pct));

            // 延迟必须测【引擎实际使用的那条流】。此前测的是 bookTicker，而 v4.0.9
            // 之后引擎决策用的是标记价——markPrice 停了、bookTicker 还在的时候，
            // 这一格显示绿色，引擎却已经在走 REST 兜底。健康指示器指错了对象，
            // 这也是标记价那次故障全程没有任何征兆的原因
            // ⚠ ws_mark_ms 而不是 mark_ms，理由同 100ms 快刷那一处：
            //   mark_ms 会被 REST 兜底刷新，用它的话 WS 死了这格还是绿的
            const int64_t latency = (tick.ws_mark_ms > 0) ? (now_ms - tick.ws_mark_ms) : -1;
            // 阈值按 markPrice@1s 的节奏定：正常包龄在 0~1000ms 之间均匀分布，
            // 用旧的 100/500ms 会一直显示红色。超过 3 秒说明丢了两三包，
            // 超过 kStaleMs(10s) 引擎就当它断流转 REST 了
            const QColor lat_c = (latency < 0)    ? QColor("#484f58")
                               : (latency < 1500) ? QColor("#3fb950")
                               : (latency < 3000) ? QColor("#d29922")
                                                  : QColor("#f85149");
            botTable_->setItem(i, ColLatency,
                mkc(latency >= 0 ? QString("%1ms").arg(latency) : "--", lat_c));
        }

        // 策略相关的列（含操作列与它的重建去抖键）整片由 trend_panel.cpp 填。
        // v4.7.1 之前这里还有一个 kind 分派和三百多行的 DCA 填充体
        fillTrendRow(i, bots[(size_t)i], rt);
    }

    // 汇总
    QColor sum_c = (total_unreal + total_real >= 0) ? QColor("#3fb950") : QColor("#f85149");
    summaryLabel_->setText(
        QString("运行中: %1   冷却: %2   已停止: %3   |   "
                "未实现: %4$%5   已实现: %6$%7")
        .arg(running).arg(cooling).arg(stopped)
        .arg(total_unreal >= 0 ? "+" : "").arg(std::abs(total_unreal), 0, 'f', 2)
        .arg(total_real   >= 0 ? "+" : "").arg(std::abs(total_real),   0, 'f', 2));
    summaryLabel_->setStyleSheet(
        QString("QLabel{color:%1;font-size:11px;padding:4px 8px;"
                "background:#161b22;border:1px solid #21262d;border-radius:3px;}")
        .arg(sum_c.name()));

    // 顶部常驻：权益/可用
    if (equityLabel_) {
        if (account_info_.ok) {
            equityLabel_->setText(QString("权益: $%1   可用: $%2")
                .arg(account_info_.total_equity, 0, 'f', 2)
                .arg(account_info_.available,    0, 'f', 2));
            equityLabel_->setStyleSheet("color:#e6edf3;font-size:11px;");
        } else {
            equityLabel_->setText("权益: --   可用: --");
            equityLabel_->setStyleSheet("color:#8b949e;font-size:11px;");
        }
    }

    // 顶部常驻：uniMMR（仅统一账户）。币安 1.05 起强制减仓，所以 1.3 以下标红、
    // 2.0 以下标黄。无持仓时币安返回一个极大的哨兵值，显示成 ∞ 而不是一串数字
    if (mmrLabel_) {
        const bool has_mmr = account_info_.ok && account_info_.uni_mmr > 0;
        mmrLabel_->setVisible(has_mmr);
        if (has_mmr) {
            const double m = account_info_.uni_mmr;
            QString v = (m >= 1e6) ? QString("∞") : QString::number(m, 'f', 2);
            QString col = (m < 1.3) ? "#f85149" : (m < 2.0) ? "#d29922" : "#e6edf3";
            mmrLabel_->setText(QString("   uniMMR: %1").arg(v));
            mmrLabel_->setStyleSheet(QString("color:%1;font-size:11px;").arg(col));
            mmrLabel_->setToolTip(
                "统一账户维持保证金率（全账户口径）。\n"
                "币安在 1.05 开始强制减仓——这是唯一能看到真实强平距离的数，\n"
                "只看 U 本位子账户的保证金会低估风险。");
        }
    }

    // 顶部常驻：限流状态。只在被限速/被封禁时出现——平时不占位置，
    // 出现即意味着请求量已经顶到交易所配额，是要处理的信号
    if (rateLabel_ && client_) {
        auto rs = client_->rate_status();
        const bool show = rs.banned || rs.throttled > 0 || rs.rejected > 0;
        rateLabel_->setVisible(show);
        if (show) {
            QString t;
            QString col = "#8b949e";
            if (rs.banned) {
                t = QString("   限流: 已封禁 %1s").arg(rs.ban_left_ms / 1000);
                col = "#f85149";
            } else {
                t = QString("   限流: %1/%2").arg(rs.used_weight).arg(rs.limit);
                col = (rs.used_weight > rs.limit * 0.75) ? "#d29922" : "#8b949e";
            }
            rateLabel_->setText(t);
            rateLabel_->setStyleSheet(QString("color:%1;font-size:11px;").arg(col));
            rateLabel_->setToolTip(
                QString("交易所 REST 配额（按 IP 计，权重由服务器响应头回报）\n"
                        "本分钟已用 %1 / %2\n"
                        "本地推迟过 %3 次请求，收到过 %4 次交易所限流拒绝\n\n"
                        "订单请求享有优先权：只在真正被封禁时才等，不参与权重软限速。")
                    .arg(rs.used_weight).arg(rs.limit).arg(rs.throttled).arg(rs.rejected));
        }
    }

    // 顶部常驻：账户累计资金费。只在非零时显示——没持过仓的时候不该占顶部栏的位置
    if (fundLabel_) {
        double sum = 0;
        for (const auto& sy : funding_.symbols()) sum += funding_.get(sy).total;
        const bool show = (sum != 0);
        fundLabel_->setVisible(show);
        if (show) {
            fundLabel_->setText(QString("   资金费: %1$%2")
                .arg(sum >= 0 ? "+" : "-").arg(std::abs(sum), 0, 'f', 2));
            fundLabel_->setStyleSheet(QString("color:%1;font-size:11px;")
                .arg(sum >= 0 ? "#3fb950" : "#f85149"));
            fundLabel_->setToolTip(
                "所有品种累计的资金费（永续每8小时结算一次）。\n"
                "这是【真实划走的现金】，不是浮亏——价格涨回来也拿不回来。\n"
                "逐品种明细见表格里\"均价\"列的悬停提示（带 * 的行）。");
        }
    }
}

} // namespace ccg
