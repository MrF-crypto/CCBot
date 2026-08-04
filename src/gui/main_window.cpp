#include "gui/main_window.h"
#include "core/key_store.h"
#include "net/alert.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QSplitter>
#include <QMessageBox>
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

namespace ccg {

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
    , pool_(std::make_shared<ThreadPool>(2))        // 引擎专用：下单/平仓，绝不排队
    , fetchPool_(std::make_shared<ThreadPool>(4))   // 数据拉取专用：慢任务全在这
{
    setWindowTitle("CCG 合约监控  v2.8");
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
    if (ticker_) ticker_->stop();
    if (tick_timer_) tick_timer_->stop();
    if (ob_timer_)   ob_timer_->stop();
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
    if (dst == old) return;                                // 回退模式，无需迁移
    if (QFile::exists(dst + "/ccg_bots.json")) return;     // 新目录已有数据，不动

    int n = 0;
    for (const char* f : {"ccg_creds.dat", "ccg_bots.json", "ccg_trades.json",
                          "ccg_settings.json"}) {
        QString src = old + "/" + f;
        if (QFile::exists(src) && QFile::copy(src, dst + "/" + f)) ++n;
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
    if (c.api_key.empty() || c.api_secret.empty()) return;
    if (!KeyStore::save(c, cred_path()))
        log("API Key 本地保存失败（DPAPI 加密或写盘出错）", "ERR");
}

void MainWindow::load_credentials() {
    KeyStore::Creds c;
    if (!KeyStore::load(c, cred_path())) return;
    apiKey_    = QString::fromStdString(c.api_key);
    apiSecret_ = QString::fromStdString(c.api_secret);
    testnet_   = c.testnet;
    log("已自动载入保存的 API Key", "OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// 持久化：全局设置（账户级总保证金上限 / 警报 webhook）
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::save_settings() {
    QJsonObject o;
    o["max_total_margin"] = maxTotalMargin_;
    o["alert_webhook"]    = alertWebhook_;
    QFile f(QString::fromStdString(settings_path()));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson());
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
// 持久化：Bot 配置
// ─────────────────────────────────────────────────────────────────────────────
static qint64 tp_to_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}
static std::chrono::system_clock::time_point ms_to_tp(qint64 ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

void MainWindow::save_bots() {
    if (!engine_) return;
    QJsonArray arr;
    for (const auto& b : engine_->get_bots()) {
        // 已停止的 bot 也要存盘：加完品种默认就是停止状态，等右键配置/手动开启，
        // 不存的话重启一次就从自选列表里消失了
        const auto& c = b.cfg;
        QJsonObject o;
        o["symbol"]       = QString::fromStdString(c.symbol);
        o["strat_type"]   = (int)c.strat_type;
        o["direction"]    = (int)c.direction;
        o["budget_usdt"]  = c.budget_usdt;
        o["leverage"]     = c.leverage;
        o["max_entries"]  = c.max_entries;
        o["interval_pct"] = c.interval_pct;
        o["trail_entry"]  = c.trail_entry;
        o["tp_pct"]       = c.tp_pct;
        o["trail_tp"]     = c.trail_tp;
        o["auto_restart"] = c.auto_restart;
        o["cooldown_secs"]= c.cooldown_secs;
        o["stop_loss_pct"]= c.stop_loss_pct;

        o["entry_mode"]     = (int)c.entry_mode;
        o["kline_interval"] = QString::fromStdString(c.kline_interval);
        o["boll_period"]    = c.boll_period;
        o["boll_mult"]      = c.boll_mult;
        o["use_rsi_filter"] = c.use_rsi_filter;
        o["rsi_period"]     = c.rsi_period;
        o["rsi_threshold"]  = c.rsi_threshold;
        o["rsi_confirm_mode"] = (int)c.rsi_confirm_mode;
        o["rsi_oversold_th"]  = c.rsi_oversold_th;
        o["dynamic_band_mode"] = c.dynamic_band_mode;
        o["min_profit_floor"]  = c.min_profit_floor;
        o["use_trend_filter"]  = c.use_trend_filter;
        o["trend_interval"]    = QString::fromStdString(c.trend_interval);
        o["trend_ema_period"]  = c.trend_ema_period;
        o["sr_radar"]          = c.sr_radar;
        o["sr_interval"]       = QString::fromStdString(c.sr_interval);

        // 持仓/状态快照 —— 没有这些字段的话，App 重启后本地均价/持仓量会从零重新累积，
        // 跟交易所实际仓位脱节（这正是均价跟交易所对不上的根因之一）
        o["bot_id"]            = QString::fromStdString(b.bot_id);
        o["state"]             = (int)b.state;
        o["avg_price"]         = b.avg_price;
        o["total_qty"]         = b.total_qty;
        o["total_cost"]        = b.total_cost;
        o["current_price"]     = b.current_price;
        o["last_entry_price"]  = b.last_entry_price;
        o["dca_extreme"]       = b.dca_extreme;
        o["interval_hit"]      = b.interval_hit;
        o["tp_reached"]        = b.tp_reached;
        o["tp_extreme"]        = b.tp_extreme;
        o["realized_pnl"]      = b.realized_pnl;
        o["cycle_count"]       = b.cycle_count;
        o["cooldown_until_ms"] = tp_to_ms(b.cooldown_until);

        QJsonArray entries;
        for (const auto& e : b.entries) {
            QJsonObject eo;
            eo["level"]     = e.level;
            eo["price"]     = e.price;
            eo["qty"]       = e.qty;
            eo["cost_usdt"] = e.cost_usdt;
            eo["order_id"]  = QString::fromStdString(e.order_id);
            eo["time_ms"]   = tp_to_ms(e.time);
            entries.append(eo);
        }
        o["entries"] = entries;

        arr.append(o);
    }
    // QSaveFile = 原子保存（写临时文件+commit时改名），进程崩在写文件中途也不会
    // 损坏 ccg_bots.json——这个文件丢了等于仓位跟踪全丢
    QSaveFile f(QString::fromStdString(bot_cfg_path()));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(arr).toJson());
        f.commit();
    }
}

void MainWindow::load_and_restore_bots() {
    QFile f(QString::fromStdString(bot_cfg_path()));
    if (!f.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;

    int restored = 0;
    for (const auto& v : doc.array()) {
        auto o = v.toObject();
        CcgConfig c;
        c.symbol       = o["symbol"].toString().toStdString();
        c.strat_type   = (CcgConfig::StratType)o["strat_type"].toInt(1);
        c.direction    = (CcgConfig::Direction)o["direction"].toInt(0);
        c.budget_usdt  = o["budget_usdt"].toDouble(3000.0);
        c.leverage     = o["leverage"].toInt(3);
        c.max_entries  = o["max_entries"].toInt(6);
        c.interval_pct = o["interval_pct"].toDouble(8.0);
        c.trail_entry  = o["trail_entry"].toDouble(1.0);
        c.tp_pct       = o["tp_pct"].toDouble(5.0);
        c.trail_tp     = o["trail_tp"].toDouble(2.0);
        c.auto_restart = o["auto_restart"].toBool(true);
        c.cooldown_secs= o["cooldown_secs"].toInt(60);
        c.stop_loss_pct= o["stop_loss_pct"].toDouble(0.0);

        c.entry_mode     = (CcgConfig::EntryMode)o["entry_mode"].toInt(0);
        c.kline_interval = o["kline_interval"].toString("1h").toStdString();
        c.boll_period    = o["boll_period"].toInt(20);
        c.boll_mult      = o["boll_mult"].toDouble(2.0);
        c.use_rsi_filter = o["use_rsi_filter"].toBool(true);
        c.rsi_period     = o["rsi_period"].toInt(14);
        c.rsi_threshold  = o["rsi_threshold"].toDouble(30.0);
        c.rsi_confirm_mode = (CcgConfig::RsiConfirmMode)o["rsi_confirm_mode"].toInt(0);
        c.rsi_oversold_th  = o["rsi_oversold_th"].toDouble(25.0);
        c.dynamic_band_mode = o["dynamic_band_mode"].toBool(false);
        c.min_profit_floor  = o["min_profit_floor"].toDouble(0.3);
        c.use_trend_filter  = o["use_trend_filter"].toBool(false);
        c.trend_interval    = o["trend_interval"].toString("4h").toStdString();
        c.trend_ema_period  = o["trend_ema_period"].toInt(200);
        c.sr_radar          = o["sr_radar"].toBool(false);
        c.sr_interval       = o["sr_interval"].toString("4h").toStdString();
        if (c.symbol.empty()) continue;

        CcgBot bot;
        bot.bot_id           = o["bot_id"].toString().toStdString();
        bot.cfg              = c;
        bot.state             = (CcgBot::State)o["state"].toInt((int)CcgBot::State::Running);
        bot.avg_price         = o["avg_price"].toDouble();
        bot.total_qty         = o["total_qty"].toDouble();
        bot.total_cost        = o["total_cost"].toDouble();
        bot.current_price     = o["current_price"].toDouble();
        bot.last_entry_price  = o["last_entry_price"].toDouble();
        bot.dca_extreme       = o["dca_extreme"].toDouble();
        bot.interval_hit      = o["interval_hit"].toBool();
        bot.tp_reached        = o["tp_reached"].toBool();
        bot.tp_extreme        = o["tp_extreme"].toDouble();
        bot.realized_pnl      = o["realized_pnl"].toDouble();
        bot.cycle_count       = o["cycle_count"].toInt();
        bot.cooldown_until    = ms_to_tp((qint64)o["cooldown_until_ms"].toDouble());
        for (const auto& ev : o["entries"].toArray()) {
            auto eo = ev.toObject();
            CcgEntry e;
            e.level     = eo["level"].toInt();
            e.price     = eo["price"].toDouble();
            e.qty       = eo["qty"].toDouble();
            e.cost_usdt = eo["cost_usdt"].toDouble();
            e.order_id  = eo["order_id"].toString().toStdString();
            e.time      = ms_to_tp((qint64)eo["time_ms"].toDouble());
            bot.entries.push_back(e);
        }

        auto id = engine_->restore_bot(bot);
        if (!id.empty()) {
            if (ticker_) ticker_->subscribe(c.symbol);
            ++restored;
        }
    }
    if (restored > 0) {
        log(QString("已恢复 %1 个 Bot 配置").arg(restored), "OK");
        refreshBotTable();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 持久化：交易明细
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::save_trades() {
    QJsonArray arr;
    for (const auto& t : trades_) {
        QJsonObject o;
        o["symbol"]      = QString::fromStdString(t.symbol);
        o["direction"]   = (int)t.direction;
        o["entry_price"] = t.entry_price;
        o["exit_price"]  = t.exit_price;
        o["qty"]         = t.qty;
        o["pnl"]         = t.pnl;
        o["layers"]      = t.layers;
        o["reason"]      = QString::fromStdString(t.reason);
        o["close_time_ms"] = tp_to_ms(t.close_time);
        arr.append(o);
    }
    QFile f(QString::fromStdString(trade_path()));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson());
}

void MainWindow::load_trades() {
    QFile f(QString::fromStdString(trade_path()));
    if (!f.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;
    trades_.clear();
    for (const auto& v : doc.array()) {
        auto o = v.toObject();
        TradeRecord t;
        t.symbol      = o["symbol"].toString().toStdString();
        t.direction   = (CcgConfig::Direction)o["direction"].toInt();
        t.entry_price = o["entry_price"].toDouble();
        t.exit_price  = o["exit_price"].toDouble();
        t.qty         = o["qty"].toDouble();
        t.pnl         = o["pnl"].toDouble();
        t.layers      = o["layers"].toInt(0);
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

    struct SymStat {
        int cycles = 0; int wins = 0; double pnl = 0;
        int layer_sum = 0; int layer_max = 0;
        std::chrono::system_clock::time_point first_close{}, last_close{};
    };
    std::map<std::string, SymStat> stats;
    for (const auto& t : trades_) {
        auto& s = stats[t.symbol];
        if (s.cycles == 0) s.first_close = t.close_time;
        s.cycles++;
        if (t.pnl > 0) s.wins++;
        s.pnl       += t.pnl;
        s.layer_sum += t.layers;
        s.layer_max  = std::max(s.layer_max, t.layers);
        s.last_close = t.close_time;
    }

    auto* statTable = new QTableWidget((int)stats.size(), 7);
    statTable->setHorizontalHeaderLabels(
        {"品种","周期数","周期/周","胜率","累计盈亏","平均层数","最大层数"});
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
        double avgLayer = s.cycles ? (double)s.layer_sum / s.cycles : 0;
        statTable->setItem(row, 0, mkc(QString::fromStdString(sym), QColor("#e6edf3")));
        statTable->setItem(row, 1, mkc(QString::number(s.cycles), QColor("#8b949e")));
        statTable->setItem(row, 2, mkc(s.cycles > 1 ? QString::number(perWeek, 'f', 1) : "--",
                                        QColor("#8b949e")));
        statTable->setItem(row, 3, mkc(QString("%1%").arg(winRate, 0, 'f', 0), QColor("#8b949e")));
        statTable->setItem(row, 4, mkc(QString("%1$%2").arg(s.pnl >= 0 ? "+" : "")
                                        .arg(std::abs(s.pnl), 0, 'f', 2),
                                        s.pnl >= 0 ? QColor("#3fb950") : QColor("#f85149")));
        statTable->setItem(row, 5, mkc(QString::number(avgLayer, 'f', 1), QColor("#8b949e")));
        statTable->setItem(row, 6, mkc(QString::number(s.layer_max), QColor("#8b949e")));
        ++row;
    }
    dv->addWidget(statTable);

    auto* statHint = new QLabel(
        "调参提示：平均层数长期 <2 → 间隔偏宽（吃不进层）；最大层数经常顶满 → 间隔偏窄或趋势过滤失效；"
        "周期/周 × 平均盈亏 = 该品种的真实产能。");
    statHint->setWordWrap(true);
    statHint->setStyleSheet("color:#8b949e;font-size:10px;");
    dv->addWidget(statHint);

    // ── 交易明细 ─────────────────────────────────────────────────────────────
    auto* histTitle = new QLabel("交易明细");
    histTitle->setStyleSheet("color:#58a6ff;font-size:11px;font-weight:bold;padding-top:6px;");
    dv->addWidget(histTitle);

    auto* table = new QTableWidget(0, 9);
    table->setHorizontalHeaderLabels(
        {"时间","品种","方向","开仓价","平仓价","数量","盈亏","层数","原因"});
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
        table->setItem(i, 2, mkc(t.direction == CcgConfig::Direction::Short ? "空" : "多",
                                  t.direction == CcgConfig::Direction::Short
                                      ? QColor("#f85149") : QColor("#3fb950")));
        table->setItem(i, 3, mkc(QString("$%1").arg(t.entry_price, 0, 'f', 4), QColor("#8b949e")));
        table->setItem(i, 4, mkc(QString("$%1").arg(t.exit_price,  0, 'f', 4), QColor("#8b949e")));
        table->setItem(i, 5, mkc(QString::number(t.qty, 'f', 4), QColor("#8b949e")));
        table->setItem(i, 6, mkc(QString("%1$%2").arg(t.pnl >= 0 ? "+" : "").arg(std::abs(t.pnl), 0, 'f', 2),
                                  t.pnl >= 0 ? QColor("#3fb950") : QColor("#f85149")));
        table->setItem(i, 7, mkc(t.layers > 0 ? QString::number(t.layers) : "--", QColor("#8b949e")));
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

    auto* testnetBox = new QCheckBox("测试网（币安合约测试网，需要单独申请测试用Key）");
    testnetBox->setChecked(testnet_);
    credForm->addRow("", testnetBox);

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
        "触发硬止损平仓、账户连接失败时会 POST 一条消息过去（{\"text\":\"...\"}）。"
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
            bool ok = send_webhook(url.toStdString(), "CCGMonitor 测试消息：webhook 配置成功", &err);
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

    apiKey_    = apiKeyEdit->text().trimmed();
    apiSecret_ = secretEdit->text().trimmed();
    testnet_   = testnetBox->isChecked();
    save_credentials();

    bool ok;
    double m = marginEdit->text().toDouble(&ok);
    maxTotalMargin_ = (ok && m > 0) ? m : 0.0;
    alertWebhook_   = webhookEdit->text().trimmed();

    if (engine_) engine_->set_max_total_margin(maxTotalMargin_);
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
        logBox_->append(QString("<font color='%1'>%2 %3</font>")
                        .arg(color).arg(ts).arg(msg.toHtmlEscaped()));
        auto* sb = logBox_->verticalScrollBar();
        sb->setValue(sb->maximum());
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

        // Bot 表格 — 15 列
        botTable_ = new QTableWidget(0, 15);
        botTable_->setHorizontalHeaderLabels(
            {"#","品种","方向","策略","层进度",
             "均价","最新成交价","延迟","浮动P&L","保证金","收益率","强平价",
             "已实现","状态","操作"});
        auto* hdr = botTable_->horizontalHeader();
        hdr->setSectionResizeMode(QHeaderView::Stretch);
        for (int c : {0, 4, 7, 13})
            hdr->setSectionResizeMode(c, QHeaderView::Fixed);
        hdr->resizeSection(0, 26);
        hdr->resizeSection(4, 54);
        hdr->resizeSection(7, 60);
        hdr->resizeSection(13, 60);
        hdr->setSectionResizeMode(14, QHeaderView::Fixed);
        hdr->resizeSection(14, 175);

        botTable_->verticalHeader()->setVisible(false);
        botTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        botTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        botTable_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        botTable_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(botTable_, &QTableWidget::customContextMenuRequested,
                this, &MainWindow::onWatchlistContextMenu);
        tv->addWidget(botTable_, 1);

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

        logBox_ = new QTextEdit();
        logBox_->setReadOnly(true);
        logBox_->setMaximumHeight(160);
        logBox_->setStyleSheet("QTextEdit{font-size:11px;font-family:Consolas;}");
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
    cfg.testnet    = testnet_;

    // 无论本次连接是否成功都先保存，避免限流/网络失败导致密钥丢失、下次仍需手动输入
    save_credentials();

    connLabel_->setText("连接中...");
    connLabel_->setStyleSheet("color:#d29922;font-size:11px;");
    setConnState(ConnState::Connecting);
    btnConnect_->setEnabled(false);

    run_async([this, cfg]() {
        auto client = std::make_shared<TradingClient>(cfg);
        client->sync_server_time();
        auto info = client->fetch_account();

        QMetaObject::invokeMethod(this, [this, client, info, cfg]() {
            btnConnect_->setEnabled(true);
            if (!info.ok) {
                connLabel_->setText("连接失败: " + QString::fromStdString(info.error));
                connLabel_->setStyleSheet("color:#f85149;font-size:11px;");
                setConnState(ConnState::Failed);
                log("连接失败: " + QString::fromStdString(info.error), "ERR");
                if (!alertedDisconnect_) {
                    alertedDisconnect_ = true;
                    sendAlert(QString("[CCGMonitor] 账户连接失败: %1")
                              .arg(QString::fromStdString(info.error)));
                }
                return;
            }

            client_ = client;
            engine_ = std::make_shared<CcgEngine>(client_, pool_);
            engine_->set_max_total_margin(maxTotalMargin_);
            engine_->set_log_cb([this](const std::string& msg) {
                QMetaObject::invokeMethod(this, [this, msg]() {
                    log(QString::fromStdString(msg));
                    refreshBotTable();
                    save_bots();
                }, Qt::QueuedConnection);
            });
            engine_->set_trade_cb([this](const TradeRecord& tr) {
                QMetaObject::invokeMethod(this, [this, tr]() {
                    trades_.push_back(tr);
                    save_trades();
                    refreshStats();
                    if (tr.reason == "硬止损") {
                        sendAlert(QString("[CCGMonitor] %1 触发硬止损平仓 | 均价 $%2 → 平仓 $%3 | 盈亏 %4$%5")
                            .arg(QString::fromStdString(tr.symbol))
                            .arg(tr.entry_price, 0, 'f', 4).arg(tr.exit_price, 0, 'f', 4)
                            .arg(tr.pnl >= 0 ? "+" : "").arg(std::abs(tr.pnl), 0, 'f', 2));
                    }
                }, Qt::QueuedConnection);
            });

            account_info_       = info;
            connNetName_        = cfg.testnet ? "测试网" : "主网";
            alertedDisconnect_  = false;
            setConnState(ConnState::Connected);   // 顶部计时从此刻开始，文本由 updateHeader() 接管

            // 启动盘口 WebSocket
            ticker_ = std::make_unique<BookTickerStream>(cfg.testnet);
            ticker_->start();

            tick_timer_->start();

            log(QString("连接成功 | %1 | 权益 $%2 | 可用 $%3")
                .arg(cfg.testnet ? "测试网" : "主网")
                .arg(info.total_equity, 0, 'f', 2)
                .arg(info.available,    0, 'f', 2), "OK");

            // 恢复上次保存的 Bot
            load_and_restore_bots();

            // 启动对账：本地跟踪的仓位 vs 交易所实际持仓。外部手动平过仓/强平过的话，
            // 本地状态是错的，带着错误均价继续跑会把止盈止损全算错
            run_async([this]() {
                if (!client_ || !engine_) return;
                auto ex_pos = client_->fetch_positions();
                QMetaObject::invokeMethod(this, [this, ex_pos]() {
                    if (!engine_) return;
                    std::vector<CcgEngine::ExchangePos> ex;
                    for (const auto& p : ex_pos) ex.push_back({p.symbol, p.direction, p.qty});
                    auto issues = engine_->reconcile_positions(ex);
                    if (!issues.empty()) {
                        save_bots();   // 收敛后的状态立刻落盘
                        refreshBotTable();
                        QString msg = QString("[CCGMonitor] 启动对账发现 %1 处不一致，详见日志").arg(issues.size());
                        sendAlert(msg);
                    }
                }, Qt::QueuedConnection);
            });
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// SR雷达（影子模式）：自动支撑/阻力区检测 + 触区告警 + 展示，不参与下单
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshSrZones() {
    if (!client_ || !engine_) return;
    // 收集开了 SR 雷达的品种（同品种多bot取任一配置的周期）
    std::map<std::string, std::string> radar;   // symbol → interval
    for (const auto& b : engine_->get_bots())
        if (b.cfg.sr_radar && b.state != CcgBot::State::Stopped)
            radar.emplace(b.cfg.symbol, b.cfg.sr_interval);
    if (radar.empty()) return;

    run_async([this, radar]() {
        for (const auto& [sym, interval] : radar) {
            auto raw = client_->fetch_bars(sym, interval, 400);
            if (raw.size() < 50) continue;   // 新品种历史不够，跳过
            std::vector<srzones::Bar> bars;
            bars.reserve(raw.size());
            for (const auto& r : raw) bars.push_back({r.open, r.high, r.low, r.close, r.volume});
            auto zones = srzones::detect_zones(bars);
            QMetaObject::invokeMethod(this, [this, sym, zones = std::move(zones)]() {
                auto& st = srStates_[sym];
                st.zones       = zones;
                st.computed_ms = QDateTime::currentMSecsSinceEpoch();
            }, Qt::QueuedConnection);
        }
    });
}

void MainWindow::checkSrTouches() {
    if (!ticker_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto& [sym, st] : srStates_) {
        if (st.zones.empty()) continue;
        double price = ticker_->mid_price(sym);
        if (price <= 0) continue;

        for (const auto& z : st.zones) {
            if (!z.contains(price)) continue;
            // 去重：同一区域2小时内只报一次（换了区域立刻可报）
            bool same_zone = std::fabs(z.mid() - st.last_alert_mid) < 1e-9;
            if (same_zone && (now - st.last_alert_ms) < 2 * 3600 * 1000) break;
            st.last_alert_mid = z.mid();
            st.last_alert_ms  = now;

            int conf = z.confluence();
            QString kind = QString::fromStdString(srzones::src_label(z));
            if (conf >= 2) kind += QString(" ×%1共振").arg(conf);
            QString msg = QString("[SR雷达] %1 价格 %2 进入区域[%3]（%4 ~ %5，评分%6，触碰%7次）")
                .arg(QString::fromStdString(sym)).arg(price, 0, 'f', 4).arg(kind)
                .arg(z.lo, 0, 'f', 4).arg(z.hi, 0, 'f', 4)
                .arg(z.score, 0, 'f', 1).arg(z.touches);
            log(msg, "WARN");
            sendAlert(msg);
            break;   // 一个tick最多报一个区域
        }
    }
}

void MainWindow::openSrZonesDialog(const std::string& symbol) {
    auto it = srStates_.find(symbol);
    double price = ticker_ ? ticker_->mid_price(symbol) : 0;

    QDialog dlg(this);
    dlg.setWindowTitle(QString("支撑/阻力区 - %1").arg(QString::fromStdString(symbol)));
    dlg.resize(620, 420);
    auto* dv = new QVBoxLayout(&dlg);

    if (it == srStates_.end() || it->second.zones.empty()) {
        auto* lbl = new QLabel(
            "暂无区域数据。\n\n请先在该品种的策略配置里勾选【SR雷达】，"
            "开启后约15分钟内完成首次计算（4h K线，摆动点聚类+FVG检测）。");
        lbl->setWordWrap(true);
        dv->addWidget(lbl);
    } else {
        auto& st = it->second;
        auto* info = new QLabel(QString("现价 %1  |  区域按价格从高到低排列，绿色=现价所在区域  |  更新于 %2")
            .arg(price, 0, 'f', 4)
            .arg(QDateTime::fromMSecsSinceEpoch(st.computed_ms).toString("HH:mm:ss")));
        info->setStyleSheet("color:#8b949e;font-size:11px;");
        dv->addWidget(info);

        auto zones = st.zones;
        std::sort(zones.begin(), zones.end(),
                  [](const srzones::Zone& a, const srzones::Zone& b) { return a.mid() > b.mid(); });

        auto* table = new QTableWidget((int)zones.size(), 7);
        table->setHorizontalHeaderLabels({"类型","共振","区间下沿","区间上沿","距现价%","触碰","评分"});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        for (int i = 0; i < (int)zones.size(); ++i) {
            const auto& z = zones[i];
            bool at_zone = price > 0 && z.contains(price);
            QColor c = at_zone ? QColor("#3fb950")
                     : (price > 0 && z.hi < price) ? QColor("#58a6ff")   // 下方=潜在支撑
                                                    : QColor("#d29922"); // 上方=潜在阻力
            auto mk = [&](const QString& s) {
                auto* itc = new QTableWidgetItem(s);
                itc->setTextAlignment(Qt::AlignCenter);
                itc->setForeground(c);
                return itc;
            };
            double dist = price > 0 ? (z.mid() - price) / price * 100.0 : 0;
            int conf = z.confluence();
            table->setItem(i, 0, mk(QString::fromStdString(srzones::src_label(z))));
            table->setItem(i, 1, mk(conf >= 2 ? QString("×%1").arg(conf) : "-"));
            table->setItem(i, 2, mk(QString::number(z.lo, 'f', 4)));
            table->setItem(i, 3, mk(QString::number(z.hi, 'f', 4)));
            table->setItem(i, 4, mk(QString("%1%2%").arg(dist >= 0 ? "+" : "").arg(dist, 0, 'f', 2)));
            table->setItem(i, 5, mk(QString::number(z.touches)));
            table->setItem(i, 6, mk(QString::number(z.score, 'f', 1)));
        }
        dv->addWidget(table);
    }

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close);
    dv->addWidget(btnBox);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.exec();
}

// ─────────────────────────────────────────────────────────────────────────────
// 权益/可用：随 onTick 异步刷新，不再只在连接那一刻查询一次
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshAccount() {
    if (!client_) return;
    run_async([this]() {
        auto info = client_->fetch_account();
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
            // 单次失败不覆盖上一次的有效账户数据，但要计数——连续失败才判定为网络异常，
            // 避免偶尔一次超时就报警
            ++netFailCount_;
            if (netFailCount_ >= 3 && connState_ == ConnState::Connected) {
                connState_ = ConnState::NetworkError;
                log(QString("检测到网络异常（账户接口连续 %1 次拉取失败）: %2")
                    .arg(netFailCount_).arg(QString::fromStdString(info.error)), "ERR");
                if (!alertedDisconnect_) {
                    alertedDisconnect_ = true;
                    sendAlert(QString("[CCGMonitor] 检测到网络异常，账户接口连续拉取失败: %1")
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
        if (connState_ == ConnState::NetworkError) {
            connLabel_->setText("⚠ 网络异常(重试中) | " + timeStr);
            connLabel_->setStyleSheet("color:#f85149;font-size:11px;");
        } else {
            connLabel_->setText("已连接 | " + timeStr);
            connLabel_->setStyleSheet("color:#3fb950;font-size:11px;");
        }
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

    if (!engine_) { log("请先点击【连接】", "WARN"); return; }

    // 只需要输入代币符号（不区分大小写），默认按 USDT 永续合约补全后缀
    if (raw.endsWith("USDT")) raw.chop(4);
    if (raw.isEmpty()) return;
    std::string symbol = (raw + "USDT").toStdString();

    for (const auto& b : engine_->get_bots()) {
        if (b.cfg.symbol == symbol) {
            log(QString::fromStdString(symbol) + " 已经在列表里了", "WARN");
            return;
        }
    }

    CcgConfig cfg;              // 全部用默认参数，具体配置留给右键弹窗
    cfg.symbol = symbol;
    auto id = engine_->add_bot(cfg);
    if (id.empty()) return;
    engine_->stop_bot(id);      // 初始状态为停止，需要手动配置 + 开启监控
    if (ticker_) ticker_->subscribe(symbol);

    log(QString::fromStdString(symbol) + " 已添加（已停止），右键进行策略配置", "OK");
    refreshBotTable();
    save_bots();
}

void MainWindow::onWatchlistContextMenu(const QPoint& pos) {
    if (!botTable_) return;
    auto* item = botTable_->itemAt(pos);
    if (!item) return;
    auto* symItem = botTable_->item(item->row(), 1);   // 品种列
    if (!symItem) return;
    std::string sym = symItem->text().toStdString();

    // 右键菜单只留"配置策略"——原先紧挨着的"从列表中删除"太容易误点，删掉的是
    // 整个 bot（含已配好的策略和仓位跟踪），代价太高。要删除品种：先【停止】该
    // bot，再点顶部的【清除已停止】按钮，两步操作天然防误触
    QMenu menu(this);
    QAction* actConfig = menu.addAction("配置策略...");
    QAction* actSr     = menu.addAction("支撑/阻力区...");
    QAction* chosen = menu.exec(botTable_->viewport()->mapToGlobal(pos));
    if (chosen == actConfig) {
        openStrategyDialog(sym);
    } else if (chosen == actSr) {
        openSrZonesDialog(sym);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 策略配置弹窗：新建或编辑一个品种的 bot，保存后回到监控页面
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::openStrategyDialog(const std::string& symbol) {
    auto bots = engine_ ? engine_->get_bots() : std::vector<CcgBot>{};
    const CcgBot* longBot = nullptr;
    const CcgBot* shortBot = nullptr;
    for (const auto& b : bots) {
        if (b.cfg.symbol != symbol) continue;
        if (b.cfg.direction == CcgConfig::Direction::Short) shortBot = &b;
        else                                                longBot  = &b;
    }
    const CcgBot* prefill = longBot ? longBot : shortBot;

    QDialog dlg(this);
    dlg.setWindowTitle(QString("策略配置 - %1").arg(QString::fromStdString(symbol)));
    dlg.resize(820, 820);
    auto* dv = new QVBoxLayout(&dlg);

    auto* form = new QFormLayout();
    form->setSpacing(8);

    auto* dirBox = new QComboBox();
    dirBox->addItem("多(Long)");
    dirBox->addItem("空(Short)");
    dirBox->addItem("双向(Both)");
    if (longBot && shortBot)      dirBox->setCurrentIndex(2);
    else if (prefill)             dirBox->setCurrentIndex(
        prefill->cfg.direction == CcgConfig::Direction::Short ? 1 : 0);
    if (longBot && shortBot) dirBox->setEnabled(false);  // 双向都在跑，方向不用选，两边都会更新
    form->addRow("方向:", dirBox);

    auto* stratBox = new QComboBox();
    for (const char* s : {"平推(Flat)","倍投(Mart)","倍投Plus","三倍(Triple)",
                           "平方(Sq)","斐波那契","卢卡斯","递增(Lin)"})
        stratBox->addItem(s);
    stratBox->setCurrentIndex(prefill ? (int)prefill->cfg.strat_type : 1);
    form->addRow("策略:", stratBox);

    auto mkEdit = [&](const QString& label, double val) {
        auto* e = new QLineEdit(QString::number(val));
        form->addRow(label, e);
        return e;
    };
    auto mkEditI = [&](const QString& label, int val) {
        auto* e = new QLineEdit(QString::number(val));
        form->addRow(label, e);
        return e;
    };

    auto* budgetEdit   = mkEdit ("预算USDT:",      prefill ? prefill->cfg.budget_usdt  : 3000.0);
    auto* levEdit      = mkEditI("杠杆:",          prefill ? prefill->cfg.leverage     : 3);
    auto* maxEntEdit   = mkEditI("最大层:",        prefill ? prefill->cfg.max_entries  : 6);
    auto* intervalEdit = mkEdit ("间隔%:",         prefill ? prefill->cfg.interval_pct : 8.0);
    auto* trailEntEdit = mkEdit ("追踪建仓%:",     prefill ? prefill->cfg.trail_entry  : 1.0);
    auto* tpEdit       = mkEdit ("止盈%:",         prefill ? prefill->cfg.tp_pct       : 5.0);
    auto* trailTpEdit  = mkEdit ("止盈追踪%:",     prefill ? prefill->cfg.trail_tp     : 2.0);
    auto* cooldownEdit = mkEditI("冷却(s):",       prefill ? prefill->cfg.cooldown_secs: 60);
    auto* stopLossEdit = mkEdit ("止损%(0=禁用):", prefill ? prefill->cfg.stop_loss_pct : 0.0);

    auto* autoRestartBox = new QCheckBox("自动重启");
    autoRestartBox->setChecked(prefill ? prefill->cfg.auto_restart : true);
    form->addRow("", autoRestartBox);

    auto* entryModeBox = new QComboBox();
    entryModeBox->addItem("立即开仓（一开监控就开首仓）");
    entryModeBox->addItem("指标信号（BOLL+RSI 满足才开首仓）");
    entryModeBox->setCurrentIndex(
        prefill && prefill->cfg.entry_mode == CcgConfig::EntryMode::Indicator ? 1 : 0);
    form->addRow("首单模式:", entryModeBox);

    // ── 动态W模式（v2.3）───────────────────────────────────────────────────────
    auto* dynBandBox = new QCheckBox("动态W模式（补仓锚定下轨/止盈锚定上轨/间距自适应带宽）");
    dynBandBox->setChecked(prefill ? prefill->cfg.dynamic_band_mode : false);
    dynBandBox->setToolTip(
        "开启后间隔%/追踪%不再用上面的固定值，改为按实时布林带宽W自动推导：\n"
        "间隔=W/3、追踪止盈=0.15W、追踪建仓=0.1W（各有上下限夹逼）。\n"
        "补仓要求价格在带外（多:≤下轨），止盈要求触及对侧轨道且盈利≥保底利润。\n"
        "带子随趋势移动时梯子跟着走，均价贴着下轨，带内震荡即可完成周期。");
    form->addRow("", dynBandBox);
    auto* floorEdit = mkEdit("保底利润%(动态模式):",
                             prefill ? prefill->cfg.min_profit_floor : 0.3);

    // ── SR雷达（v2.6 影子模式）───────────────────────────────────────────────
    auto* srBox = new QCheckBox("SR雷达（自动检测支撑/阻力区+触区告警，不参与下单）");
    srBox->setChecked(prefill ? prefill->cfg.sr_radar : false);
    srBox->setToolTip(
        "自动检测该品种 4h 级别的支撑/阻力区域（摆动点聚类+攻防转换+FVG缺口），\n"
        "约15分钟刷新一次。价格触及区域时打日志+webhook告警（同区域2小时去重）。\n"
        "右键品种→【支撑/阻力区...】查看当前区域列表。\n"
        "影子模式：只观察不下单——先验证'程序的眼睛'准不准，执行接线是下一阶段。");
    form->addRow("", srBox);

    // ── 趋势过滤（v2.5）──────────────────────────────────────────────────────
    auto* trendBox = new QCheckBox("趋势过滤（4h EMA200+中轨斜率：空头态暂停新首仓、补仓间隔×1.5）");
    trendBox->setChecked(prefill ? prefill->cfg.use_trend_filter : false);
    trendBox->setToolTip(
        "高周期趋势判定：价格在 4h EMA200 之下 且 中轨明显下拐 = 空头态。\n"
        "空头态期间不开新首仓（不接单边下跌的飞刀），已有仓位补仓间隔放大1.5倍。\n"
        "趋势数据每5分钟刷新一次；数据缺失时过滤自动失效，不会卡死交易。");
    form->addRow("", trendBox);

    dv->addLayout(form);

    // ── 指标信号配置（entryModeBox 选"指标信号"时才用得上）──────────────────────
    auto* indBox = new QGroupBox("指标信号配置");
    indBox->setStyleSheet("QGroupBox{color:#a371f7;font-size:11px;font-weight:bold;}");
    auto* indForm = new QFormLayout(indBox);
    indForm->setSpacing(6);

    auto* klineBox = new QComboBox();
    for (const char* s : {"1m","5m","15m","30m","1h","2h","4h","1d"}) klineBox->addItem(s);
    klineBox->setCurrentText(prefill ? QString::fromStdString(prefill->cfg.kline_interval) : "1h");
    indForm->addRow("K线周期:", klineBox);

    auto* bollPeriodEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.boll_period : 20));
    indForm->addRow("BOLL周期:", bollPeriodEdit);
    auto* bollMultEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.boll_mult : 2.0));
    indForm->addRow("BOLL倍数:", bollMultEdit);

    auto* rsiFilterBox = new QCheckBox("启用RSI过滤");
    rsiFilterBox->setChecked(prefill ? prefill->cfg.use_rsi_filter : true);
    indForm->addRow("", rsiFilterBox);
    auto* rsiPeriodEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.rsi_period : 14));
    indForm->addRow("RSI周期:", rsiPeriodEdit);
    auto* rsiThEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.rsi_threshold : 30.0));
    indForm->addRow("RSI阈值(多≥/空≤100-此值):", rsiThEdit);

    auto* rsiModeBox = new QComboBox();
    rsiModeBox->addItem("瞬时快照（这一刻RSI到阈值就行）");
    rsiModeBox->addItem("反转确认（先探底跌破，再回穿阈值）");
    rsiModeBox->setCurrentIndex(
        (prefill && prefill->cfg.rsi_confirm_mode == CcgConfig::RsiConfirmMode::CrossFromOversold) ? 1 : 0);
    indForm->addRow("RSI确认方式:", rsiModeBox);

    auto* rsiOversoldEdit = new QLineEdit(
        QString::number(prefill ? prefill->cfg.rsi_oversold_th : 25.0));
    indForm->addRow("探底阈值(仅反转确认用):", rsiOversoldEdit);

    auto* indHint = new QLabel(
        "多：最新价≤BOLL下轨 且 RSI条件成立 才开首仓；空：最新价≥BOLL上轨 且 RSI条件镜像成立。\n"
        "RSI瞬时快照：这一刻RSI≥阈值(多)/≤100-阈值(空)就算数。\n"
        "RSI反转确认：本轮等待期间RSI必须先跌破探底阈值，之后再回穿RSI阈值才算数，更严格，"
        "避免在强趋势下跌中过早进场。\n"
        "指标每3秒用当前未收盘K线实时估算，不等K线收盘。");
    indHint->setWordWrap(true);
    indHint->setStyleSheet("color:#8b949e;font-size:10px;");
    indForm->addRow(indHint);

    auto* indPreviewLbl = new QLabel("指标预览：--");
    indPreviewLbl->setWordWrap(true);
    indPreviewLbl->setStyleSheet("color:#8b949e;font-size:11px;");
    indForm->addRow(indPreviewLbl);

    dv->addWidget(indBox);

    // 弹窗关闭后异步回调不能再碰弹窗里的控件，靠这个存活标记判断
    auto dlgAlive = std::make_shared<std::atomic<bool>>(true);
    connect(&dlg, &QDialog::finished, [dlgAlive](int) { *dlgAlive = false; });
    // 反转确认模式的预览需要跨轮记住"是否已经探底过"，弹窗开着期间本地累积
    auto previewDipped = std::make_shared<bool>(false);

    auto refreshIndPreview = [=]() {
        // 指标模式或动态W模式任一开启都需要预览（动态模式即使首单是"立即开仓"也依赖轨道数据）
        if (!client_ || (entryModeBox->currentIndex() != 1 && !dynBandBox->isChecked())) return;
        bool ok;
        int    bp   = bollPeriodEdit->text().toInt(&ok); if (!ok || bp <= 1) bp = 20;
        double bm   = bollMultEdit->text().toDouble(&ok); if (!ok || bm <= 0) bm = 2.0;
        int    rp   = rsiPeriodEdit->text().toInt(&ok);  if (!ok || rp <= 1) rp = 14;
        double rth  = rsiThEdit->text().toDouble(&ok);   if (!ok) rth = 30.0;
        double ovTh = rsiOversoldEdit->text().toDouble(&ok); if (!ok) ovTh = 25.0;
        bool   useRsi   = rsiFilterBox->isChecked();
        bool   crossMode = (rsiModeBox->currentIndex() == 1);
        bool   is_short = (dirBox->currentIndex() == 1);
        std::string interval = klineBox->currentText().toStdString();

        indPreviewLbl->setText("指标预览：拉取中...");
        run_async([this, dlgAlive, previewDipped, indPreviewLbl, symbol, interval, bp, bm, rp,
                   useRsi, crossMode, rth, ovTh, is_short]() {
            auto snap = client_->fetch_indicators(symbol, interval, bp, bm, rp);
            QMetaObject::invokeMethod(this, [dlgAlive, previewDipped, indPreviewLbl, snap,
                                              useRsi, crossMode, rth, ovTh, is_short]() {
                if (!*dlgAlive) return;
                if (!snap.ok) { indPreviewLbl->setText("指标预览：暂无数据（K线历史不够或网络异常）"); return; }
                bool priceCond = is_short ? (snap.price >= snap.boll_ub) : (snap.price <= snap.boll_lb);

                bool oversoldNow = is_short ? (snap.rsi >= 100.0 - ovTh) : (snap.rsi <= ovTh);
                if (crossMode && oversoldNow) *previewDipped = true;

                bool snapshotHit = is_short ? (snap.rsi <= 100.0 - rth) : (snap.rsi >= rth);
                bool rsiCond = !useRsi || (crossMode ? (*previewDipped && snapshotHit) : snapshotHit);
                bool met = priceCond && rsiCond;

                QString dipTxt = crossMode
                    ? QString("  |  已探底:%1").arg(*previewDipped ? "是" : "否") : "";
                QString txt = QString("指标预览：最新价 %1  |  下轨 %2  上轨 %3  |  RSI %4%5  →  %6")
                    .arg(snap.price,   0, 'f', 4).arg(snap.boll_lb, 0, 'f', 4)
                    .arg(snap.boll_ub, 0, 'f', 4).arg(snap.rsi,     0, 'f', 1)
                    .arg(dipTxt)
                    .arg(met ? "条件已满足" : "条件未满足");
                indPreviewLbl->setStyleSheet(
                    QString("color:%1;font-size:11px;").arg(met ? "#3fb950" : "#d29922"));
                indPreviewLbl->setText(txt);
            }, Qt::QueuedConnection);
        });
    };

    auto* indPreviewTimer = new QTimer(&dlg);
    indPreviewTimer->setInterval(4000);
    connect(indPreviewTimer, &QTimer::timeout, &dlg, refreshIndPreview);

    auto updateIndVisible = [=]() {
        bool show = entryModeBox->currentIndex() == 1 || dynBandBox->isChecked();
        indBox->setVisible(show);
        if (show) { indPreviewTimer->start(); refreshIndPreview(); }
        else        indPreviewTimer->stop();
    };
    connect(entryModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, updateIndVisible);
    connect(dynBandBox,   &QCheckBox::toggled,                                 &dlg, updateIndVisible);
    connect(bollPeriodEdit, &QLineEdit::editingFinished, &dlg, refreshIndPreview);
    connect(bollMultEdit,   &QLineEdit::editingFinished, &dlg, refreshIndPreview);
    connect(rsiPeriodEdit,  &QLineEdit::editingFinished, &dlg, refreshIndPreview);
    connect(rsiThEdit,      &QLineEdit::editingFinished, &dlg, refreshIndPreview);
    connect(rsiOversoldEdit,&QLineEdit::editingFinished, &dlg, refreshIndPreview);
    connect(rsiFilterBox,   &QCheckBox::toggled,          &dlg, refreshIndPreview);
    connect(klineBox,  QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, refreshIndPreview);
    connect(dirBox,    QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, refreshIndPreview);
    connect(rsiModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg,
            [previewDipped, refreshIndPreview]() { *previewDipped = false; refreshIndPreview(); });
    updateIndVisible();

    auto* tierLbl = new QLabel("层级分配预览（第1仓~第N仓各需要多少资金、预计建仓价与浮亏）");
    tierLbl->setStyleSheet("color:#58a6ff;font-size:11px;font-weight:bold;padding-top:4px;");
    dv->addWidget(tierLbl);

    auto* tierTable = new QTableWidget(0, 8);
    tierTable->setHorizontalHeaderLabels(
        {"层", "名义价值", "占比%", "保证金", "预计建仓价", "预计数量", "预计浮亏", "触发条件"});
    tierTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tierTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    tierTable->horizontalHeader()->resizeSection(0, 34);
    tierTable->verticalHeader()->setVisible(false);
    tierTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tierTable->setMaximumHeight(220);
    dv->addWidget(tierTable);

    auto* tierSummary = new QLabel();
    tierSummary->setWordWrap(true);
    tierSummary->setStyleSheet("color:#8b949e;font-size:11px;padding:2px 0;");
    dv->addWidget(tierSummary);

    // symbol 按值捕获，livePrice 每次刷新时重新读取（弹窗开着的时候价格可能会变）
    auto refreshTier = [=]() {
        bool ok;
        double budget = budgetEdit->text().toDouble(&ok);   if (!ok || budget <= 0) budget = 3000.0;
        int    maxEnt = maxEntEdit->text().toInt(&ok);      if (!ok || maxEnt <= 0) maxEnt = 6;
        double interv = intervalEdit->text().toDouble(&ok); if (!ok || interv <= 0) interv = 8.0;
        double trail  = trailEntEdit->text().toDouble(&ok); if (!ok) trail = 1.0;
        int    lev    = levEdit->text().toInt(&ok);         if (!ok || lev <= 0) lev = 3;
        bool   is_short = (dirBox->currentIndex() == 1);

        CcgConfig pc;
        pc.budget_usdt  = budget;
        pc.max_entries  = maxEnt;
        pc.interval_pct = interv;
        pc.strat_type   = static_cast<CcgConfig::StratType>(stratBox->currentIndex());

        auto allocs = CcgEngine::entry_usdt(pc);
        double total = 0;
        for (auto v : allocs) total += v;
        int n = (int)allocs.size();

        // 预计建仓价：跟引擎实际触发逻辑一致——每层相对上一层跌(涨)interval%触发，
        // 再反弹(回落)trail%才真正下单（should_enter() 的镜像）
        double livePrice = ticker_ ? ticker_->mid_price(symbol) : 0.0;
        std::vector<double> predPrice(n, 0.0);
        if (livePrice > 0) {
            predPrice[0] = livePrice;
            for (int i = 1; i < n; ++i) {
                double th = predPrice[i-1] * (is_short ? (1.0 + interv/100.0) : (1.0 - interv/100.0));
                predPrice[i] = th * (is_short ? (1.0 - trail/100.0) : (1.0 + trail/100.0));
            }
        }
        double finalPrice = (n > 0) ? predPrice[n-1] : 0.0;

        auto mkc = [](const QString& s, const QColor& c, int align = Qt::AlignCenter) {
            auto* it = new QTableWidgetItem(s);
            it->setTextAlignment(align);
            it->setForeground(c);
            return it;
        };

        double sumMargin = 0, sumLoss = 0;
        bool   haveLoss  = livePrice > 0;

        tierTable->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            double usdt   = allocs[i];
            double pct    = total > 0 ? usdt / total * 100.0 : 0.0;
            double margin = usdt / lev;
            sumMargin += margin;

            QString trigger = (i == 0) ? "首仓入场"
                : QString("较上层%1%2%%3%").arg(is_short ? "涨" : "跌").arg(interv, 0, 'f', 1)
                      .arg(QString("→反弹%1%").arg(trail, 0, 'f', 1));

            QColor usdt_col = (i == 0) ? QColor("#58a6ff") : QColor("#3fb950");
            tierTable->setItem(i, 0, mkc(QString("第%1").arg(i+1), QColor("#8b949e")));
            tierTable->setItem(i, 1, mkc(QString("$%1").arg(usdt, 0, 'f', 1), usdt_col,
                                          Qt::AlignRight | Qt::AlignVCenter));
            tierTable->setItem(i, 2, mkc(QString("%1%").arg(pct, 0, 'f', 1), QColor("#d29922")));
            tierTable->setItem(i, 3, mkc(QString("$%1").arg(margin, 0, 'f', 2), QColor("#e6edf3"),
                                          Qt::AlignRight | Qt::AlignVCenter));

            if (livePrice > 0) {
                double price = predPrice[i];
                double qty   = (price > 0) ? usdt / price : 0.0;
                double loss  = is_short ? qty * (finalPrice - price) : qty * (price - finalPrice);
                loss = std::max(0.0, loss);
                sumLoss += loss;
                tierTable->setItem(i, 4, mkc(QString::number(price, 'f', 4), QColor("#e6edf3"),
                                              Qt::AlignRight | Qt::AlignVCenter));
                tierTable->setItem(i, 5, mkc(QString::number(qty, 'f', 6), QColor("#8b949e"),
                                              Qt::AlignRight | Qt::AlignVCenter));
                tierTable->setItem(i, 6, mkc(QString("$%1").arg(loss, 0, 'f', 2),
                                              QColor("#f85149"), Qt::AlignRight | Qt::AlignVCenter));
            } else {
                tierTable->setItem(i, 4, mkc("--", QColor("#484f58")));
                tierTable->setItem(i, 5, mkc("--", QColor("#484f58")));
                tierTable->setItem(i, 6, mkc("--", QColor("#484f58")));
            }
            tierTable->setItem(i, 7, mkc(trigger, QColor("#8b949e"),
                                          Qt::AlignLeft | Qt::AlignVCenter));
        }

        double totalInterval = maxEnt * interv;
        QString line1 = QString("共%1层 | 总间隔 %2%3 | 名义价值(预算) $%4 | %5x杠杆 | 保证金合计 $%6")
            .arg(n).arg(totalInterval, 0, 'f', 1).arg("%")
            .arg(budget, 0, 'f', 0).arg(lev).arg(sumMargin, 0, 'f', 2);
        QString line2 = haveLoss
            ? QString("预估浮亏合计（跌/涨到第%1层时）$%2   |   全部建仓完成共需准备资金 ≈ $%3")
                  .arg(n).arg(sumLoss, 0, 'f', 2).arg(sumMargin + sumLoss, 0, 'f', 2)
            : QString("暂无实时价格，预计建仓价/浮亏/需要资金 待订阅行情后显示");
        tierSummary->setText(line1 + "\n" + line2);
    };
    connect(budgetEdit,   &QLineEdit::textChanged, &dlg, refreshTier);
    connect(maxEntEdit,   &QLineEdit::textChanged, &dlg, refreshTier);
    connect(intervalEdit, &QLineEdit::textChanged, &dlg, refreshTier);
    connect(trailEntEdit, &QLineEdit::textChanged, &dlg, refreshTier);
    connect(levEdit,      &QLineEdit::textChanged, &dlg, refreshTier);
    connect(stratBox, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, refreshTier);
    connect(dirBox,   QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, refreshTier);
    refreshTier();

    if (longBot && shortBot) {
        auto* warnLbl = new QLabel("该品种多/空两个方向都在运行中，保存会同时更新两边的参数");
        warnLbl->setWordWrap(true);
        warnLbl->setStyleSheet("color:#d29922;font-size:11px;");
        dv->addWidget(warnLbl);
    }

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    dv->addWidget(btnBox);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    if (!engine_) {
        log("请先点击【连接】", "WARN");
        return;
    }

    auto to_d = [](QLineEdit* e, double def) {
        bool ok; double v = e->text().toDouble(&ok); return ok ? v : def;
    };
    auto to_i = [](QLineEdit* e, int def) {
        bool ok; int v = e->text().toInt(&ok); return ok ? v : def;
    };

    CcgConfig cfg;
    cfg.symbol        = symbol;
    cfg.strat_type    = static_cast<CcgConfig::StratType>(stratBox->currentIndex());
    cfg.direction     = static_cast<CcgConfig::Direction>(dirBox->currentIndex());
    cfg.budget_usdt   = to_d(budgetEdit,   3000.0);
    cfg.leverage      = to_i(levEdit,       3);
    cfg.max_entries   = to_i(maxEntEdit,    6);
    cfg.interval_pct  = to_d(intervalEdit,  8.0);
    cfg.trail_entry   = to_d(trailEntEdit,  1.0);
    cfg.tp_pct        = to_d(tpEdit,        5.0);
    cfg.trail_tp      = to_d(trailTpEdit,   2.0);
    cfg.auto_restart  = autoRestartBox->isChecked();
    cfg.cooldown_secs = to_i(cooldownEdit,  60);
    cfg.stop_loss_pct = to_d(stopLossEdit,  0.0);

    cfg.entry_mode     = (entryModeBox->currentIndex() == 1)
                        ? CcgConfig::EntryMode::Indicator : CcgConfig::EntryMode::Immediate;
    cfg.kline_interval = klineBox->currentText().toStdString();
    cfg.boll_period    = to_i(bollPeriodEdit, 20);
    cfg.boll_mult      = to_d(bollMultEdit,   2.0);
    cfg.use_rsi_filter = rsiFilterBox->isChecked();
    cfg.rsi_period     = to_i(rsiPeriodEdit,  14);
    cfg.rsi_threshold  = to_d(rsiThEdit,      30.0);
    cfg.rsi_confirm_mode = (rsiModeBox->currentIndex() == 1)
                          ? CcgConfig::RsiConfirmMode::CrossFromOversold
                          : CcgConfig::RsiConfirmMode::Snapshot;
    cfg.rsi_oversold_th  = to_d(rsiOversoldEdit, 25.0);
    cfg.dynamic_band_mode = dynBandBox->isChecked();
    cfg.min_profit_floor  = to_d(floorEdit, 0.3);
    cfg.use_trend_filter  = trendBox->isChecked();
    cfg.sr_radar          = srBox->isChecked();

    QString symQ = QString::fromStdString(symbol);
    auto apply_one = [&](CcgConfig::Direction dir, const CcgBot* existing) {
        CcgConfig c = cfg;
        c.direction = dir;
        QString dirName = (dir == CcgConfig::Direction::Short) ? "空" : "多";
        if (existing) {
            engine_->update_bot_cfg(existing->bot_id, c);
            log(QString("%1 %2 策略已更新").arg(symQ).arg(dirName), "OK");
            if (!existing->entries.empty() && c.leverage != existing->cfg.leverage) {
                log("提示：该方向已有持仓，杠杆要等本轮仓位完全平掉、重新开首仓时才会真正下发给交易所",
                    "WARN");
            }
        } else {
            auto id = engine_->add_bot(c);
            if (!id.empty()) {
                engine_->stop_bot(id);   // 保存配置只写参数，不自动开始监控/建仓
                if (ticker_) ticker_->subscribe(symbol);
                log(QString("%1 %2 Bot 已添加（已停止），点击【继续】开始监控 [%3]")
                    .arg(symQ).arg(dirName).arg(QString::fromStdString(id)), "OK");
            }
        }
    };

    if (longBot && shortBot) {
        apply_one(CcgConfig::Direction::Long,  longBot);
        apply_one(CcgConfig::Direction::Short, shortBot);
    } else if (cfg.direction == CcgConfig::Direction::Both) {
        apply_one(CcgConfig::Direction::Long,  longBot);
        apply_one(CcgConfig::Direction::Short, shortBot);
    } else {
        apply_one(cfg.direction, (cfg.direction == CcgConfig::Direction::Short) ? shortBot : longBot);
    }

    refreshBotTable();
    save_bots();
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onStopAll() {
    if (!engine_) return;
    engine_->stop_all();
    tick_timer_->stop();
    log("所有Bot已停止，Tick定时器已关闭", "WARN");
    refreshBotTable();
    save_bots();
}

void MainWindow::onClearStopped() {
    if (!engine_) return;
    int n = 0;
    for (const auto& b : engine_->get_bots())
        if (b.state == CcgBot::State::Stopped) { engine_->remove_bot(b.bot_id); ++n; }
    if (n > 0) log(QString("已清除 %1 个已停止Bot").arg(n));
    refreshBotTable();
    save_bots();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick（3s 引擎驱动）
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onTick() {
    if (!engine_ || !client_) return;

    refreshPositions();
    refreshAccount();

    auto bots = engine_->get_bots();

    // 指标拉取（公开接口，不占用签名限流）：
    //  - 指标信号首单：等待 BOLL/RSI 信号的 bot（运行中+还没开首仓+指标模式）
    //  - 动态W模式：持仓中也要持续拉取——补仓锚定下轨/止盈锚定上轨都依赖实时轨道
    std::vector<CcgBot> ind_wait;
    for (const auto& b : bots)
        if (b.state == CcgBot::State::Running &&
            ((b.entries.empty() && b.cfg.entry_mode == CcgConfig::EntryMode::Indicator) ||
             b.cfg.dynamic_band_mode))
            ind_wait.push_back(b);
    if (!ind_wait.empty() && client_) {
        run_async([this, ind_wait]() {
            for (const auto& b : ind_wait) {
                auto snap = client_->fetch_indicators(b.cfg.symbol, b.cfg.kline_interval,
                                                       b.cfg.boll_period, b.cfg.boll_mult,
                                                       b.cfg.rsi_period);
                if (!snap.ok) continue;
                QMetaObject::invokeMethod(this, [this, bid = b.bot_id, snap]() {
                    if (engine_) engine_->update_indicator(bid, snap.boll_lb, snap.boll_ub, snap.rsi);
                }, Qt::QueuedConnection);
            }
        });
    }

    // SR雷达：区域每 300 tick（约15分钟）重算一次（首tick立刻算），触区检查每tick做（本地、零开销）
    if (srTickCount_++ % 300 == 0) refreshSrZones();
    checkSrTouches();

    // 趋势状态机：4h 级别数据变化慢，每 100 个 tick（约5分钟）拉一次就够；
    // 首个 tick 立刻拉一次，避免刚启动的半小时里趋势过滤空转
    static int s_trend_tick = 0;
    if (s_trend_tick++ % 100 == 0) {
        std::vector<CcgBot> trend_bots;
        for (const auto& b : bots)
            if (b.state != CcgBot::State::Stopped && b.cfg.use_trend_filter)
                trend_bots.push_back(b);
        if (!trend_bots.empty()) {
            run_async([this, trend_bots]() {
                for (const auto& b : trend_bots) {
                    auto t = client_->fetch_trend(b.cfg.symbol, b.cfg.trend_interval,
                                                   b.cfg.trend_ema_period);
                    if (!t.ok) continue;
                    QMetaObject::invokeMethod(this, [this, bid = b.bot_id, bearish = t.bearish]() {
                        if (engine_) engine_->update_trend(bid, bearish);
                    }, Qt::QueuedConnection);
                }
            });
        }
    }

    std::set<std::string> syms;
    for (const auto& b : bots)
        if (b.state != CcgBot::State::Stopped)
            syms.insert(b.cfg.symbol);
    if (syms.empty()) { refreshBotTable(); return; }

    std::set<std::string> need_rest;
    for (const auto& sym : syms) {
        double ws_price = ticker_ ? ticker_->mid_price(sym) : 0.0;
        if (ws_price > 0) {
            engine_->tick(sym, ws_price);
        } else {
            need_rest.insert(sym);
        }
    }

    if (need_rest.empty()) { refreshBotTable(); return; }

    run_async([this, need_rest = std::move(need_rest)]() {
        for (const auto& sym : need_rest) {
            double price = client_->fetch_mark_price(sym);
            if (price > 0) engine_->tick(sym, price);
        }
        QMetaObject::invokeMethod(this, [this]() { refreshBotTable(); },
                                  Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 100ms 高频刷新：只更新"最新成交价"与"延迟"两列的文本，不touch行/按钮
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshLiveQuotes() {
    if (!botTable_ || !engine_ || !ticker_) return;

    auto bots = engine_->get_bots();
    if ((int)bots.size() != botTable_->rowCount()) return;  // 行数变化时交给下次完整刷新

    auto mkc = [](const QString& s, const QColor& c, int align = Qt::AlignCenter) {
        auto* it = new QTableWidgetItem(s);
        it->setTextAlignment(align);
        it->setForeground(c);
        return it;
    };
    auto fmt_tick = [](double p, double tick) -> QString {
        if (p <= 0) return "--";
        int dp = 2;
        if (tick > 0 && tick < 1.0) {
            double t = tick; dp = 0;
            while (t < 1.0 - 1e-9 && dp < 8) { t *= 10; ++dp; }
        }
        return QString::number(p, 'f', dp);
    };

    int64_t now_ms = BookTickerStream::now_ms();

    for (int i = 0; i < (int)bots.size(); ++i) {
        const auto& b = bots[i];
        auto tick = ticker_->get(b.cfg.symbol);
        double tick_size = 0;
        if (client_) {
            TradingClient::SymbolInfo info;
            if (client_->try_get_symbol_info(b.cfg.symbol, info) && info.valid) tick_size = info.tick_size;
            else ensureSymbolInfoAsync(b.cfg.symbol);
        }
        QString lastPx = (tick.last_price > 0) ? fmt_tick(tick.last_price, tick_size) : "--";
        botTable_->setItem(i, 6, mkc(lastPx, QColor("#e6edf3")));

        int64_t latency = (tick.last_price > 0 || tick.valid) ? (now_ms - tick.recv_ms) : -1;
        QColor lat_c = (latency < 0)   ? QColor("#484f58")
                     : (latency < 100) ? QColor("#3fb950")
                     : (latency < 500) ? QColor("#d29922")
                                       : QColor("#f85149");
        botTable_->setItem(i, 7, mkc(latency >= 0 ? QString("%1ms").arg(latency) : "--", lat_c));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 品种精度信息异步预取——GUI 线程只读缓存，缺失时丢到后台线程取，绝不同步等待
// ─────────────────────────────────────────────────────────────────────────────
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
    if (!client_) return;
    run_async([this]() {
        auto positions = client_->fetch_positions();
        QMetaObject::invokeMethod(this, [this, positions = std::move(positions)]() {
            pos_cache_.clear();
            for (const auto& p : positions) {
                pos_cache_[p.symbol + (p.direction > 0 ? "_L" : "_S")] = p;
            }
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Bot 表格刷新
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshBotTable() {
    if (!engine_) { botTable_->setRowCount(0); return; }

    auto bots = engine_->get_bots();
    botTable_->setRowCount((int)bots.size());

    int running = 0, cooling = 0, stopped = 0;
    double total_unreal = 0, total_real = 0;

    auto mkc = [](const QString& s, const QColor& c, int align = Qt::AlignCenter) {
        auto* it = new QTableWidgetItem(s);
        it->setTextAlignment(align);
        it->setForeground(c);
        return it;
    };

    auto fmt_price = [](double p) -> QString {
        if (p <= 0) return "--";
        return p >= 100  ? QString("$%1").arg(p, 0, 'f', 2)
             : p >= 0.01 ? QString("$%1").arg(p, 0, 'f', 4)
                         : QString("$%1").arg(p, 0, 'f', 6);
    };

    auto fmt_tick = [](double p, double tick) -> QString {
        if (p <= 0) return "--";
        int dp = 2;
        if (tick > 0 && tick < 1.0) {
            double t = tick; dp = 0;
            while (t < 1.0 - 1e-9 && dp < 8) { t *= 10; ++dp; }
        }
        return QString::number(p, 'f', dp);
    };

    int64_t now_ms = BookTickerStream::now_ms();

    for (int i = 0; i < (int)bots.size(); ++i) {
        const auto& b = bots[i];

        // 优先用交易所真实持仓（entry_price/unrealized_pnl/notional）展示，保证跟 App 里的数字
        // 完全一致；本地 entries/avg_price 只用于策略自身的加仓层级判断，不再是展示的准头
        std::string pkey = b.cfg.symbol +
            (b.cfg.direction == CcgConfig::Direction::Short ? "_S" : "_L");
        auto pit = pos_cache_.find(pkey);
        bool has_real_pos = (pit != pos_cache_.end()) && (pit->second.qty > 0);

        double disp_avg = has_real_pos ? pit->second.entry_price : b.avg_price;

        double unreal = 0;
        if (has_real_pos) {
            unreal = pit->second.unrealized_pnl;
        } else if (!b.entries.empty() && b.avg_price > 0 && b.current_price > 0) {
            int dir = (b.cfg.direction == CcgConfig::Direction::Short) ? -1 : 1;
            unreal = (b.current_price - b.avg_price) * b.total_qty * dir;
        }
        total_unreal += unreal;
        total_real   += b.realized_pnl;

        QString state_s; QColor state_c;
        switch (b.state) {
        case CcgBot::State::Running:
            if (b.entries.empty()) {
                if (b.cfg.entry_mode == CcgConfig::EntryMode::Indicator) {
                    state_s = b.ind_ok ? "等待信号" : "等待信号(取数中)";
                    state_c = QColor("#a371f7");
                } else {
                    state_s = "等待首仓"; state_c = QColor("#58a6ff");
                }
            } else {
                state_s = b.cfg.dynamic_band_mode ? "运行中·动态W" : "运行中";
                state_c = QColor("#3fb950");
            }
            ++running; break;
        case CcgBot::State::Cooldown: {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                b.cooldown_until - std::chrono::system_clock::now()).count();
            state_s = QString("冷却%1s").arg(std::max(0LL, secs));
            state_c = QColor("#d29922"); ++cooling; break;
        }
        case CcgBot::State::Stopped:
            state_s = "已停止"; state_c = QColor("#8b949e"); ++stopped; break;
        }

        QColor dir_c = (b.cfg.direction == CcgConfig::Direction::Long)  ? QColor("#3fb950")
                     : (b.cfg.direction == CcgConfig::Direction::Short) ? QColor("#f85149")
                                                                         : QColor("#58a6ff");

        QString layers  = QString("%1/%2").arg((int)b.entries.size()).arg(b.cfg.max_entries);
        QString unr_s   = (!has_real_pos && b.entries.empty()) ? "--"
            : QString("%1$%2").arg(unreal >= 0 ? "+" : "").arg(std::abs(unreal), 0, 'f', 2);
        QString rea_s   = QString("%1$%2")
            .arg(b.realized_pnl >= 0 ? "+" : "")
            .arg(std::abs(b.realized_pnl), 0, 'f', 2);

        botTable_->setItem(i, 0,  mkc(QString::number(i+1),       QColor("#484f58")));
        botTable_->setItem(i, 1,  mkc(QString::fromStdString(b.cfg.symbol),
                                       QColor("#e6edf3"), Qt::AlignLeft | Qt::AlignVCenter));
        botTable_->setItem(i, 2,  mkc(QString::fromStdString(CcgEngine::dir_name(b.cfg.direction)), dir_c));
        botTable_->setItem(i, 3,  mkc(QString::fromStdString(CcgEngine::strat_name(b.cfg.strat_type)),
                                       QColor("#8b949e")));
        botTable_->setItem(i, 4,  mkc(layers,                     QColor("#58a6ff")));
        botTable_->setItem(i, 5,  mkc(fmt_price(disp_avg),        QColor("#8b949e")));

        // 最新成交价 + 延迟：来自 WebSocket aggTrade 流，独立于策略引擎的 tick 价格
        auto tick = ticker_ ? ticker_->get(b.cfg.symbol) : BookTickerStream::Tick{};
        double tick_size = 0;
        if (client_) {
            TradingClient::SymbolInfo info;
            if (client_->try_get_symbol_info(b.cfg.symbol, info) && info.valid) tick_size = info.tick_size;
            else ensureSymbolInfoAsync(b.cfg.symbol);
        }
        QString lastPx = (tick.last_price > 0) ? fmt_tick(tick.last_price, tick_size) : "--";
        botTable_->setItem(i, 6,  mkc(lastPx, QColor("#e6edf3")));

        int64_t latency = (tick.last_price > 0 || tick.valid) ? (now_ms - tick.recv_ms) : -1;
        QColor lat_c = (latency < 0)   ? QColor("#484f58")
                     : (latency < 100) ? QColor("#3fb950")
                     : (latency < 500) ? QColor("#d29922")
                                       : QColor("#f85149");
        botTable_->setItem(i, 7,  mkc(latency >= 0 ? QString("%1ms").arg(latency) : "--", lat_c));

        botTable_->setItem(i, 8,  mkc(unr_s, unreal >= 0       ? QColor("#3fb950") : QColor("#f85149")));

        // 保证金 / 收益率：优先用交易所真实名义价值/杠杆算，没有真实持仓时退回本地估算
        double margin = has_real_pos && pit->second.leverage > 0
            ? pit->second.notional / pit->second.leverage
            : (b.cfg.leverage > 0 ? b.total_cost / b.cfg.leverage : 0);
        QString margin_s = (margin > 0) ? fmt_price(margin) : "--";
        botTable_->setItem(i, 9,  mkc(margin_s, QColor("#8b949e")));

        double roi = (margin > 0) ? unreal / margin * 100.0 : 0;
        QString roi_s = (margin > 0)
            ? QString("%1%2%").arg(roi >= 0 ? "+" : "").arg(roi, 0, 'f', 1)
            : "--";
        botTable_->setItem(i, 10, mkc(roi_s, roi >= 0 ? QColor("#3fb950") : QColor("#f85149")));

        // 强平价：来自交易所真实持仓（refreshPositions() 每 3s 拉取一次），本地无法准确估算
        double liq = has_real_pos ? pit->second.liq_price : 0;
        botTable_->setItem(i, 11, mkc(liq > 0 ? fmt_price(liq) : "--", QColor("#d29922")));

        botTable_->setItem(i, 12, mkc(rea_s, b.realized_pnl >= 0 ? QColor("#3fb950") : QColor("#f85149")));
        botTable_->setItem(i, 13, mkc(state_s, state_c));

        // 操作列
        std::string bid  = b.bot_id;
        bool        is_stopped = (b.state == CcgBot::State::Stopped);

        auto* opW = new QWidget();
        auto* opL = new QHBoxLayout(opW);
        opL->setContentsMargins(3, 1, 3, 1);
        opL->setSpacing(4);

        // 停止=暂停监控（保留当前持仓跟踪，不平仓）；继续=原地恢复监控，不会重新触发首仓
        auto* btnSR = new QPushButton(is_stopped ? "继续" : "停止");
        btnSR->setFixedHeight(20);
        btnSR->setStyleSheet(is_stopped
            ? "QPushButton{background:#1a3d1a;color:#3fb950;font-size:11px;padding:0 6px;}"
            : "QPushButton{background:#3d1a1a;color:#f85149;font-size:11px;padding:0 6px;}");
        connect(btnSR, &QPushButton::clicked, [this, bid, is_stopped]() {
            if (!engine_) return;
            if (is_stopped) engine_->resume_bot(bid);
            else             engine_->stop_bot(bid);
            refreshBotTable();
            save_bots();
        });
        opL->addWidget(btnSR);

        auto* btnClose = new QPushButton("平仓");
        btnClose->setFixedHeight(20);
        btnClose->setEnabled(!b.entries.empty());
        btnClose->setStyleSheet(
            "QPushButton{background:#3d2d0a;color:#d29922;font-size:11px;padding:0 6px;}"
            "QPushButton:disabled{background:#21262d;color:#484f58;}");
        connect(btnClose, &QPushButton::clicked, [this, bid]() {
            if (!engine_) return;
            engine_->close_bot(bid);   // 异步市价平仓，完成后由 log 回调刷新表格
            log("已发送平仓请求", "WARN");
        });
        opL->addWidget(btnClose);

        auto* btnDel = new QPushButton("删除");
        btnDel->setFixedHeight(20);
        btnDel->setStyleSheet(
            "QPushButton{background:#2d333b;color:#8b949e;font-size:11px;padding:0 6px;}");
        connect(btnDel, &QPushButton::clicked, [this, bid]() {
            if (!engine_) return;
            engine_->remove_bot(bid);
            refreshBotTable();
            save_bots();
        });
        opL->addWidget(btnDel);
        opL->addStretch();

        botTable_->setCellWidget(i, 14, opW);
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
}

} // namespace ccg
