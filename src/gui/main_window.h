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

// 只在成员函数签名里出现，前向声明即可——不为一个指针类型把整个 QtWidgets
// 头拖进每个包含 main_window.h 的翻译单元
class QVBoxLayout;

#include "core/ccg_engine.h"
#include "core/sar_engine.h"
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

// ── 一张表容纳两套策略 ───────────────────────────────────────────────────────
// v4.5.0 起 DCA 与 SAR 合并进同一张监控表（原先是两个标签页）。合并只发生在
// 【界面层】：两个引擎各自照原样持有自己的 bot、各自落盘自己的文件，这里只是把
// 两边的 bot 排成一个行序列给表格用。
//
// 这么做的代价是列语义要跨策略复用（层进度↔金字塔档数、均价↔开仓价、
// 强平价↔止损线）；换来的是"一个品种一套策略"从【4 处手写检查】变成配置弹窗里
// 单选框的天然性质。
struct BotRow {
    enum class Kind { Dca, Sar };
    Kind        kind = Kind::Dca;
    std::string bot_id;
    std::string symbol;
    // 排序键：先品种字典序，再 _B(0) < _L(1) < _S(2) < SAR(3)。
    // 前三档刻意和 CcgEngine 的 bot_id 后缀字典序一致——DCA 的 bot 装在
    // std::map 里本来就是这个顺序，所以现有用户看到的行序一行都不会变
    int         sort_key = 0;
    const CcgBot* dca = nullptr;   // kind==Dca 时有效
    const SarBot* sar = nullptr;   // kind==Sar 时有效
};

// 表格底部汇总行的累加器。两种行都要往里加，所以提成一个结构体传引用——
// 否则 fillSarRow 要么返回一个五元组，要么把汇总算两遍
struct RowTotals {
    int    running = 0, cooling = 0, stopped = 0;
    double unreal  = 0, real    = 0;
};

// SAR 配置表单的控件集合。定义在 sar_panel.cpp —— main_window.cpp 只需要能
// 持有并传递它，不需要知道里面有哪些控件（那是 30 多个指针，摊到头文件里
// 只会让每次改一个 spinbox 都触发全量重编）
struct SarFormWidgets;

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

    // 品种右键 → 策略配置弹窗（新建或编辑已存在的 bot 都走这里）。
    // 弹窗顶部可选策略：网格 DCA / 趋势 SAR，下面的表单整片切换
    void openStrategyDialog(const std::string& symbol);

    // 两个引擎的 bot 合成一个行序列。vector 由【调用方持有】——BotRow 里存的是
    // 裸指针，若在这里构造临时 vector 再返回，指针立刻悬空
    std::vector<BotRow> buildRows(const std::vector<CcgBot>& dca,
                                  const std::vector<SarBot>& sar) const;

    // ── SAR 趋势跟随（与 DCA 并列的第二套策略，现已同表显示）────────────────
    // 把一行填成 SAR 行。列位复用 DCA 的 16 列，语义映射见函数体里的对照表
    void fillSarRow(int row, const SarBot& b, RowTotals& t);
    // SAR 表单的构建与回读。拆成两半是为了让 openStrategyDialog 能把表单嵌进
    // 自己的分页里，而不用把 30 多个控件的构造逻辑复制一份
    std::shared_ptr<SarFormWidgets> buildSarForm(QVBoxLayout* into, const SarConfig& c);
    bool collectSarForm(const std::shared_ptr<SarFormWidgets>& w, SarConfig& out);
    // 校验 + 落地（建/改 bot、订阅行情、落盘）。返回 false = 已向用户报错且未改动
    bool applySarConfig(SarConfig c, const SarBot* existing);
    std::string sar_cfg_path()   const;   // 配置与运行时状态同一个文件
    void save_sar_bots();          // 配置 + 运行时状态一起落盘
    void load_and_restore_sar();

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
    std::shared_ptr<SarEngine>         sar_engine_;
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
    // DCA 与 SAR 同表。行序由 buildRows() 决定，与两个引擎的 bot 一一对应
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
    // 已经报过"PRICE_FILTER 缺失"的品种，每个只报一次（仅 GUI 线程访问）
    std::set<std::string> tickWarned_;

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
    // SAR 信号拉取（ATR + 唐奇安）。与指标批次分开：周期不同（60秒 vs 5分钟），
    // 而且 SAR 的 bot 集合与 DCA 的完全没有交集
    std::atomic<bool> sarSigBusy_{false};
    // 新增/恢复 SAR bot 后置位，让下一个 tick 立刻拉一次信号而不是等满 20 拍。
    // 没有它的话刚添加的品种会干等最多 60 秒，界面上什么都没有——
    // 而用户此刻正盯着看它到底有没有在工作
    std::atomic<bool> sarSigForce_{false};
    // 已经报过「信号拉不到」的品种（仅 GUI 线程访问）。存在的理由是去重：
    // 拉取每分钟一轮，而一个拼错的品种会永远失败——不去重就是每小时 60 条
    // 一模一样的告警，把真正有用的日志全冲走。只在【集合发生变化】时报
    std::set<std::string> sarSigFailed_;
    std::atomic<bool> dcaAtrBusy_{false};   // DCA 的 ATR 移动止损拉取守卫

    // ── 日志 ──
    QPlainTextEdit* logBox_ = nullptr;   // 上限 kLogMaxLines 行，超出自动丢最早的
};

} // namespace ccg
