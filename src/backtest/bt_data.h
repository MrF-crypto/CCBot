#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// 回测数据层：CSV 解析 → 质检 → 二进制缓存。
// 数据格式（自建数据集 crypto-binance-swap-candle-csv-1m-2026）：
//   第1行 = 版权说明（跳过），第2行 = 表头，之后每行一根 1m K线
//   candle_begin_time,open,high,low,close,volume,quote_volume,trade_num,
//   taker_buy_base_asset_volume,taker_buy_quote_asset_volume,Spread,symbol,fundingRate
//   时间为 UTC（已用资金费率结算时刻 00/08/16 整点验证）
namespace ccbot::bt {

// 一根 1m K线（回测内部表示）。48字节，一年52.6万根≈25MB/品种
struct Bar {
    int64_t ts_ms      = 0;   // openTime, UTC毫秒
    double  open       = 0;
    double  high       = 0;
    double  low        = 0;
    double  close      = 0;
    double  volume     = 0;   // 币量
    double  quote_vol  = 0;   // 成交额（SR雷达的POC用它，跨品种可比）
    double  spread     = 0;   // 该分钟买卖价差【比例值】（0.00038 = 3.8bp）。
                              // 滑点模型用它的真实值，不再拍脑袋固定2bp
    double  funding    = 0;   // 资金费率，非0=该时刻结算（每8小时一次）
};

struct Series {
    std::string       symbol;   // 规范化后的ID，如 BTCUSDT
    std::vector<Bar>  bars;

    bool empty() const { return bars.empty(); }
    size_t size() const { return bars.size(); }
};

// 质检报告：回测结论必须能追溯到数据版本
struct QualityReport {
    std::string symbol;
    size_t  total_bars   = 0;
    int64_t first_ts     = 0;
    int64_t last_ts      = 0;
    size_t  gap_count    = 0;    // 缺口段数（相邻bar间隔 > 1分钟）
    size_t  gap_minutes  = 0;    // 缺失的总分钟数
    size_t  bad_ohlc     = 0;    // high<low / 非正价格等坏点（已剔除）
    size_t  dup_ts       = 0;    // 重复时间戳（已剔除）
    size_t  funding_pts  = 0;    // 资金费率结算点数
    std::vector<std::pair<int64_t,int64_t>> largest_gaps;  // 最大的几个缺口 {起,止}

    std::string to_text() const;
};

// 文件名 → 规范化符号：BTC-USDT.csv → BTCUSDT；AAVEUSDC.csv → AAVEUSDC
std::string normalize_symbol(const std::string& filename);

// 解析 CSV（跳过版权首行，容忍非补零时间格式 2026-01-01 00:00:00）
// 失败返回 false 并写 err
bool load_csv(const std::string& path, Series& out, QualityReport& rep, std::string& err);

// 合并多个 CSV（数据按年分目录时用）：按时间排序拼接、跨文件去重，
// 返回合并后的连续序列。任一文件失败即整体失败
bool load_csv_multi(const std::vector<std::string>& paths, Series& out,
                    QualityReport& rep, std::string& err);

// ── 流式读取器：全市场回测用 ─────────────────────────────────────────────────
// 596 品种全量载入需 36GB 内存，装不下。改为每品种只在内存保留一个小缓冲区
// （默认8192根≈600KB），读完自动从磁盘续填——内存占用与品种数线性但系数极小
class BarStream {
public:
    bool open(const std::string& symbol, std::vector<std::string> paths);
    // 取下一根；无更多数据返回 false
    bool next(Bar& out);
    // 预览下一根的时间戳（不消费）；无数据返回 0
    int64_t peek_ts();
    const std::string& symbol() const { return symbol_; }

private:
    bool refill();

    std::string symbol_;
    std::vector<std::string> paths_;
    size_t file_idx_ = 0;
    std::shared_ptr<void> fh_;       // ifstream（避免头文件引入 <fstream>）
    std::vector<Bar> buf_;
    size_t pos_ = 0;
    int64_t last_ts_ = 0;
    bool header_done_ = false;
    // 列下标（按表头定位，不写死顺序）
    int i_ts_=-1, i_o_=-1, i_h_=-1, i_l_=-1, i_c_=-1, i_v_=-1, i_qv_=-1, i_sp_=-1, i_fr_=-1;
};

// 二进制缓存：加载一次CSV要几秒，缓存后毫秒级（参数网格要反复读同一份数据）
bool save_cache(const std::string& path, const Series& s);
bool load_cache(const std::string& path, Series& out);

// 从1m聚合到更大周期（1h/4h/1d）。align_ms=周期毫秒数，按UTC自然边界对齐。
// 只输出【已收盘】的完整bar；末尾未完成的部分由回放器单独处理（实时未收盘bar）
std::vector<Bar> aggregate(const std::vector<Bar>& src, int64_t period_ms);

// 周期字符串 → 毫秒（"1m"/"5m"/"1h"/"4h"/"1d"）
int64_t interval_ms(const std::string& s);

// UTC毫秒 → "YYYY-MM-DD HH:MM:SS"
std::string ts_to_str(int64_t ms);

} // namespace ccbot::bt
