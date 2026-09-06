#include "gui/main_window.h"
#include "version.h"
#include "core/key_store.h"
#include "net/alert.h"
#include "core/decision.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QSplitter>
#include <QMessageBox>
#include <QScrollArea>
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
    , pool_(std::make_shared<ThreadPool>(2))        // 引擎专用：下单/平仓，绝不排队
    , fetchPool_(std::make_shared<ThreadPool>(4))   // 数据拉取专用：慢任务全在这
{
    setWindowTitle(QString("CCG 合约监控  %1").arg(ccbot::kVersion));
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
    //   a) use-after-free：任务的 lambda 捕获的是 this 和 engine_ 的裸指针，而
    //      CcgEngine 里 pool_ 的声明位置在 mtx_/bots_ 之前 —— 意味着线程池 join
    //      的时候那两个成员已经析构，在途任务一访问就是 UAF。等排空之后再让
    //      成员开始销毁，这条路径就不存在了。
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
    save_bots();

    // ④ 没排空就【不要】走正常析构路径。
    //
    // 在途任务捏着 engine_ 的裸指针，而成员逆序析构会先销毁 bots_/mtx_——
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
        QFile marker(dst + "/.migrated");
        marker.open(QIODevice::WriteOnly);
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
        o["use_disaster_stop"] = c.use_disaster_stop;
        o["disaster_stop_pct"] = c.disaster_stop_pct;

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
        o["dyn_fixed_interval"] = c.dyn_fixed_interval;
        o["mtf_ladder"]        = c.mtf_ladder;
        o["mtf_tier_layers"]   = QString::fromStdString(c.mtf_tier_layers);
        o["mtf_k"]             = c.mtf_k;
        o["mtf_min_gap_pct"]   = c.mtf_min_gap_pct;
        o["use_trend_filter"]  = c.use_trend_filter;
        o["trend_interval"]    = QString::fromStdString(c.trend_interval);
        o["trend_ema_period"]  = c.trend_ema_period;
        o["sr_radar"]          = c.sr_radar;
        o["sr_interval"]       = QString::fromStdString(c.sr_interval);
        o["use_htf_filter"]      = c.use_htf_filter;
        o["htf_interval"]        = QString::fromStdString(c.htf_interval);
        o["htf_pos_max"]         = c.htf_pos_max;
        o["htf_day_chg_max"]     = c.htf_day_chg_max;
        o["htf_week_chg_max"]    = c.htf_week_chg_max;
        o["use_sr_support"]      = c.use_sr_support;
        o["use_sr_headroom"]     = c.use_sr_headroom;
        o["sr_min_confluence"]   = c.sr_min_confluence;
        o["sr_independent_conf"] = c.sr_independent_conf;
        o["sr_lower_half_only"]  = c.sr_lower_half_only;
        o["sr_headroom_ratio"]   = c.sr_headroom_ratio;
        o["use_sr_exit"]         = c.use_sr_exit;
        o["use_structural_stop"] = c.use_structural_stop;

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
        o["ind_dipped"]        = b.ind_dipped;
        o["tp_extreme"]        = b.tp_extreme;
        o["realized_pnl"]      = b.realized_pnl;
        o["cycle_count"]       = b.cycle_count;
        // 满层健康度：随 bot 落盘，重启后继续累加（口径=自创建以来）
        o["full_layer_secs"]   = (double)b.full_layer_secs;
        o["alive_secs"]        = (double)b.alive_secs;
        o["cooldown_until_ms"] = tp_to_ms(b.cooldown_until);
        // 交易所侧灾难止损单号：重启后据此撤掉旧单再按当前均价重挂
        o["disaster_stop_id"]    = QString::fromStdString(b.disaster_stop_id);
        o["disaster_stop_price"] = b.disaster_stop_price;

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
        c.strat_type   = (CcgConfig::StratType)o["strat_type"].toInt(7);
        c.direction    = (CcgConfig::Direction)o["direction"].toInt(0);
        c.budget_usdt  = o["budget_usdt"].toDouble(3000.0);
        c.leverage     = o["leverage"].toInt(3);
        c.max_entries  = o["max_entries"].toInt(7);
        c.interval_pct = o["interval_pct"].toDouble(8.0);
        c.trail_entry  = o["trail_entry"].toDouble(1.0);
        c.tp_pct       = o["tp_pct"].toDouble(5.0);
        c.trail_tp     = o["trail_tp"].toDouble(2.0);
        c.auto_restart = o["auto_restart"].toBool(true);
        c.cooldown_secs= o["cooldown_secs"].toInt(300);
        c.stop_loss_pct= o["stop_loss_pct"].toDouble(0.0);
        c.use_disaster_stop = o["use_disaster_stop"].toBool(false);
        c.disaster_stop_pct = o["disaster_stop_pct"].toDouble(30.0);

        c.entry_mode     = (CcgConfig::EntryMode)o["entry_mode"].toInt(1);
        c.kline_interval = o["kline_interval"].toString("1h").toStdString();
        c.boll_period    = o["boll_period"].toInt(20);
        c.boll_mult      = o["boll_mult"].toDouble(2.0);
        c.use_rsi_filter = o["use_rsi_filter"].toBool(true);
        c.rsi_period     = o["rsi_period"].toInt(14);
        c.rsi_threshold  = o["rsi_threshold"].toDouble(30.0);
        c.rsi_confirm_mode = (CcgConfig::RsiConfirmMode)o["rsi_confirm_mode"].toInt(1);
        c.rsi_oversold_th  = o["rsi_oversold_th"].toDouble(25.0);
        c.dynamic_band_mode = o["dynamic_band_mode"].toBool(true);
        c.min_profit_floor  = o["min_profit_floor"].toDouble(3.5);
        // 兜底 0（自适应）而不是建议值 6：老版本存的配置里没有这个字段，
        // 兜底成 6 等于在用户不知情时把正在跑的策略换掉（间距 0.9%→6%）
        c.dyn_fixed_interval = o["dyn_fixed_interval"].toDouble(0.0);
        c.mtf_ladder        = o["mtf_ladder"].toBool(false);
        c.mtf_tier_layers   = o["mtf_tier_layers"].toString().toStdString();
        c.mtf_k             = o["mtf_k"].toDouble(0.5);
        c.mtf_min_gap_pct   = o["mtf_min_gap_pct"].toDouble(2.0);
        c.use_trend_filter  = o["use_trend_filter"].toBool(true);
        c.trend_interval    = o["trend_interval"].toString("4h").toStdString();
        c.trend_ema_period  = o["trend_ema_period"].toInt(200);
        c.sr_radar          = o["sr_radar"].toBool(true);
        c.sr_interval       = o["sr_interval"].toString("4h").toStdString();
        c.use_htf_filter      = o["use_htf_filter"].toBool(true);
        c.htf_interval        = o["htf_interval"].toString("1d").toStdString();
        c.htf_pos_max         = o["htf_pos_max"].toDouble(0.60);
        // 兜底 0 而非某个"建议值"：老 bots.json 里没有这两个键，兜成非零
        // 等于在用户不知情时给正在跑的策略加了两道闸门（同 v4.0.6 固定间隔的处理）
        c.htf_day_chg_max     = o["htf_day_chg_max"].toDouble(0.0);
        c.htf_week_chg_max    = o["htf_week_chg_max"].toDouble(0.0);
        // v3.8 迁移：老配置只有 smart_gates 总开关 + use_sr_gate。
        // 总开关为 false 时三层完全不参与，升级后必须保持这个行为——否则
        // 老 bot 会突然开始拦截
        const bool legacy_smart = o["smart_gates"].toBool(true);
        const bool legacy_sr    = o["use_sr_gate"].toBool(true);
        if (o.contains("use_sr_support")) {
            c.use_sr_support  = o["use_sr_support"].toBool(true);
            c.use_sr_headroom = o["use_sr_headroom"].toBool(true);
        } else {
            c.use_sr_support  = legacy_smart && legacy_sr;
            c.use_sr_headroom = legacy_smart && legacy_sr;
        }
        if (!o.contains("use_sr_support") && !legacy_smart) c.use_htf_filter = false;
        c.sr_min_confluence   = o["sr_min_confluence"].toInt(2);
        c.sr_independent_conf = o["sr_independent_conf"].toBool(true);
        c.sr_lower_half_only  = o["sr_lower_half_only"].toBool(false);
        c.sr_headroom_ratio   = o["sr_headroom_ratio"].toDouble(3.0);
        c.use_sr_exit         = o["use_sr_exit"].toBool(false);
        c.use_structural_stop = o["use_structural_stop"].toBool(false);
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
        bot.ind_dipped        = o["ind_dipped"].toBool(false);
        bot.tp_extreme        = o["tp_extreme"].toDouble();
        bot.realized_pnl      = o["realized_pnl"].toDouble();
        bot.cycle_count       = o["cycle_count"].toInt();
        bot.full_layer_secs   = (int64_t)o["full_layer_secs"].toDouble();
        bot.alive_secs        = (int64_t)o["alive_secs"].toDouble();
        bot.cooldown_until    = ms_to_tp((qint64)o["cooldown_until_ms"].toDouble());
        bot.disaster_stop_id    = o["disaster_stop_id"].toString().toStdString();
        bot.disaster_stop_price = o["disaster_stop_price"].toDouble(0.0);
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

    apiKey_      = apiKeyEdit->text().trimmed();
    apiSecret_   = secretEdit->text().trimmed();
    accountMode_ = (modeCombo->currentIndex() == 1) ? 1 : 0;
    testnet_     = (accountMode_ == 1) ? false : testnetBox->isChecked();
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

        // Bot 表格 — 15 列。
        // 资金费不进这张表：它是【账户级慢变量】（8小时才结算一次），而这张表是
        // 每3秒刷新的【逐品种实时行】。混在一起既挤掉实时数据的宽度，也不符合它
        // 的性质——账户合计放顶部栏，逐品种细节放"均价"列的悬停提示
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
        hdr->resizeSection(13, 96);
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
                    sendAlert(QString("[CCGMonitor] 账户连接失败: %1")
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
            connNetName_        = cfg.testnet ? "测试网"
                                  : (cfg.account_mode == TradingClient::AccountMode::PortfolioMargin
                                     ? "统一账户" : "主网");
            alertedDisconnect_  = false;
            setConnState(ConnState::Connected);   // 顶部计时从此刻开始，文本由 updateHeader() 接管

            // 启动盘口 WebSocket
            ticker_ = std::make_unique<BookTickerStream>(cfg.testnet);
            ticker_->start();

            srTickCount_    = 0;   // 保证"首tick立即拉取SR/趋势"在（罕见的）重连后依然成立
            trendTickCount_ = 0;
            tick_timer_->start();

            log(QString("连接成功 | %1 | 权益 $%2 | 可用 $%3%4")
                .arg(connNetName_)
                .arg(info.total_equity, 0, 'f', 2)
                .arg(info.available,    0, 'f', 2)
                .arg(info.uni_mmr > 0
                     ? QString(" | uniMMR %1").arg(info.uni_mmr, 0, 'f', 2)
                     : QString()), "OK");

            // 恢复上次保存的 Bot
            load_and_restore_bots();
            funding_.load(funding_path());   // 资金费账本（品种级，与 bot 生命周期无关）

            // 启动对账：本地跟踪的仓位 vs 交易所实际持仓。外部手动平过仓/强平过的话，
            // 本地状态是错的，带着错误均价继续跑会把止盈止损全算错
            run_async([this]() {
                if (!client_ || !engine_) return;
                auto ex_pos = client_->fetch_positions();
                QMetaObject::invokeMethod(this, [this, ex_pos]() {
                    if (!engine_) return;
                    std::vector<CcgEngine::ExchangePos> ex;
                    for (const auto& p : ex_pos) ex.push_back({p.symbol, p.direction, p.qty, p.entry_price});
                    auto issues = engine_->reconcile_positions(ex);
                    if (!issues.empty()) {
                        save_bots();   // 收敛后的状态立刻落盘
                        refreshBotTable();
                        QString msg = QString("[CCGMonitor] 启动对账发现 %1 处不一致，详见日志").arg(issues.size());
                        sendAlert(msg);
                    }
                    // 对账之后再重建交易所侧灾难止损单：必须等本地持仓收敛到真相，
                    // 否则会照着一个错误的均价挂止损
                    engine_->resync_disaster_stops();
                }, Qt::QueuedConnection);
            });
        }, Qt::QueuedConnection);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 多周期梯子：补齐 4h / 12h 两档的布林带。
// 1h 和 1d 两档由指标拉取和宏观%B拉取顺带喂了（它们本来就在拉那两个周期的带），
// 所以这里每个 bot 只多 2 个公开接口请求——限流治理还没做，能省则省。
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshMtfBands() {
    if (!client_ || !engine_) return;
    if (mtfFetchBusy_.exchange(true)) return;

    struct Need { std::string bot_id, symbol; int tier; std::string interval; int period; double mult; };
    std::vector<Need> needs;
    for (const auto& b : engine_->get_bots()) {
        if (!b.cfg.mtf_ladder || b.state == CcgBot::State::Stopped) continue;
        needs.push_back({b.bot_id, b.cfg.symbol, 1, "4h",  b.cfg.boll_period, b.cfg.boll_mult});
        needs.push_back({b.bot_id, b.cfg.symbol, 2, "12h", b.cfg.boll_period, b.cfg.boll_mult});
        // 指标/宏观用的不是 1h / 1d 时，那两档也得自己拉
        if (b.cfg.kline_interval != "1h")
            needs.push_back({b.bot_id, b.cfg.symbol, 0, "1h", b.cfg.boll_period, b.cfg.boll_mult});
        if (b.cfg.htf_interval != "1d")
            needs.push_back({b.bot_id, b.cfg.symbol, 3, "1d", b.cfg.boll_period, b.cfg.boll_mult});
    }
    if (needs.empty()) { mtfFetchBusy_.store(false); return; }

    run_async([this, needs]() {
        for (const auto& n : needs) {
            auto snap = client_->fetch_indicators(n.symbol, n.interval, n.period, n.mult, 14);
            if (!snap.ok) continue;
            QMetaObject::invokeMethod(this, [this, n, snap]() {
                if (engine_) engine_->update_mtf_band(n.bot_id, n.tier, snap.boll_lb, snap.boll_ub);
            }, Qt::QueuedConnection);
        }
        mtfFetchBusy_.store(false);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 资金费账本刷新。
// 这是【账本】不是风控：只记录和展示持有成本，不参与任何交易决策。
// 永续合约每 8 小时结算一次资金费，这笔钱是真实划走的现金——价格涨回来也拿不回，
// 所以它和"浮亏"性质完全不同，必须单独看得见。
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshFunding() {
    if (!client_ || !engine_) return;
    if (fundFetchBusy_.exchange(true)) return;   // 上一批没跑完就跳过

    // 收集所有品种，以及"最早的建仓时间"用作首次补历史的起点
    std::vector<std::string> syms;
    int64_t earliest = 0;
    std::set<std::string> seen;
    for (const auto& b : engine_->get_bots()) {
        if (seen.insert(b.cfg.symbol).second) syms.push_back(b.cfg.symbol);
        if (!b.entries.empty()) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          b.entries.front().time.time_since_epoch()).count();
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
// SR雷达（影子模式）：自动支撑/阻力区检测 + 触区告警 + 展示，不参与下单
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshSrZones() {
    if (!client_ || !engine_) return;
    // 收集开了 SR 雷达的品种（同品种多bot取任一配置的周期）
    std::map<std::string, std::string> radar;   // symbol → interval
    for (const auto& b : engine_->get_bots())
        if (b.cfg.sr_radar && b.state != CcgBot::State::Stopped)
            radar.emplace(b.cfg.symbol, b.cfg.sr_interval);

    // 清理已关闭雷达/已删除的品种，否则旧区域会永远用陈旧数据持续误报
    for (auto it = srStates_.begin(); it != srStates_.end();)
        it = radar.count(it->first) ? std::next(it) : srStates_.erase(it);

    if (radar.empty() || srFetchBusy_.load()) return;
    srFetchBusy_.store(true);

    run_async([this, radar]() {
        for (const auto& [sym, interval] : radar) {
            auto raw = client_->fetch_bars(sym, interval, 400);
            if (raw.size() < 50) continue;   // 新品种历史不够，跳过
            std::vector<srzones::Bar> bars;
            bars.reserve(raw.size());
            for (const auto& r : raw) bars.push_back({r.open, r.high, r.low, r.close, r.volume});
            auto zones = srzones::detect_zones(bars);
            double atr = srzones::atr(bars, 14);
            QMetaObject::invokeMethod(this, [this, sym, atr, zones = std::move(zones)]() {
                auto& st = srStates_[sym];
                st.zones       = zones;
                st.atr         = atr;
                st.computed_ms = QDateTime::currentMSecsSinceEpoch();
            }, Qt::QueuedConnection);
        }
        srFetchBusy_.store(false);
    });
}

// 触区告警已移除：SR 区域现在是三层拦截的内部数据源，不再是需要人盯的事件。
// 它每 tick 都可能触发，是运行日志里最占地方的一类，而拦截生效后"价格进了某个区域"
// 本身并不需要人做任何事——该拦的闸门已经拦了。
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
            int conf = z.confluence_independent();   // 与决策层同口径
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
    dlg.resize(860, 760);

    // 外层：滚动区 + 固定在底部的按钮。分组之后内容比一屏高，小屏笔记本上
    // 原先的固定高度会把"保存"顶出屏幕外
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

    // 分组工厂：所有分组共用同一套外观与对齐，避免出现"一半分组一半裸表单"
    // 这种两套组织方式并存的情况（改版前正是如此）
    auto mkGroup = [&](const QString& title, const QString& color) {
        auto* box = new QGroupBox(title);
        box->setStyleSheet(QString("QGroupBox{color:%1;font-size:11px;font-weight:bold;"
                                   "border:1px solid #21262d;border-radius:4px;"
                                   "margin-top:8px;padding:10px 12px 8px;}"
                                   "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;}")
                           .arg(color));
        auto* f = new QFormLayout(box);
        f->setSpacing(7);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        dv->addWidget(box);
        return f;
    };
    // 灰字说明统一样式，并且统一挂在所属分组的末尾——改版前它们散落在中间
    auto addHint = [](QFormLayout* f, const QString& text) {
        auto* h = new QLabel(text);
        h->setWordWrap(true);
        h->setStyleSheet("color:#8b949e;font-size:10px;");
        f->addRow(h);            // 跨两列，不占标签列
        return h;
    };
    // 勾选框：跨两列铺满，与输入框的字段列对齐同一条轴。
    // 改版前用 addRow("", box)，勾选框从字段列开始、左边空一大片，
    // 和右对齐的标签形成两条互不相干的对齐轴——这就是"看着不对称"的来源
    auto addCheck = [](QFormLayout* f, QCheckBox* box) { f->addRow(box); };
    // 子项缩进：改版前用全角空格撑，宽度依赖字体且会撑宽整个标签列
    auto addSub = [](QFormLayout* f, const QString& label, QWidget* w) {
        auto* row = new QWidget();
        auto* hl = new QHBoxLayout(row);
        hl->setContentsMargins(18, 0, 0, 0);
        hl->setSpacing(8);
        auto* lb = new QLabel(label);
        lb->setStyleSheet("color:#8b949e;");
        hl->addWidget(lb);
        hl->addWidget(w, 1);
        f->addRow(row);
    };

    // 五个分组一次建好，视觉顺序由这里决定——控件在代码里哪一行创建都不影响它
    // 落在哪个分组，所以下面可以按"逻辑相关"归组，而不必迁就原来的书写顺序
    auto* form      = mkGroup("基础参数", "#58a6ff");
    auto* sigForm   = mkGroup("入场信号（首单怎么开）", "#a371f7");
    auto* dcaForm   = mkGroup("补仓机制（跌了怎么加）", "#3fb950");
    auto* gateForm  = mkGroup("入场拦截（什么时候不开）", "#d29922");
    auto* riskForm  = mkGroup("风控与出场", "#f85149");

    auto* dirBox = new QComboBox();
    dirBox->addItem("多");
    dirBox->addItem("空");
    dirBox->addItem("双向");
    if (longBot && shortBot)      dirBox->setCurrentIndex(2);
    else if (prefill)             dirBox->setCurrentIndex(
        prefill->cfg.direction == CcgConfig::Direction::Short ? 1 : 0);
    if (longBot && shortBot) dirBox->setEnabled(false);  // 双向都在跑，方向不用选，两边都会更新
    form->addRow("方向:", dirBox);

    auto* stratBox = new QComboBox();
    // 顺序必须与 CcgConfig::StratType 的枚举顺序一致（下拉框按索引存取）
    for (const char* s : {"平推","倍投","倍投Plus","三倍",
                           "平方","斐波那契","卢卡斯","递增"})
        stratBox->addItem(s);
    stratBox->setCurrentIndex(prefill ? (int)prefill->cfg.strat_type : 7);   // 默认递增（实证最优）
    form->addRow("策略:", stratBox);

    // 带目标分组的版本（旧版写死往 form 里塞，所有参数因此只能待在同一张扁平表里）
    auto mkEditIn = [&](QFormLayout* f, const QString& label, double val) {
        auto* e = new QLineEdit(QString::number(val));
        f->addRow(label, e);
        return e;
    };
    auto mkEdit  = [&](const QString& label, double val) { return mkEditIn(form, label, val); };
    auto mkEditI = [&](const QString& label, int val) {
        return mkEditIn(form, label, (double)val);
    };

    auto* budgetEdit   = mkEdit ("预算USDT:",      prefill ? prefill->cfg.budget_usdt  : 3000.0);
    auto* levEdit      = mkEditI("杠杆:",          prefill ? prefill->cfg.leverage     : 3);
    auto* maxEntEdit   = mkEditI("最大层:",        prefill ? prefill->cfg.max_entries  : 7);
    auto* intervalEdit = mkEdit ("间隔%:",         prefill ? prefill->cfg.interval_pct : 8.0);
    auto* trailEntEdit = mkEdit ("追踪建仓%:",     prefill ? prefill->cfg.trail_entry  : 1.0);
    auto* tpEdit       = mkEdit ("止盈%:",         prefill ? prefill->cfg.tp_pct       : 5.0);
    auto* trailTpEdit  = mkEdit ("止盈追踪%:",     prefill ? prefill->cfg.trail_tp     : 2.0);
    auto* cooldownEdit = mkEditI("冷却(s):",       prefill ? prefill->cfg.cooldown_secs: 300);
    auto* stopLossEdit = mkEditIn(riskForm, "本地止损%(0=禁用):",
                                  prefill ? prefill->cfg.stop_loss_pct : 0.0);
    auto* disStopBox = new QCheckBox("在交易所挂灾难止损单（进程外保护）");
    disStopBox->setChecked(prefill ? prefill->cfg.use_disaster_stop : false);
    addCheck(riskForm, disStopBox);
    // 缩进表示"这是上面那个勾选框的子项"。原先用 └ 制表符，小字号下会被认成字母 L
    auto* disStopEdit  = new QLineEdit(QString::number(prefill ? prefill->cfg.disaster_stop_pct : 30.0));
    addSub(riskForm, "触发位置：均价下方%", disStopEdit);
    // 没勾选时把比例框灰掉：启用与否是策略取向，不该藏在"这个数字是不是0"里
    disStopEdit->setEnabled(disStopBox->isChecked());
    connect(disStopBox, &QCheckBox::toggled, disStopEdit, &QWidget::setEnabled);
    {
        QString t =
            "在【交易所】挂一张 STOP_MARKET 单（均价下方该比例处），程序崩溃/断电/"
            "误关窗口后它依然生效——这是唯一的进程外保护。上面那个\"止损%\"只活在本进程里。\n"
            "⚠ 它会把浮亏变成实亏。如果你的策略是「套住就长线持有、只要不归零就等」，"
            "那这个功能与你的取向冲突，保持不勾选即可（默认就是不勾）。\n"
            "⚠ 勾选的话只防瀑布，不参与常规止盈：网格天然要吃深度回撤，设太紧会在正常"
            "补仓过程中被打掉。建议留足余量（满层跌 20% 的配置设 30~35）。";
        addHint(riskForm, t);
    }

    auto* autoRestartBox = new QCheckBox("自动重启");
    autoRestartBox->setChecked(prefill ? prefill->cfg.auto_restart : true);
    addCheck(form, autoRestartBox);

    auto* entryModeBox = new QComboBox();
    entryModeBox->addItem("立即开仓（一开监控就开首仓）");
    entryModeBox->addItem("指标信号（BOLL+RSI 满足才开首仓）");
    entryModeBox->setCurrentIndex(
        prefill ? (prefill->cfg.entry_mode == CcgConfig::EntryMode::Indicator ? 1 : 0) : 1);
    sigForm->addRow("首单模式:", entryModeBox);

    // ── 动态W模式（v2.3）───────────────────────────────────────────────────────
    auto* dynBandBox = new QCheckBox("动态W模式（补仓锚定下轨/止盈锚定上轨/间距自适应带宽）");
    dynBandBox->setChecked(prefill ? prefill->cfg.dynamic_band_mode : true);
    dynBandBox->setToolTip(
        "开启后间隔%/追踪%不再用上面的固定值，改为按实时布林带宽W自动推导：\n"
        "间隔=W/3、追踪止盈=0.15W、追踪建仓=0.1W（各有上下限夹逼）。\n"
        "补仓要求价格在带外（多:≤下轨），止盈要求触及对侧轨道且盈利≥保底利润。\n"
        "带子随趋势移动时梯子跟着走，均价贴着下轨，带内震荡即可完成周期。");
    addCheck(dcaForm, dynBandBox);
    auto* floorEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.min_profit_floor : 3.5));
    addSub(dcaForm, "保底利润%（动态模式）", floorEdit);

    // 固定补仓间隔：此前只有回测命令行能设，实盘够不着——而 walk-forward 证明
    // 它全面优于 W/3 自适应推导。不通到界面等于把最好的配置锁在回测里
    auto* fixIvEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.dyn_fixed_interval : 0.0));
    fixIvEdit->setPlaceholderText("0 = 用 W/3 自适应；建议填 6");
    fixIvEdit->setToolTip(
        "只替换动态W的【间距推导】，结构锚定完全保留——\n"
        "补仓仍要求价格在下轨外，止盈仍要求触及上轨且盈利≥保底利润。\n\n"
        "⚠ 实证建议填 6。8品种 × 8个滚动窗口 = 64 个纯样本外测试段：\n"
        "  固定 6%   总净利  70620   盈利 56/64   满层中位  0.0%   ← 最优\n"
        "  固定 5%   总净利  67289   盈利 52/64   满层中位  0.7%\n"
        "  固定 4%   总净利  53826   盈利 49/64   满层中位  1.2%\n"
        "  固定 3%   总净利 −35616   盈利 38/64   满层中位 22.4%   ← 样本外净亏\n"
        "而 W/3 推导出的间隔中位仅 0.90%，满层中位高达 84.5%，\n"
        "5 个品种里 0 个能打平其最优固定间隔。\n\n"
        "满层＝弹药耗尽、失去摊薄能力，是驱动回撤与资金费的枢纽变量：\n"
        "满层<10% 的品种 22/22 盈利，>75% 的 0/6 盈利（中位亏 16854U）。\n"
        "填 6 之后实测平均只用 1.5 层、满层率 0.0%——子弹根本用不完。");
    addSub(dcaForm, "固定补仓间隔%（0=自适应，建议 6）", fixIvEdit);

    // ── 多周期梯子（v3.7 实验，默认关）────────────────────────────────────────
    auto* mtfBox = new QCheckBox("多周期梯子（补仓档位锚定 1h/4h/12h/1d 下轨，越深的层要求越极端）");
    mtfBox->setChecked(prefill ? prefill->cfg.mtf_ladder : false);
    mtfBox->setToolTip(
        "把补仓间距的来源从固定参数换成市场结构：梯子是 N 个有序槽位，\n"
        "第 i 槽必须先跌破【它所属档位】那个周期的布林下轨，才武装追踪建仓。\n"
        "带宽大致按 √T 缩放，所以 1h→4h→12h→1d 的间距天然递增，\n"
        "浅回调只消耗第一档，深层弹药留给真正的大跌。\n\n"
        "⚠ 实验功能，尚未经过完整回测验证。BTC 2021 单年的初步对照里它输给\n"
        "现行动态W（收益/回撤 0.69 vs 3.08）——原因是补仓变克制之后，拉均价\n"
        "的能力被削弱，仓位摊薄不下去。它的论点在持续阴跌里才成立，需要实盘\n"
        "或完整回测积累数据。开启前请明白这一点。\n"
        "只接管补仓间距，止盈那半（触上轨+保底利润）完全不变。");
    addCheck(dcaForm, mtfBox);

    auto* mtfTiersEdit = new QLineEdit(prefill ? QString::fromStdString(prefill->cfg.mtf_tier_layers) : "");
    mtfTiersEdit->setPlaceholderText("留空=按 3:2:2:1 权重自动分配");
    addSub(dcaForm, "各档层数 (1h,4h,12h,1d)", mtfTiersEdit);
    auto* mtfKEdit   = new QLineEdit(QString::number(prefill ? prefill->cfg.mtf_k : 0.5));
    addSub(dcaForm, "最小间距系数 k（×该档带宽）", mtfKEdit);
    auto* mtfGapEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.mtf_min_gap_pct : 2.0));
    addSub(dcaForm, "最小间距兜底%", mtfGapEdit);
    addHint(dcaForm,
        "最小间距 = max(k × 该档带宽, 兜底%)，相对上一笔成交价。前者自适应——"
        "瀑布本身是高波动事件，带子撑开时地板跟着撑开，挡住「四档同时触发、"
        "整个梯子打在崩盘顶部」；后者防止带数据异常时失去地板。");
    auto syncMtfUi = [mtfBox, mtfTiersEdit, mtfKEdit, mtfGapEdit]() {
        const bool on = mtfBox->isChecked();
        mtfTiersEdit->setEnabled(on);
        mtfKEdit->setEnabled(on);
        mtfGapEdit->setEnabled(on);
    };
    syncMtfUi();
    connect(mtfBox, &QCheckBox::toggled, &dlg, [syncMtfUi](bool) { syncMtfUi(); });

    // SR 雷达不再是独立选项：它是三层拦截/止盈锚/结构止损的【内部数据源】，
    // 由下面那几个开关自动带上（见提交时的 cfg.sr_radar 赋值）。
    // 曾经暴露成勾选框，是影子模式时期"先验证眼睛准不准"的遗留——那个阶段已经过去，
    // 而留着它只会让人把结构层的数据源关掉、把闸门变成永久 fail-open。

    // ── 趋势过滤（v2.5）──────────────────────────────────────────────────────
    auto* trendBox = new QCheckBox("趋势过滤（4h EMA200+中轨斜率：空头态暂停新首仓、补仓间隔×1.5）");
    trendBox->setChecked(prefill ? prefill->cfg.use_trend_filter : true);
    trendBox->setToolTip(
        "高周期趋势判定：价格在 4h EMA200 之下 且 中轨明显下拐 = 空头态。\n"
        "空头态期间不开新首仓（不接单边下跌的飞刀），已有仓位补仓间隔放大1.5倍。\n"
        "趋势数据每5分钟刷新一次；数据缺失时过滤自动失效，不会卡死交易。");
    addCheck(dcaForm, trendBox);
    // 多周期梯子接管间距推导后，×1.5 那一半会被整个覆盖掉（不是叠加）——
    // 不说明的话，同时勾两个的人会以为"空头态补仓更保守"，而那件事不会发生
    addHint(dcaForm, "⚠ 开启【多周期梯子】后，其中「补仓间隔×1.5」不生效"
                     "（间距完全由档位带宽推导）；「空头态暂停新首仓」照常生效。");


    // ── v3.0 三层决策 ────────────────────────────────────────────────────────
    // 三个判据平级独立（v3.8 起不再有"三层决策拦截"总开关）。
    // 拆开的价值在可归因：绑在一起时无法知道拦截来自哪一条
    auto* htfBox = new QCheckBox("① 高位拦截：日线%B 高于阈值不开新首仓");
    htfBox->setChecked(prefill ? prefill->cfg.use_htf_filter : true);
    htfBox->setToolTip("大图景已经在高位时不追小回调。\n"
                       "%B = 价格在日线布林带中的相对位置，0=下轨 1=上轨。");
    addCheck(gateForm, htfBox);

    auto* supBox = new QCheckBox("② 支撑拦截：价格须正踩在够格支撑区【内部】");
    supBox->setChecked(prefill ? prefill->cfg.use_sr_support : true);
    supBox->setToolTip(
        "注意是「正处于区域内部」，不是「下方有支撑」——下方 2% 处有铁墙也不算。\n"
        "够格 = 独立共振数≥2（摆动与其算术衍生的斐波归为一族，只计一票）。\n"
        "⚠ 这通常是三条里最紧的一条：区域厚度约 0.5×ATR，而价格大部分时间\n"
        "落在区域之间的空隙里。想放宽拦截先从这条入手。");
    addCheck(gateForm, supBox);

    auto* headBox = new QCheckBox("③ 净空拦截：头顶到最近够格阻力的空间须够止盈");
    headBox->setChecked(prefill ? prefill->cfg.use_sr_headroom : true);
    headBox->setToolTip(
        "净空比 = 到上方最近够格阻力的距离 ÷ 预期止盈距离。\n"
        "通俗说：赚到目标之前有没有一堵墙挡着。头顶无够格阻力时视为无限大（放行）。\n"
        "实测阻力侧门槛放宽是灾难，说明这条判据有真实信息量。");
    addCheck(gateForm, headBox);

    addHint(gateForm, "三条平级独立，全不勾 = 三层决策完全不参与。"
                      "微观层（1h信号+站稳）与趋势过滤沿用各自开关，不受这里控制。");
    auto* htfMaxEdit   = mkEditIn(gateForm, "日线%B 拦截阈值:", prefill ? prefill->cfg.htf_pos_max : 0.60);
    auto* dayChgEdit   = mkEditIn(gateForm, "日涨幅拦截%（0=关）:",
                                  prefill ? prefill->cfg.htf_day_chg_max : 0.0);
    auto* weekChgEdit  = mkEditIn(gateForm, "近7日涨幅拦截%（0=关）:",
                                  prefill ? prefill->cfg.htf_week_chg_max : 0.0);
    addHint(gateForm,
            "两条涨幅与 %B 同源（共用那次日线拉取，不增加请求），但口径不同：\n"
            "%B 问「价格在波动区间的什么位置」，涨幅问「最近涨得多急」。\n"
            "窄幅横盘时 %B 可以贴着上轨而涨幅极小；急涨突破时涨幅很大而 %B 未必越界。\n"
            "「近7日」是滚动口径（相对7根日线前的收盘），不是本周K线——\n"
            "后者每周一归零，闸门会在行情最容易延续的时点失效大半天。\n"
            "做空时镜像：拦的是跌幅。两条独立于上面的①开关，可以只用涨幅不用 %B。\n"
            "⚠ 这两个阈值没有回测依据（回测侧未实现涨幅跟踪），填多少靠手判。");
    auto* headroomEdit = mkEditIn(gateForm, "净空比下限:", prefill ? prefill->cfg.sr_headroom_ratio : 3.0);
    auto* srExitBox = new QCheckBox("止盈锚定阻力区（够格阻力比上轨近时在阻力前落袋，仅动态W）");
    srExitBox->setChecked(prefill ? prefill->cfg.use_sr_exit : false);
    addCheck(riskForm, srExitBox);
    auto* structStopBox = new QCheckBox("结构性止损（持续跌破最深支撑区约1分钟平仓停机，仅动态W+多头）");
    structStopBox->setChecked(prefill ? prefill->cfg.use_structural_stop : false);
    addCheck(riskForm, structStopBox);


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
    indForm->addRow(rsiFilterBox);   // 与其余勾选框同一条对齐轴
    auto* rsiPeriodEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.rsi_period : 14));
    indForm->addRow("RSI周期:", rsiPeriodEdit);
    auto* rsiThEdit = new QLineEdit(QString::number(prefill ? prefill->cfg.rsi_threshold : 30.0));
    indForm->addRow("RSI阈值(多≥/空≤100-此值):", rsiThEdit);

    auto* rsiModeBox = new QComboBox();
    rsiModeBox->addItem("瞬时快照（这一刻RSI到阈值就行）");
    rsiModeBox->addItem("反转确认（先探底跌破，再回穿阈值）");
    rsiModeBox->setCurrentIndex(prefill
        ? (prefill->cfg.rsi_confirm_mode == CcgConfig::RsiConfirmMode::CrossFromOversold ? 1 : 0)
        : 1);   // 默认反转确认（实证：快照模式在大跌段重亏）
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

    sigForm->addRow(indBox);

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

    // 标题行：左边说明，右边快速增减层数（改的是上面的"最大层数"输入框，
    // 预览会跟着刷新——比手动去改那个框直观）
    auto* tierHead = new QWidget();
    auto* tierHeadL = new QHBoxLayout(tierHead);
    tierHeadL->setContentsMargins(0, 4, 0, 0);
    tierHeadL->setSpacing(6);

    auto* tierLbl = new QLabel("层级分配预览（各层资金、预计建仓价、浮亏；横向可滚动）");
    tierLbl->setStyleSheet("color:#58a6ff;font-size:11px;font-weight:bold;");
    tierHeadL->addWidget(tierLbl);
    tierHeadL->addStretch();

    auto mkStepBtn = [](const QString& t) {
        auto* b = new QPushButton(t);
        b->setFixedSize(22, 20);
        b->setStyleSheet("QPushButton{background:#21262d;color:#8b949e;font-size:13px;"
                         "border:1px solid #30363d;border-radius:3px;}"
                         "QPushButton:hover{border-color:#58a6ff;color:#58a6ff;}");
        return b;
    };
    auto* btnTierMinus = mkStepBtn("−");
    auto* btnTierPlus  = mkStepBtn("+");
    btnTierMinus->setToolTip("减少一层");
    btnTierPlus->setToolTip("增加一层");
    tierHeadL->addWidget(btnTierMinus);
    tierHeadL->addWidget(btnTierPlus);
    dv->addWidget(tierHead);

    connect(btnTierMinus, &QPushButton::clicked, &dlg, [maxEntEdit]() {
        int v = maxEntEdit->text().toInt();
        if (v > 1) maxEntEdit->setText(QString::number(v - 1));   // textChanged 会触发预览刷新
    });
    connect(btnTierPlus, &QPushButton::clicked, &dlg, [maxEntEdit]() {
        int v = maxEntEdit->text().toInt();
        if (v < kMaxLayers) maxEntEdit->setText(QString::number(v + 1));
    });

    // 列宽固定 + 横向滚动（而不是 Stretch 挤在一屏里）：列一多，Stretch 会把每列
    // 压到看不清。参考界面同样是固定列宽配横向滚动条
    static const struct { const char* head; int w; } kTierCols[] = {
        {"单",           44},
        {"名义价值",     86},
        {"占比%",        56},
        {"保证金",       80},
        {"间隔%",        62},
        {"追踪建仓%",    76},
        {"止盈%",        62},
        {"止盈回降%",    76},
        {"预计建仓价",   96},
        {"实际建仓价",   96},
        {"预计数量",     88},
        {"浮动盈亏",     84},
        {"满层浮亏",     84},
    };
    constexpr int kTierColN = (int)(sizeof(kTierCols) / sizeof(kTierCols[0]));

    auto* tierTable = new QTableWidget(0, kTierColN);
    {
        QStringList heads;
        for (const auto& c : kTierCols) heads << c.head;
        tierTable->setHorizontalHeaderLabels(heads);
        auto* th = tierTable->horizontalHeader();
        th->setSectionResizeMode(QHeaderView::Fixed);
        for (int i = 0; i < kTierColN; ++i) th->resizeSection(i, kTierCols[i].w);
        th->setStretchLastSection(true);
    }
    tierTable->verticalHeader()->setVisible(false);
    tierTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tierTable->setMaximumHeight(220);
    tierTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 不开斑马纹：全局样式表没定义 alternate-background-color，Qt 会退回默认调色板
    // 的浅灰，在这套深色皮肤上格格不入；项目里其他表格也都不用它，行的区分交给
    // 已有的 gridline-color
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

        double sumMargin = 0, sumLoss = 0, sumQty = 0, sumUnreal = 0;
        bool   haveLoss  = livePrice > 0;

        // 动态W模式下间隔/追踪/止盈是运行时按实时带宽算出来的，写死一个数字是骗人的
        const bool dynMode = dynBandBox->isChecked();
        const QString dynTxt = "动态";
        double tpPct    = tpEdit->text().toDouble();
        double tpTrail  = trailTpEdit->text().toDouble();

        // 编辑一个已有持仓的 bot 时，把每层的真实成交价和当前浮盈填进去；
        // 新建时这些列是空的（参考界面同样是未成交显示 0）
        const std::vector<CcgEntry>* filled = (prefill && !prefill->entries.empty())
                                              ? &prefill->entries : nullptr;

        auto right = Qt::AlignRight | Qt::AlignVCenter;
        auto dim   = QColor("#484f58");

        tierTable->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            double usdt   = allocs[i];
            double pct    = total > 0 ? usdt / total * 100.0 : 0.0;
            double margin = usdt / lev;
            sumMargin += margin;

            int c = 0;
            QColor usdt_col = (i == 0) ? QColor("#58a6ff") : QColor("#3fb950");
            tierTable->setItem(i, c++, mkc(QString("第%1单").arg(i+1), QColor("#8b949e")));
            tierTable->setItem(i, c++, mkc(QString("$%1").arg(usdt, 0, 'f', 1), usdt_col, right));
            tierTable->setItem(i, c++, mkc(QString("%1%").arg(pct, 0, 'f', 1), QColor("#d29922")));
            tierTable->setItem(i, c++, mkc(QString("$%1").arg(margin, 0, 'f', 2),
                                            QColor("#e6edf3"), right));

            // 逐层参数：首仓没有"间隔/追踪建仓"这回事
            tierTable->setItem(i, c++, mkc(i == 0 ? "--" : (dynMode ? dynTxt : QString::number(interv, 'f', 1)),
                                            i == 0 ? dim : (dynMode ? QColor("#a371f7") : QColor("#8b949e"))));
            tierTable->setItem(i, c++, mkc(i == 0 ? "--" : (dynMode ? dynTxt : QString::number(trail, 'f', 1)),
                                            i == 0 ? dim : (dynMode ? QColor("#a371f7") : QColor("#8b949e"))));
            tierTable->setItem(i, c++, mkc(dynMode ? dynTxt : QString::number(tpPct, 'f', 1),
                                            dynMode ? QColor("#a371f7") : QColor("#8b949e")));
            tierTable->setItem(i, c++, mkc(dynMode ? dynTxt : QString::number(tpTrail, 'f', 1),
                                            dynMode ? QColor("#a371f7") : QColor("#8b949e")));

            // 预计建仓价 / 实际建仓价 / 预计数量
            double price = (livePrice > 0) ? predPrice[i] : 0.0;
            double qty   = (price > 0) ? usdt / price : 0.0;
            tierTable->setItem(i, c++, price > 0
                ? mkc(QString::number(price, 'f', 4), QColor("#e6edf3"), right)
                : mkc("--", dim));

            double realPx = 0, realQty = 0;
            if (filled && i < (int)filled->size()) {
                realPx  = (*filled)[i].price;
                realQty = (*filled)[i].qty;
            }
            tierTable->setItem(i, c++, realPx > 0
                ? mkc(QString::number(realPx, 'f', 4), QColor("#58a6ff"), right)
                : mkc("0", dim));

            tierTable->setItem(i, c++, qty > 0
                ? mkc(QString::number(qty, 'f', 6), QColor("#8b949e"), right)
                : mkc("--", dim));
            sumQty += qty;

            // 浮动盈亏（当前）：只有已成交的层才有，按真实成交价对现价算
            if (realPx > 0 && realQty > 0 && livePrice > 0) {
                double up = is_short ? (realPx - livePrice) * realQty
                                     : (livePrice - realPx) * realQty;
                sumUnreal += up;
                tierTable->setItem(i, c++, mkc(QString("%1$%2").arg(up >= 0 ? "+" : "-")
                                                 .arg(std::abs(up), 0, 'f', 2),
                                                up >= 0 ? QColor("#3fb950") : QColor("#f85149"), right));
            } else {
                tierTable->setItem(i, c++, mkc(realPx > 0 ? "--" : "0", dim));
            }

            // 满层浮亏：跌(涨)到最后一层时，这一层的账面亏损
            if (livePrice > 0) {
                double loss = is_short ? qty * (finalPrice - price) : qty * (price - finalPrice);
                loss = std::max(0.0, loss);
                sumLoss += loss;
                tierTable->setItem(i, c++, mkc(QString("$%1").arg(loss, 0, 'f', 2),
                                                QColor("#f85149"), right));
            } else {
                tierTable->setItem(i, c++, mkc("--", dim));
            }
        }

        // 汇总条：参考界面是紧凑的一行。总间隔 = 首仓价到满层价的【实际】跌幅。
        // 两点容易算错：
        //  ① 间隔是相对上一层逐层复利的，"层数×间隔"会大幅高估（8层×8% 线性得
        //     64%，实际只有 40%）
        //  ② 单层步长不是 (1-间隔)，而是 (1-间隔)×(1+追踪建仓)——跌到位只是触发，
        //     还要反弹 trail% 才真正成交，反弹把跌幅吐回去一部分
        // 无实时价时的回退公式必须和有价时算的是同一个东西，否则同一个标签在
        // 两种情况下含义不同
        const double stepFactor = is_short
            ? (1.0 + interv / 100.0) * (1.0 - trail / 100.0)
            : (1.0 - interv / 100.0) * (1.0 + trail / 100.0);
        double totalDrop = (livePrice > 0 && finalPrice > 0)
            ? std::abs(finalPrice / livePrice - 1.0) * 100.0
            : std::abs(1.0 - std::pow(stepFactor, std::max(0, n - 1))) * 100.0;

        QString line1 = QString("单数:%1  |  总量:%2  |  总间隔:%3%  |  杠杆:%4x  |  "
                                "名义价值:$%5  |  需要保证金:$%6")
            .arg(n)
            .arg(sumQty > 0 ? QString::number(sumQty, 'f', 4) : "--")
            .arg(totalDrop, 0, 'f', 1)
            .arg(lev)
            .arg(budget, 0, 'f', 0)
            .arg(sumMargin, 0, 'f', 2);
        QString line2 = haveLoss
            ? QString("满层浮亏合计:$%1  |  全部建仓共需准备:≈$%2%3")
                  .arg(sumLoss, 0, 'f', 2)
                  .arg(sumMargin + sumLoss, 0, 'f', 2)
                  .arg(sumUnreal != 0
                       ? QString("  |  当前总浮动盈亏:%1$%2")
                             .arg(sumUnreal >= 0 ? "+" : "-").arg(std::abs(sumUnreal), 0, 'f', 2)
                       : QString())
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
    // 预览新增了"止盈%/止盈回降%"两列和"动态"标记，这三个控件也要触发刷新，
    // 否则改了止盈参数预览还停在旧值上
    connect(tpEdit,      &QLineEdit::textChanged, &dlg, refreshTier);
    connect(trailTpEdit, &QLineEdit::textChanged, &dlg, refreshTier);
    connect(dynBandBox,  &QCheckBox::toggled,     &dlg, [refreshTier](bool) { refreshTier(); });
    refreshTier();

    if (longBot && shortBot) {
        auto* warnLbl = new QLabel("该品种多/空两个方向都在运行中，保存会同时更新两边的参数");
        warnLbl->setWordWrap(true);
        warnLbl->setStyleSheet("color:#d29922;font-size:11px;");
        dv->addWidget(warnLbl);
    }

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    // 按钮固定在滚动区外：内容一长，放在里面会被滚出可视范围
    { auto* bw = new QWidget(); auto* bl = new QHBoxLayout(bw);
      bl->setContentsMargins(14, 6, 14, 12); bl->addWidget(btnBox);
      outer->addWidget(bw); }
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
    cfg.max_entries   = to_i(maxEntEdit,    7);
    cfg.interval_pct  = to_d(intervalEdit,  8.0);
    cfg.trail_entry   = to_d(trailEntEdit,  1.0);
    cfg.tp_pct        = to_d(tpEdit,        5.0);
    cfg.trail_tp      = to_d(trailTpEdit,   2.0);
    cfg.auto_restart  = autoRestartBox->isChecked();
    cfg.cooldown_secs = to_i(cooldownEdit,  300);
    cfg.stop_loss_pct = to_d(stopLossEdit,  0.0);
    cfg.use_disaster_stop = disStopBox->isChecked();
    cfg.disaster_stop_pct = to_d(disStopEdit, 30.0);

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
    cfg.min_profit_floor  = to_d(floorEdit, 3.5);
    cfg.dyn_fixed_interval = to_d(fixIvEdit, 0.0);
    cfg.mtf_ladder        = mtfBox->isChecked();
    cfg.mtf_tier_layers   = mtfTiersEdit->text().trimmed().toStdString();
    cfg.mtf_k             = to_d(mtfKEdit,   0.5);
    cfg.mtf_min_gap_pct   = to_d(mtfGapEdit, 2.0);
    cfg.use_trend_filter  = trendBox->isChecked();
    cfg.use_htf_filter      = htfBox->isChecked();
    cfg.use_sr_support      = supBox->isChecked();
    cfg.use_sr_headroom     = headBox->isChecked();
    cfg.htf_pos_max         = to_d(htfMaxEdit,   0.60);
    cfg.htf_day_chg_max     = to_d(dayChgEdit,   0.0);
    cfg.htf_week_chg_max    = to_d(weekChgEdit,  0.0);
    cfg.sr_headroom_ratio   = to_d(headroomEdit, 3.0);
    cfg.use_sr_exit         = srExitBox->isChecked();
    cfg.use_structural_stop = structStopBox->isChecked();
    // SR 雷达跟着依赖它的功能自动开关，用户不再单独控制：
    // 开了三层拦截却没有区域数据，结构层会静默地永久 fail-open（闸门形同虚设）；
    // 反过来三个都没开时雷达也没有存在意义，白占 K 线拉取额度
    // SR 雷达跟随任一需要区域数据的功能（高位层用日线带，不依赖雷达）
    cfg.sr_radar = cfg.use_sr_support || cfg.use_sr_headroom ||
                   cfg.use_sr_exit || cfg.use_structural_stop;

    QString symQ = QString::fromStdString(symbol);
    auto apply_one = [&](CcgConfig::Direction dir, const CcgBot* existing) {
        CcgConfig c = cfg;
        c.direction = dir;
        QString dirName = (dir == CcgConfig::Direction::Short) ? "空" : "多";
        if (existing) {
            // 弹窗没有这些项的输入控件，编辑保存时必须从原配置继承——
            // 否则手改过 JSON 的值会被静默重置回默认
            c.trend_interval    = existing->cfg.trend_interval;
            c.trend_ema_period  = existing->cfg.trend_ema_period;
            c.sr_interval       = existing->cfg.sr_interval;
            c.htf_interval      = existing->cfg.htf_interval;
            c.sr_min_confluence = existing->cfg.sr_min_confluence;
            c.sr_independent_conf = existing->cfg.sr_independent_conf;
            c.sr_lower_half_only  = existing->cfg.sr_lower_half_only;
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

    // 配置保存即预热：立即触发一次SR区域重算和趋势/%B拉取，消除"新勾选雷达要等
    // 15分钟、%B要等5分钟"的预热期——否则立即开仓模式的首仓永远在数据到达前发出
    refreshSrZones();
    trendTickCount_ = 0;   // 下一tick立即触发趋势+%B批次
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
    if (!engine_) return;
    int running = 0, withPos = 0;
    for (const auto& b : engine_->get_bots()) {
        if (b.state != CcgBot::State::Stopped) ++running;
        if (b.total_qty > 0) ++withPos;
    }
    if (!confirmDanger("确认全部停止",
            QString("将停止 %1 个运行中的 Bot，并关闭 Tick 定时器。\n\n"
                    "持仓【不会】被平掉，但止盈、止损、补仓全部暂停——"
                    "当前有 %2 个品种持仓，停止期间它们不再受任何本地策略管理。\n\n"
                    "确定要停止吗？").arg(running).arg(withPos),
            "全部停止")) return;

    engine_->stop_all();
    tick_timer_->stop();
    log("所有Bot已停止，Tick定时器已关闭", "WARN");
    refreshBotTable();
    save_bots();
}

void MainWindow::onClearStopped() {
    if (!engine_) return;
    int n = 0, withPos = 0;
    for (const auto& b : engine_->get_bots()) {
        if (b.state != CcgBot::State::Stopped) continue;
        ++n;
        if (b.total_qty > 0) ++withPos;
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
    for (const auto& b : engine_->get_bots())
        if (b.state == CcgBot::State::Stopped) { engine_->remove_bot(b.bot_id); ++done; }
    if (done > 0) log(QString("已清除 %1 个已停止Bot").arg(done), withPos > 0 ? "WARN" : "INFO");
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
    if (!ind_wait.empty() && client_ && !indFetchBusy_.load()) {
        indFetchBusy_.store(true);
        run_async([this, ind_wait]() {
            for (const auto& b : ind_wait) {
                auto snap = client_->fetch_indicators(b.cfg.symbol, b.cfg.kline_interval,
                                                       b.cfg.boll_period, b.cfg.boll_mult,
                                                       b.cfg.rsi_period);
                if (!snap.ok) continue;
                const bool tier0 = b.cfg.mtf_ladder && b.cfg.kline_interval == "1h";
                QMetaObject::invokeMethod(this, [this, bid = b.bot_id, snap, tier0]() {
                    if (!engine_) return;
                    engine_->update_indicator(bid, snap.boll_lb, snap.boll_ub, snap.rsi);
                    // 复用：指标拉的就是 1h 带，正好是多周期梯子的第0档，不必再拉一次
                    if (tier0) engine_->update_mtf_band(bid, 0, snap.boll_lb, snap.boll_ub);
                }, Qt::QueuedConnection);
            }
            indFetchBusy_.store(false);
        });
    }

    // SR雷达：区域每 300 tick（约15分钟）重算一次（首tick立刻算），触区检查每tick做（本地、零开销）
    if (srTickCount_++ % 300 == 0) refreshSrZones();

    // 资金费：费率每 100 tick（约5分钟）刷一次，历史流水每 1200 tick（约1小时）同步一次。
    // 结算本身 8 小时才一次，再密没有意义，纯属浪费限流额度
    if (fundTickCount_++ % 100 == 0) { refreshFunding(); refreshMtfBands(); }

    // 成交明细的延迟落盘：save_trades 做了去抖，被压下的写在这里补上
    if (tradesDirty_) save_trades(true);

    // v3.0 结构摘要喂入引擎：每tick按当前价重算"脚下支撑/头顶阻力/结构止损位"
    // （纯本地计算零开销；区域本体15分钟一换，摘要跟着价格实时变）
    if (engine_ && ticker_) {
        for (const auto& b : engine_->get_bots()) {
            auto sit = srStates_.find(b.cfg.symbol);
            if (sit == srStates_.end() || sit->second.zones.empty()) continue;
            double price = ticker_->mid_price(b.cfg.symbol);
            if (price <= 0) continue;
            decision::DigestOpts dop; dop.min_conf = b.cfg.sr_min_confluence;
            dop.independent_conf = b.cfg.sr_independent_conf;
            dop.lower_half_only  = b.cfg.sr_lower_half_only;
            auto dg = decision::digest_zones(sit->second.zones, price, dop);
            double stop_level = (dg.deep_sup_lo > 0 && sit->second.atr > 0)
                                ? dg.deep_sup_lo - 0.25 * sit->second.atr : 0;
            engine_->update_sr_structure(b.bot_id, dg.at_support, dg.sup_hi,
                                         dg.res_lo, stop_level);
        }
    }

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
    if (srTickCount_ % 300 == 0) {
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
    if (srTickCount_ % 20 == 0 && engine_ && posCacheMs_ > 0 && posAge < 30000) {
        std::vector<CcgEngine::ExchangePos> ex;
        ex.reserve(pos_cache_.size());
        for (const auto& [k, p] : pos_cache_)
            ex.push_back({p.symbol, p.direction, p.qty, p.entry_price});
        auto issues = engine_->reconcile_positions(ex, CcgEngine::ReconcileMode::Periodic);
        if (!issues.empty()) {
            // 明细不在这里打——引擎内部已经逐条 log 过（"⚠ 对账: ..."），
            // 再打一遍就是双份。这里只做落盘、刷新和外部告警
            save_bots();          // 收敛后的状态立刻落盘
            refreshBotTable();
            sendAlert(QString("[CCGMonitor] 运行中对账发现 %1 处不一致，详见日志")
                      .arg(issues.size()));
        }
    }

    // 趋势状态机：4h 级别数据变化慢，每 100 个 tick（约5分钟）拉一次就够；
    // 首个 tick 立刻拉一次，避免刚启动的半小时里趋势过滤空转。
    // v3.0：日线%B（宏观层）搭同一班车——等首仓的 bot 每5分钟拉一次日线布林
    if (trendTickCount_++ % 100 == 0) {
        std::vector<CcgBot> trend_bots, htf_bots;
        for (const auto& b : bots) {
            if (b.state == CcgBot::State::Stopped) continue;
            if (b.cfg.use_trend_filter) trend_bots.push_back(b);
            // %B 对所有非停止 bot 持续保鲜（不限"等首仓中"）：立即开仓模式点继续
            // 3秒内就下单、冷却结束当tick就重进——只给等待中的bot拉的话，这些
            // 首仓永远赶不上数据，%B恒为"缺失(放行)"
            // 涨幅拦截与 %B 同源，任一开启都要拉这份高周期数据
            if (b.cfg.use_htf_filter ||
                b.cfg.htf_day_chg_max > 0 || b.cfg.htf_week_chg_max > 0)
                htf_bots.push_back(b);
        }
        if ((!trend_bots.empty() || !htf_bots.empty()) && !trendFetchBusy_.load()) {
            trendFetchBusy_.store(true);
            run_async([this, trend_bots, htf_bots]() {
                for (const auto& b : trend_bots) {
                    auto t = client_->fetch_trend(b.cfg.symbol, b.cfg.trend_interval,
                                                   b.cfg.trend_ema_period);
                    if (!t.ok) continue;
                    QMetaObject::invokeMethod(this, [this, bid = b.bot_id, bearish = t.bearish]() {
                        if (engine_) engine_->update_trend(bid, bearish);
                    }, Qt::QueuedConnection);
                }
                for (const auto& b : htf_bots) {
                    auto snap = client_->fetch_indicators(b.cfg.symbol, b.cfg.htf_interval,
                                                           20, 2.0, 14);
                    if (!snap.ok) continue;
                    double pb = decision::pct_b(snap.price, snap.boll_lb, snap.boll_ub);
                    const bool tier3 = b.cfg.mtf_ladder && b.cfg.htf_interval == "1d";
                    QMetaObject::invokeMethod(this, [this, bid = b.bot_id, pb, snap, tier3]() {
                        if (!engine_) return;
                        engine_->update_htf(bid, pb, snap.chg_ok, snap.chg_1, snap.chg_7);
                        // 复用：宏观层拉的就是日线带，正好是第3档
                        if (tier3) engine_->update_mtf_band(bid, 3, snap.boll_lb, snap.boll_ub);
                    }, Qt::QueuedConnection);
                }
                trendFetchBusy_.store(false);
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
// 最新成交价的小数位数。
//
// 首选交易所给的 tick_size（最小价格步长），它是精确答案。但 tick_size 是异步
// 预取的（见 ensureSymbolInfoAsync），拿不到时传进来是 0。
// 此处原先写死兜底 2 位小数，后果是：价格低于 $0.01 的币一律显示成 "0.00"，
// 看起来像行情挂了，其实只是品种精度还没到位——而这个格子 100ms 刷新一次，
// 若该品种的 symbol_info 始终取不回来，就会永远显示 0。
// 兜底改成按数量级推，口径与本文件的 fmt_price 一致。
// tick_size ≥ 1 的情形保持原样（2 位），不在本次修复范围内。
// ─────────────────────────────────────────────────────────────────────────────
static QString fmt_tick_px(double p, double tick) {
    if (p <= 0) return "--";
    int dp = 2;
    if (tick > 0 && tick < 1.0) {
        double t = tick; dp = 0;
        while (t < 1.0 - 1e-9 && dp < 8) { t *= 10; ++dp; }
    } else if (tick <= 0) {
        dp = p >= 100 ? 2 : p >= 1 ? 4 : p >= 0.01 ? 5 : p >= 0.0001 ? 6 : 8;
    }
    return QString::number(p, 'f', dp);
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
        QString lastPx = (tick.last_price > 0) ? fmt_tick_px(tick.last_price, tick_size) : "--";
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
// 动态W模式下"保底达标"只是必要条件：还要价格触及上轨才会激活追踪止盈。
// 只显示"已达标"会让人以为马上就要平仓了
static QString bot_tp_hint(const CcgBot& b, bool floor_ok) {
    if (b.ind_boll_ub <= 0)
        return QStringLiteral("上轨数据未就绪");
    const bool is_long = (b.cfg.direction != CcgConfig::Direction::Short);
    const double band = is_long ? b.ind_boll_ub : b.ind_boll_lb;
    const bool touched = is_long ? (b.current_price >= band) : (b.current_price <= band);
    if (touched && floor_ok) return QStringLiteral("上轨已触及 + 保底达标 → 追踪止盈已可激活");
    if (!floor_ok)           return QString("还需价格涨到 %1 才够保底").arg(
                                    b.avg_price * (1.0 + b.cfg.min_profit_floor / 100.0), 0, 'f', 4);
    return QString("保底已达标，但还需触及%1轨 %2")
           .arg(is_long ? "上" : "下").arg(band, 0, 'f', 4);
}

void MainWindow::refreshBotTable() {
    if (!engine_) { botTable_->setRowCount(0); return; }

    auto bots = engine_->get_bots();
    botTable_->setRowCount((int)bots.size());
    // 行数变了就把键表整体作废——行与 bot 的对应关系已经错位
    if (opRowKeys_.size() != bots.size()) opRowKeys_.assign(bots.size(), QString());

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
        QString signal_tip;   // 「等待信号」卡在哪一步的详细说明（挂状态列悬停）
        switch (b.state) {
        case CcgBot::State::Running:
            if (b.entries.empty()) {
                if (b.cfg.entry_mode == CcgConfig::EntryMode::Indicator) {
                    // 「等待信号」原先是个黑盒：价格没到下轨、RSI没探底、探底了没回穿、
                    // 数据过期——四种情况长得一模一样，而拦截日志有去重（同一原因只打
                    // 一次），所以日志里也看不出来。这里把卡点直接显示出来。
                    // 注意：指标信号是第①道闸，它不过就走不到三层拦截，也就不会有
                    // 任何拦截日志——这正是"一直没提示也不开单"的成因
                    state_s = "等待信号"; state_c = QColor("#a371f7");
                    signal_tip.clear();
                    if (!b.ind_ok) {
                        state_s = "等待·取数中";
                        signal_tip = "还没取到该品种的指标数据（BOLL/RSI）。\n"
                                     "刚添加或刚连接时正常，约 5 分钟内会拉到。\n"
                                     "长时间停在这里通常是该品种 K 线拉取失败——检查品种名是否正确。";
                    } else if (std::chrono::steady_clock::now() - b.ind_time >= kIndStale) {
                        state_s = "等待·数据过期"; state_c = QColor("#d29922");
                        signal_tip = "指标数据超过 180 秒未更新，信号判定已冻结（宁可错过不可乱开）。\n"
                                     "通常是网络问题或该品种K线拉取失败。";
                    } else {
                        const bool is_long = (b.cfg.direction != CcgConfig::Direction::Short);
                        const bool priceOk = is_long ? (b.current_price <= b.ind_boll_lb)
                                                     : (b.current_price >= b.ind_boll_ub);
                        bool rsiOk = true, needDip = false;
                        if (b.cfg.use_rsi_filter) {
                            const bool snap = is_long ? (b.ind_rsi >= b.cfg.rsi_threshold)
                                                      : (b.ind_rsi <= 100.0 - b.cfg.rsi_threshold);
                            if (b.cfg.rsi_confirm_mode == CcgConfig::RsiConfirmMode::CrossFromOversold) {
                                needDip = !b.ind_dipped;
                                rsiOk = b.ind_dipped && snap;
                            } else rsiOk = snap;
                        }
                        if (!priceOk && !rsiOk)      state_s = "等待·破轨+RSI";
                        else if (!priceOk)           state_s = "等待·破轨";
                        else if (needDip)            state_s = "等待·RSI探底";
                        else if (!rsiOk)             state_s = "等待·RSI回穿";
                        else                         state_s = "信号已满足";   // 卡在后面的闸

                        const double band = is_long ? b.ind_boll_lb : b.ind_boll_ub;
                        signal_tip = QString("首仓要【同时】满足这两条：\n\n"
                                             "① 价格%1轨：现价 %2 / %3轨 %4  %5\n"
                                             "② RSI：当前 %6")
                            .arg(is_long ? "破下" : "破上")
                            .arg(b.current_price, 0, 'f', 4)
                            .arg(is_long ? "下" : "上").arg(band, 0, 'f', 4)
                            .arg(priceOk ? "✓" : "✗")
                            .arg(b.ind_rsi, 0, 'f', 1);
                        if (!b.cfg.use_rsi_filter) {
                            signal_tip += "（RSI 过滤已关）";
                        } else if (b.cfg.rsi_confirm_mode == CcgConfig::RsiConfirmMode::CrossFromOversold) {
                            signal_tip += QString("\n   反转确认：需先探底跌破 %1（%2），再回穿 %3（%4）")
                                .arg(b.cfg.rsi_oversold_th, 0, 'f', 0)
                                .arg(b.ind_dipped ? "已探底✓" : "未探底✗")
                                .arg(b.cfg.rsi_threshold, 0, 'f', 0)
                                .arg(rsiOk ? "✓" : "✗");
                        } else {
                            signal_tip += QString("  需 ≥%1  %2")
                                .arg(b.cfg.rsi_threshold, 0, 'f', 0).arg(rsiOk ? "✓" : "✗");
                        }
                        signal_tip += "\n\n两条都满足后才会走到三层拦截；在那之前不会有任何拦截日志。";
                        // 引擎自己记的最近一次判定结果。信号已满足却不开仓时，
                        // 答案就在这里（趋势/三层/保证金）——这个字段以前完全不上界面
                        if (!b.last_action.empty())
                            signal_tip += "\n当前引擎记录：" + QString::fromStdString(b.last_action);
                        // 完整判据快照带实时数字，每 tick 刷新——"信号已满足却不开仓"
                        // 时，这一行直接告诉你是三条里的哪一条把它挡住的
                        if (!b.last_decision.empty())
                            signal_tip += "\n三层判据：" + QString::fromStdString(b.last_decision);
                    }
                } else {
                    // 「立即开仓」模式没有指标信号这道闸，所以卡住的原因只可能来自
                    // 后面三道。这些状态本来只反映在 last_action 里（不上表格），
                    // 界面上一样是个黑盒
                    state_s = "等待首仓"; state_c = QColor("#58a6ff");
                    signal_tip = "立即开仓模式：没有指标信号这道闸，理论上下一个 tick 就会开首仓。\n"
                                 "若长时间停在这里，只可能被后面三道之一挡住：\n"
                                 "  ① 趋势过滤——高周期空头态暂停新首仓\n"
                                 "  ② 三层拦截——高位 / 支撑 / 净空任一不满足\n"
                                 "  ③ 账户总保证金上限——已用额度不够再开一仓\n"
                                 "具体是哪一条，看运行日志里该品种最近的一条拦截提示"
                                 "（同一原因只打一次，不会重复刷）。";
                    if (!b.last_action.empty())
                        signal_tip += "\n\n当前引擎记录：" + QString::fromStdString(b.last_action);
                    if (!b.last_decision.empty())
                        signal_tip += "\n三层判据：" + QString::fromStdString(b.last_decision);
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
        // 层进度 + 满层健康度。颜色即判据，不用去读数字：
        //   实测（60品种）满层<10% 时 22/22 盈利；>75% 时 0/6 盈利、中位亏 16854
        // 分母不足 1 天时不着色——样本太少，颜色会误导
        {
            const double fp   = b.full_layer_pct();
            const bool   ripe = b.alive_secs >= 86400;
            QColor lc = QColor("#58a6ff");
            if (ripe) {
                if      (fp >= 50.0) lc = QColor("#f85149");   // 危险：已进入失败模式区间
                else if (fp >= 25.0) lc = QColor("#d29922");   // 警告：盈利概率开始下滑
            }
            auto* it = mkc(layers, lc);
            QString tip = QString("满层时间占比 %1%（统计时长 %2）")
                          .arg(fp, 0, 'f', 1)
                          .arg(b.alive_secs >= 86400
                               ? QString("%1 天").arg(b.alive_secs / 86400.0, 0, 'f', 1)
                               : QString("%1 小时").arg(b.alive_secs / 3600.0, 0, 'f', 1));
            if (!ripe) {
                tip += "\n统计不足 1 天，暂不判读";
            } else if (fp >= 50.0) {
                tip += "\n\n⚠ 危险区：回测中满层>75% 的品种 0/6 盈利（中位亏 16854U）。\n"
                       "满 8 层且持续一个月以上，应停掉该 bot，不要补钱摊平。";
            } else if (fp >= 25.0) {
                tip += "\n\n注意：回测中满层 25~50% 的品种 8/11 盈利，已明显低于\n"
                       "满层<10% 那档的 22/22。观察即可，别加预算。";
            } else {
                tip += "\n\n健康：回测中满层<10% 的品种 22/22 盈利。";
            }
            it->setToolTip(tip);
            botTable_->setItem(i, 4, it);
        }
        // 均价 + 资金费修正后的回本价（悬停）。放在均价上是有道理的：回本价本质
        // 就是被资金费修正过的均价——对长期持有的用法，那才是真正要盯的数
        auto* avg_item = mkc(fmt_price(disp_avg), QColor("#8b949e"));
        {
            auto fe = funding_.get(b.cfg.symbol);
            if (fe.since_open < 0 && b.avg_price > 0 && b.total_qty > 0) {
                double be = FundingLedger::effective_breakeven(
                    b.avg_price, b.total_qty, fe.since_open,
                    b.cfg.direction == CcgConfig::Direction::Long);
                avg_item->setText(fmt_price(disp_avg) + " *");
                avg_item->setToolTip(
                    QString("均价 %1\n本轮持仓已付资金费 %2 USDT\n回本价 %3（+%4%）\n"
                            "当前费率年化 %5%")
                    .arg(fmt_price(b.avg_price))
                    .arg(-fe.since_open, 0, 'f', 2)
                    .arg(fmt_price(be))
                    .arg((be / b.avg_price - 1.0) * 100.0, 0, 'f', 2)
                    .arg(-FundingLedger::annualized_pct(fe.rate), 0, 'f', 1));
            }
        }
        botTable_->setItem(i, 5,  avg_item);

        // 最新成交价 + 延迟：来自 WebSocket aggTrade 流，独立于策略引擎的 tick 价格
        auto tick = ticker_ ? ticker_->get(b.cfg.symbol) : BookTickerStream::Tick{};
        double tick_size = 0;
        if (client_) {
            TradingClient::SymbolInfo info;
            if (client_->try_get_symbol_info(b.cfg.symbol, info) && info.valid) tick_size = info.tick_size;
            else ensureSymbolInfoAsync(b.cfg.symbol);
        }
        QString lastPx = (tick.last_price > 0) ? fmt_tick_px(tick.last_price, tick_size) : "--";
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
        auto* roi_item = mkc(roi_s, roi >= 0 ? QColor("#3fb950") : QColor("#f85149"));
        // 这一列是【杠杆后的资金回报率】（浮盈÷保证金），而止盈用的所有 % 都是
        // 【价格相对均价的涨幅】——两者差一个杠杆倍数。盯着这一列判断"快到止盈没"
        // 会系统性误判：3倍杠杆下保底利润 2% 触发时，这里显示的是 +6%。
        // 悬停把两个口径并排放出来，消掉这个误读
        if (margin > 0 && b.avg_price > 0 && b.current_price > 0) {
            const bool is_long = (b.cfg.direction != CcgConfig::Direction::Short);
            double gain = (is_long ? (b.current_price / b.avg_price - 1.0)
                                   : (1.0 - b.current_price / b.avg_price)) * 100.0;
            const double floor_pct = b.cfg.min_profit_floor;
            QString tip = QString("浮动盈亏 $%1\n保证金 $%2\n收益率 %3%4%（已按 %5x 杠杆放大）\n\n"
                                  "── 止盈实际看的是价格涨幅 ──\n价格涨幅 %6%7%")
                .arg(unreal, 0, 'f', 2).arg(margin, 0, 'f', 2)
                .arg(roi >= 0 ? "+" : "").arg(roi, 0, 'f', 1).arg(b.cfg.leverage)
                .arg(gain >= 0 ? "+" : "").arg(gain, 0, 'f', 2);
            if (b.cfg.dynamic_band_mode) {
                tip += QString("（保底线 %1%，%2）")
                    .arg(floor_pct, 0, 'f', 1)
                    .arg(gain >= floor_pct ? "已达标" : "未达标");
                // 达标只是必要条件：动态W下还要触上轨才激活追踪止盈
                tip += QString("\n%1").arg(bot_tp_hint(b, gain >= floor_pct));
            } else {
                tip += QString("（止盈线 %1%，%2）")
                    .arg(b.cfg.tp_pct, 0, 'f', 1)
                    .arg(gain >= b.cfg.tp_pct ? "已达标" : "未达标");
            }
            roi_item->setToolTip(tip);
        }
        botTable_->setItem(i, 10, roi_item);

        // 强平价：来自交易所真实持仓（refreshPositions() 每 3s 拉取一次），本地无法准确估算
        double liq = has_real_pos ? pit->second.liq_price : 0;
        botTable_->setItem(i, 11, mkc(liq > 0 ? fmt_price(liq) : "--", QColor("#d29922")));

        botTable_->setItem(i, 12, mkc(rea_s, b.realized_pnl >= 0 ? QColor("#3fb950") : QColor("#f85149")));
        auto* state_item = mkc(state_s, state_c);
        if (!signal_tip.isEmpty()) state_item->setToolTip(signal_tip);
        botTable_->setItem(i, 13, state_item);

        // 操作列。
        // 这一列原先【每次刷新都整套重建】——3秒一次 × 每行3个按钮，31个bot就是
        // 每3秒销毁重建近百个控件。除了浪费，还有个真实的交互 bug：点击那一瞬间
        // 正好赶上刷新，按钮被 setCellWidget 销毁，这一下点击就丢了（表现为"点了没反应"）。
        // 现在按"影响按钮外观/行为的状态"做键，键没变就原样留着不动。
        std::string bid  = b.bot_id;
        bool        is_stopped = (b.state == CcgBot::State::Stopped);
        const QString opKey = QString("%1|%2|%3")
            .arg(QString::fromStdString(bid))
            .arg(is_stopped ? 1 : 0)
            .arg(b.entries.empty() ? 0 : 1);   // 平仓按钮的可用性只取决于有没有持仓
        if (i < (int)opRowKeys_.size() && opRowKeys_[i] == opKey
            && botTable_->cellWidget(i, 14) != nullptr) {
            continue;   // 本行按钮无需变动，跳过重建（后面没有别的列了）
        }

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
            if (is_stopped) {
                engine_->resume_bot(bid);
                // 「全部停止」会关掉 tick 定时器——单个 bot 恢复时必须把它拉起来，
                // 否则 bot 显示"运行中"但引擎永远不被驱动：止损/止盈/补仓全部失效
                if (tick_timer_ && !tick_timer_->isActive()) {
                    tick_timer_->start();
                    log("Tick 定时器已重新启动");
                }
            } else {
                engine_->stop_bot(bid);
            }
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
        // 二次确认：平仓是【立刻市价成交、动真钱、不可撤销】的操作，
        // 而这个按钮就挨着"停止"和"删除"，误点代价太大
        {
            QString csym = QString::fromStdString(b.cfg.symbol);
            QString cdir = QString::fromStdString(CcgEngine::dir_name(b.cfg.direction));
            double  cqty = b.total_qty, cavg = b.avg_price, cunr = unreal;
            connect(btnClose, &QPushButton::clicked, [this, bid, csym, cdir, cqty, cavg, cunr]() {
                if (!engine_) return;
                if (!confirmDanger("确认平仓",
                        QString("%1 %2\n持仓 %3   均价 $%4\n当前浮动盈亏 %5$%6\n\n"
                                "将【立刻市价平掉全部持仓】，成交后不可撤销。\n\n确定要平仓吗？")
                            .arg(csym).arg(cdir)
                            .arg(cqty, 0, 'f', 6).arg(cavg, 0, 'f', 4)
                            .arg(cunr >= 0 ? "+" : "-").arg(std::abs(cunr), 0, 'f', 2),
                        "立刻平仓")) return;
                engine_->close_bot(bid);   // 异步市价平仓，完成后由 log 回调刷新表格
                log("已发送平仓请求（若无持仓或订单正在处理中会自动忽略）", "WARN");
            });
        }
        opL->addWidget(btnClose);

        auto* btnDel = new QPushButton("删除");
        btnDel->setFixedHeight(20);
        btnDel->setStyleSheet(
            "QPushButton{background:#2d333b;color:#8b949e;font-size:11px;padding:0 6px;}");
        // 二次确认：删除本身不平仓，但会让本地不再跟踪这个仓位——
        // 有持仓时删掉等于亲手制造一个无人管理的孤儿仓位
        {
            QString dsym = QString::fromStdString(b.cfg.symbol);
            double  dqty = b.total_qty;
            connect(btnDel, &QPushButton::clicked, [this, bid, dsym, dqty]() {
                if (!engine_) return;
                QString warn = dqty > 0
                    ? QString("\n\n⚠ 该 Bot 仍持有 %1 的仓位！删除【不会】平掉它，"
                              "但本地从此不再跟踪——它会变成交易所上无人管理的孤儿仓位，"
                              "没有任何止盈止损。").arg(dqty, 0, 'f', 6)
                    : QString();
                if (!confirmDanger("确认删除 Bot",
                        QString("将删除 %1 的 Bot 配置。%2\n\n确定要删除吗？")
                            .arg(dsym).arg(warn),
                        "删除")) return;
                engine_->remove_bot(bid);
                refreshBotTable();
                save_bots();
            });
        }
        opL->addWidget(btnDel);
        opL->addStretch();

        botTable_->setCellWidget(i, 14, opW);
        if (i < (int)opRowKeys_.size()) opRowKeys_[i] = opKey;
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
