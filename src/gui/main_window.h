#pragma once
#include <QMainWindow>
#include <QTableWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
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
#include "core/thread_pool.h"
#include "net/trading_client.h"
#include "net/book_ticker_stream.h"

namespace ccg {

using namespace ccbot;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

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
    void refreshStats();
    void openTradeHistoryDialog();
    // 品种精度信息缓存未命中时，去后台线程取一次，绝不在 GUI 线程同步阻塞等待
    void ensureSymbolInfoAsync(const std::string& symbol);

    // 持久化
    std::string cred_path()     const;
    std::string bot_cfg_path()  const;
    std::string trade_path()   const;
    std::string settings_path() const;
    std::string log_path()      const;
    void save_credentials();
    void load_credentials();
    void save_bots();
    void load_and_restore_bots();
    void save_trades();
    void load_trades();
    void save_settings();
    void load_settings();

    // 关键事件外部提醒（Telegram/企业微信/飞书 webhook 等），后台线程发送，不阻塞 GUI
    void sendAlert(const QString& text);

    // 后端
    std::shared_ptr<TradingClient>     client_;
    std::shared_ptr<CcgEngine>         engine_;
    std::shared_ptr<ThreadPool>        pool_;
    std::unique_ptr<BookTickerStream>  ticker_;

    QTimer* tick_timer_  = nullptr;
    QTimer* ob_timer_    = nullptr;
    QTimer* header_timer_ = nullptr;   // 呼吸灯 + 连接计时，高频刷新

    // ── 连接区 ──（API Key/Secret/测试网的输入控件挪进了"设置"弹窗，这里只存值）
    QString      apiKey_;
    QString      apiSecret_;
    bool         testnet_ = false;
    QPushButton* btnConnect_;
    QLabel*      connLabel_;
    QLabel*      breatheDot_  = nullptr;   // 连接状态呼吸灯
    QLabel*      equityLabel_ = nullptr;   // 权益/可用，顶部常驻
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

    // ── 交易明细 / 盈利统计 ──
    std::vector<TradeRecord> trades_;
    QLabel*       statsLabel_ = nullptr;

    // symbol + "_L"/"_S" → 交易所真实持仓（强平价来源，随 tick_timer_ 每 3s 刷新一次）
    std::unordered_map<std::string, TradingClient::Position> pos_cache_;

    // 正在后台预取品种精度信息的品种集合，避免同一品种被重复发起请求（仅 GUI 线程访问）
    std::set<std::string> pendingSymbolFetch_;

    // ── 日志 ──
    QTextEdit* logBox_ = nullptr;
};

} // namespace ccg
