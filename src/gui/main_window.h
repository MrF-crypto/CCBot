#pragma once
#include <QMainWindow>
#include <QTableWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QCheckBox>
#include <QSplitter>
#include <QPoint>
#include <QElapsedTimer>
#include <memory>
#include <functional>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <set>

#include "core/ccg_engine.h"
#include "core/funding_ledger.h"
#include "core/thread_pool.h"
#include "net/trading_client.h"
#include "net/book_ticker_stream.h"
#include <map>

namespace ccg {

using namespace ccbot;

// ── 长跑上限 ─────────────────────────────────────────────────────────────────
// 这个程序的设计用法是连续挂几个月，任何"每次事件追加一条、从不裁剪"的结构
// 都会变成必然的增长点。完整历史照常落盘，内存里只留最近的部分。
inline constexpr int    kLogMaxLines   = 3000;    // 日志框保留行数
inline constexpr size_t kMaxTrades     = 20000;   // 内存/落盘保留的成交记录条数
inline constexpr int    kTradeSaveMinMs = 3000;   // 成交落盘的最小间隔（合并密集平仓）

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // 数据目录（便携模式）：main.cpp 的单实例锁也用它，故公开
    static QString portable_data_dir();

private slots:
    void onConnect();
    void onStopAll();
    void onClearStopped();
    void onTick();
    void refreshBotTable();
    void refreshLiveQuotes();
    void refreshPositions();
    void onWatchlistContextMenu(const QPoint& pos);
    void onAddWatchSymbol();
    void refreshAccount();
    void updateHeader();
    void openSettingsDialog();

private:
    void buildUi();
    void log(const QString& msg, const QString& level = "INFO");
    void run_async(std::function<void()> fn);

    // 品种右键 → 策略配置弹窗（新建或编辑已存在的 bot 都走这里）
    void openStrategyDialog(const std::string& symbol);

    // 危险操作的二次确认（默认按钮是取消，防误点后顺手回车）
    bool confirmDanger(const QString& title, const QString& body, const QString& okText);
    void refreshFunding();
    void refreshMtfBands();   // 多周期梯子的 4h/12h 档带值（1h/1d 由别处顺带喂）
    void refreshStats();
    void openTradeHistoryDialog();
    // 品种精度信息缓存未命中时，去后台线程取一次，绝不在 GUI 线程同步阻塞等待
    void ensureSymbolInfoAsync(const std::string& symbol);
    // 品种的价格步长；缓存未命中时顺带发起异步预取，返回 0（调用方按数量级兜底）。
    // 提成一个函数是因为这段查询原先在 refreshLiveQuotes 和 refreshBotTable 里
    // 各有一份逐字相同的副本——同一处的 fmt_tick 就是这样被改了一份漏了一份
    double tickSizeOf(const std::string& symbol);
    // 24h 涨跌：推送流优先，回落到全市场 REST 快照。
    // out_stale 回填"该续期了"，调用方据此触发那一次全市场请求
    bool   chg24Of(const std::string& symbol, double& out_pct, bool& out_stale);
    // 删除 bot 后调用：若已无任何 bot 使用该品种，退订它的行情流。
    // 不退订的话 streams_ 只增不减，重连时全量重订，反复增删会一路累积到
    // 币安合约单连接 200 条流的上限，超出后【静默】失效
    void unsubscribeIfUnused(const std::string& symbol);

    // 持久化（便携模式：数据存程序目录 data/ 下，不可写时回退 AppData）
    void migrate_appdata_if_needed();
    std::string cred_path()     const;
    std::string bot_cfg_path()  const;
    std::string trade_path()   const;
    std::string settings_path() const;
    std::string log_path()      const;
    std::string funding_path()  const;
    void save_credentials();
    void load_credentials();
    void save_bots();
    void load_and_restore_bots();
    void save_trades(bool force = false);
    void load_trades();
    void save_settings();
    void load_settings();

    // 关键事件外部提醒（Telegram/企业微信/飞书 webhook 等），后台线程发送，不阻塞 GUI
    void sendAlert(const QString& text);

    // 后端
    std::shared_ptr<TradingClient>     client_;
    std::shared_ptr<CcgEngine>         engine_;
    // pool_ = 引擎专用（下单/平仓）；fetchPool_ = 数据拉取专用（账户/持仓/指标/趋势）。
    // 必须分开：拉取任务动辄几百毫秒~几秒，混在一个池里会把手动平仓排到队尾等十几秒
    std::shared_ptr<ThreadPool>        pool_;
    std::shared_ptr<ThreadPool>        fetchPool_;
    std::unique_ptr<BookTickerStream>  ticker_;

    QTimer* tick_timer_  = nullptr;
    QTimer* ob_timer_    = nullptr;
    QTimer* header_timer_ = nullptr;   // 呼吸灯 + 连接计时，高频刷新

    // ── 连接区 ──（API Key/Secret/测试网的输入控件挪进了"设置"弹窗，这里只存值）
    QString      apiKey_;
    QString      apiSecret_;
    bool         testnet_ = false;
    int          accountMode_ = 0;   // 0=普通合约(fapi)  1=统一账户(papi)
    QPushButton* btnConnect_;
    QLabel*      connLabel_;
    QLabel*      breatheDot_  = nullptr;   // 连接状态呼吸灯
    QLabel*      equityLabel_ = nullptr;   // 权益/可用，顶部常驻
    QLabel*      mmrLabel_    = nullptr;   // uniMMR，顶部常驻（仅统一账户）
    QLabel*      rateLabel_   = nullptr;   // 限流状态，仅在被限速/封禁时显示
    QLabel*      fundLabel_   = nullptr;   // 累计资金费，顶部常驻（非零时才显示）
    QLabel*      pnlBadge_    = nullptr;   // 累计已实现盈亏徽标，顶部常驻

    // NetworkError：曾经连接成功，但账户接口连续拉取失败（网络断了/VPN掉了这种），
    // 跟 Failed（一开始就没连上，比如密钥错）区分开，方便判断要不要报警、要不要重置计时
    enum class ConnState { Disconnected, Connecting, Connected, Failed, NetworkError };
    ConnState      connState_ = ConnState::Disconnected;
    QString        connNetName_;       // "主网"/"测试网"，连接成功后顶部计时行要用
    QElapsedTimer  connect_elapsed_;   // 连接成功后开始计时，用于顶部计时显示
    QElapsedTimer  breathe_clock_;     // 呼吸灯相位时钟
    void setConnState(ConnState s);

    // 权益/可用，随 onTick 异步刷新（不再只在连接那一刻查询一次）
    TradingClient::AccountInfo account_info_;

    // ── 全局设置（账户级总保证金上限 / 警报 webhook）──
    double  maxTotalMargin_ = 0;   // 0=不限
    QString alertWebhook_;
    bool    alertedDisconnect_ = false;   // 断线提醒去重，恢复连接后重置
    int     netFailCount_      = 0;       // 账户接口连续拉取失败次数，用于判定网络异常

    // ── 加品种 ──
    QLineEdit*    addSymbolEdit_   = nullptr;

    // ── 实盘监控表（右键品种 → 策略配置弹窗）──
    QTableWidget* botTable_    = nullptr;
    QLabel*       summaryLabel_ = nullptr;
    // 操作列按钮的重建键：键没变就不重建控件（避免点击被刷新吞掉）
    std::vector<QString> opRowKeys_;

    // ── 交易明细 / 盈利统计 ──
    std::vector<TradeRecord> trades_;
    qint64 lastTradeSaveMs_ = 0;   // 落盘去抖
    bool   tradesDirty_     = false;
    QLabel*       statsLabel_ = nullptr;

    // symbol + "_L"/"_S" → 交易所真实持仓（强平价来源，随 tick_timer_ 每 3s 刷新一次）
    std::unordered_map<std::string, TradingClient::Position> pos_cache_;
    // pos_cache_ 最后一次【成功拉取】的时刻（0=从未成功）。
    // 周期对账必须靠它区分"交易所确实没有持仓"和"我还没拿到数据"——
    // 两者的 pos_cache_ 都是空的，但前者该清本地仓位、后者绝不能动。
    // 拿不到数据就当成"这一轮不对账"，宁可晚一分钟发现，也不能凭空清掉真实持仓
    qint64 posCacheMs_ = 0;

    // 正在后台预取品种精度信息的品种集合，避免同一品种被重复发起请求（仅 GUI 线程访问）
    std::set<std::string> pendingSymbolFetch_;

    // ── 各批次的 tick 计数（仅GUI线程访问）──
    int slowTickCount_  = 0;   // 慢批次节拍：对账(每20) / 重新对时(每300)
    int trendTickCount_ = 0;
    int fundTickCount_  = 0;
    // 资金费账本：每 8 小时结算一次的真实现金流出，不是浮亏。
    // 只记账不参与任何交易决策
    FundingLedger     funding_;
    bool              fundingBackfilled_ = false;

    // 周期性拉取的防堆积守卫：上一批任务没跑完就跳过本批。没有守卫的话，
    // bot 数量多时（31个×每个~0.3s）批量任务的生产速度会超过消化速度，
    // 拉取队列无限增长——弹窗预览等一次性任务被排到队尾永远轮不到
    std::atomic<bool> indFetchBusy_{false};
    std::atomic<bool> trendFetchBusy_{false};
    // REST 价格兜底的防重入。串行遍历全部缺价品种，品种一多远超 3 秒的 tick 周期，
    // 没有这道闸会在 fetchPool_ 里无限堆积并饿死高周期指标拉取
    std::atomic<bool> restFetchBusy_{false};
    // 24h 涨幅的 REST 兜底：全市场一次取回，缓存到下一轮
    // 软/硬两条线是 stale-while-revalidate：超过 soft 就后台刷新但【继续用旧值】，
    // 只有超过 hard 才判为无数据。单一阈值会在每次过期时制造一个"没有数据"的
    // 空窗——v4.0.14 的实盘日志里是精确的 90 秒周期、3 秒空窗，
    // strict 闸门会在那 3 秒里假拦截一次并刷一条噪音日志
    static constexpr int64_t kChg24SoftMs = 60'000;    // 超过就后台续期
    static constexpr int64_t kChg24HardMs = 600'000;   // 超过才算真没有
    std::atomic<bool> chg24FetchBusy_{false};
    std::unordered_map<std::string, double> chg24Rest_;
    std::mutex        chg24Mtx_;
    int64_t           chg24RestMs_ = 0;
    std::atomic<bool> accFetchBusy_{false};
    std::atomic<bool> posFetchBusy_{false};
    std::atomic<bool> fundFetchBusy_{false};
    std::atomic<bool> mtfFetchBusy_{false};

    // ── 日志 ──
    QPlainTextEdit* logBox_ = nullptr;   // 上限 kLogMaxLines 行，超出自动丢最早的
};

} // namespace ccg
