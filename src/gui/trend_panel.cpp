// MainWindow 的趋势策略部分：行序、行填充、配置表单、落地、持久化。
//
// 单独一个翻译单元，不塞进 main_window.cpp——那个文件已经 3800+ 行，
// 而且是本项目历史上几乎每一个 GUI bug 的出处。
//
// 表单是三个策略（海龟 / 趋势SAR / 纯裸K）共用一张：品种、周期、仓位、反手、
// 进程外保护是共享的，只有【入场信号与止损算法】那一段随策略换页。
// 换页而不是灰掉：三套参数不混搭，灰掉的控件仍在视野里会让人以为调了有用。
//
// 落盘文件名仍是 sar_bots.json（v4.x 留下的），改名会让升级的人丢掉全部配置。
#include "gui/main_window.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ccg {

namespace {

QTableWidgetItem* mk(const QString& text, const QString& color = QString()) {
    auto* it = new QTableWidgetItem(text);
    it->setTextAlignment(Qt::AlignCenter);
    if (!color.isEmpty()) it->setForeground(QColor(color));
    return it;
}

QString fmt_px(double v) {
    if (v <= 0) return "—";
    // 价格数量级差 6 个量级（BTC 十万 vs 某些山寨 0.00001），固定小数位
    // 要么把大价格挤爆要么把小价格显示成 0
    const int prec = (v >= 1000) ? 2 : (v >= 1) ? 4 : (v >= 0.01) ? 6 : 8;
    return QString::number(v, 'f', prec);
}

// 两位小数带正负号，用于盈亏列
QString fmt_signed(double v) {
    return QString("%1%2").arg(v >= 0 ? "+" : "").arg(v, 0, 'f', 2);
}

const char* kGreen = "#3fb950";
const char* kRed   = "#f85149";
const char* kGrey  = "#8b949e";
const char* kAmber = "#d29922";
const char* kBlue  = "#58a6ff";

QString pnl_color(double v) {
    return v > 0 ? kGreen : v < 0 ? kRed : kGrey;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 行填充
// ─────────────────────────────────────────────────────────────────────────────
// 行序由调用方（refreshBotTable）按品种字典序排好。v4.5.0~v4.7.1 这里还有一个
// buildRows：把网格DCA 与趋势SAR 两个引擎的 bot 合成一个带 Kind 判别和跨策略
// 排序键的行序列。只剩一套策略之后，"行序"就是品种字典序，那套机制整个没了。
//
// 16 列的列位沿用当年与 DCA 共表时的排布（列头文案已按趋势策略改过）。
// 对照表（6/7/8 由调用方统一填好，这里不碰）：
//
//   列          DCA 的含义        SAR 放什么            同义?
//   ──────────────────────────────────────────────────────────────────────────
//   2 方向      配置(多/空/双向)  运行时持仓方向        ✗ 语义不同
//   3 策略      加仓曲线          SAR·模式名            ✗ 改为"策略·参数"
//   4 层进度    已开层/总层       金字塔档数            ≈
//   5 均价      持仓均价          开仓价                ✓
//   9 浮动P&L   同                同                    ✓
//  10 保证金    同                同                    ✓
//  11 收益率    同                同                    ✓
//  12 强平/止损 强平价            追踪止损线            ✗ 列头已改名
//  13 已实现    同                同                    ✓
//  14 状态      同                同                    ✓
//
// SAR 独有的 ATR 数据灯、胜率、连续反手、决策文字没有对应列位，全部收进悬停：
// ATR 灯 → 状态列，胜率与反手 → 已实现列，决策文字 → 状态列。
void MainWindow::fillTrendRow(int row, const TrendBot& b, RowTotals& t) {
    const bool is_long = (b.st.pos == trend::Pos::Long);
    const bool has_pos = (b.st.pos != trend::Pos::Flat && b.qty > 0);
    const bool stopped = (b.state == TrendBot::State::Stopped);

    double upnl = 0;
    if (has_pos && b.current_price > 0)
        upnl = (b.current_price - b.st.entry_price) * b.qty * (is_long ? 1.0 : -1.0);

    // 汇总。DCA 那边的三分法是 运行中/冷却中/已停止，这里对齐同一口径：
    // SAR 的 State 只有 Running/Stopped，"冷却"体现在 cooldown_left 上
    t.unreal += upnl;
    t.real   += b.realized_pnl;
    if (stopped)                     ++t.stopped;
    else if (b.st.cooldown_left > 0) ++t.cooling;
    else                             ++t.running;

    // ── 0 # ──
    botTable_->setItem(row, ColIdx, mk(QString::number(row + 1), kGrey));

    // ── 1 品种 ──
    botTable_->setItem(row, ColSym, mk(QString::fromStdString(b.cfg.symbol)));

    // ── 2 方向 ──
    // ⚠ 与 DCA 不同义。DCA 那列是【配置】（你选的多/空/双向），SAR 的配置里
    //   根本没有方向——两个方向都由信号决定，所以这里只能显示【当前持仓方向】。
    //   空仓时显示"空仓"而不是"—"，好让这个差别在界面上看得出来
    auto* dir_it = mk(has_pos ? (is_long ? "多" : "空") : "空仓",
                      has_pos ? (is_long ? kGreen : kRed) : kGrey);
    dir_it->setToolTip("趋势 SAR 没有方向配置：上破做多、下破做空由信号决定。\n"
                       "这一格显示的是【当前持仓方向】，不是配置。");
    botTable_->setItem(row, ColDir, dir_it);

    // ── 3 策略 ──
    // ── 周期·模式 ──
    // 周期必须在表上可见：1m 和 4h 的行为差异巨大，而它此前只能进弹窗才看得到。
    // 模式名单独占一格信息量太小，合成 "3m·裸K" / "4h·通道" 更划算
    const auto strat   = b.cfg.rule.strategy;
    const bool bar_mode = (strat == trend::Strategy::BareK);
    const char* short_name = (strat == trend::Strategy::Turtle)       ? "·海龟"
                           : (strat == trend::Strategy::ParabolicSar) ? "·SAR"
                                                                      : "·裸K";
    auto* strat_it = mk(QString::fromStdString(b.cfg.interval) + short_name, kBlue);
    {
        QString tip = QString("%1　信号周期 %2\n")
                          .arg(trend::strategy_name(strat))
                          .arg(QString::fromStdString(b.cfg.interval));
        switch (strat) {
        case trend::Strategy::Turtle:
            tip += QString("唐奇安突破入场 + Chandelier ATR 追踪止损\n"
                           "通道周期 %1 / ATR 周期 %2 / k=%3\n")
                       .arg(b.cfg.rule.donchian_period)
                       .arg(b.cfg.rule.atr_period)
                       .arg(b.cfg.rule.atr_mult, 0, 'f', 1);
            break;
        case trend::Strategy::ParabolicSar:
            tip += QString("抛物线 SAR：SAR 值既是入场也是止损也是反转点\n"
                           "AF %1 → %2，步长 %3（每刷新一次极值就加速一档）\n"
                           "趋势走得越久 SAR 贴得越紧，这是它防利润回吐的机制\n")
                       .arg(b.cfg.rule.af_start, 0, 'f', 3)
                       .arg(b.cfg.rule.af_max,   0, 'f', 3)
                       .arg(b.cfg.rule.af_step,  0, 'f', 3);
            break;
        case trend::Strategy::BareK:
            tip += QString("入场：%1\n摆动止损根数 N=%2（信号根不算在内）\n每根K线最多开一次：%3\n")
                       .arg(b.cfg.rule.bare_entry == trend::BareEntry::Immediate
                                ? "盘中即时（实时价 vs 本根开盘价）"
                                : "等收盘突破上一根高/低点")
                       .arg(b.cfg.rule.swing_bars)
                       .arg(b.cfg.rule.once_per_bar ? "开" : "关");
            break;
        }
        tip += (b.cfg.size_mode == TrendConfig::SizeMode::RiskBased)
                   ? QString("仓位：固定单笔风险，单笔愿亏 %1U（名义上限 %2U）\n")
                         .arg(b.cfg.risk_usdt, 0, 'f', 2).arg(b.cfg.budget_usdt, 0, 'f', 0)
                   : QString("仓位：固定名义 %1U\n").arg(b.cfg.budget_usdt, 0, 'f', 0);
        tip += QString("杠杆 %1x　反手：%2　新鲜度上限 %3 秒")
                   .arg(b.cfg.leverage)
                   .arg(b.cfg.rule.reverse == trend::ReverseMode::Immediate
                            ? "立即反手" : "不反手（回到正常入场）")
                   .arg(b.cfg.effective_max_age_sec());
        strat_it->setToolTip(tip);
    }
    botTable_->setItem(row, ColMode, strat_it);

    // ── 委托止损（交易所侧那张"保命单"）──
    // 这一格回答的是【我的保命单还在不在】。使用者把"哪怕断电断网也要有底"
    // 列成了硬要求，那它在不在就必须一眼可见，不能只躺在日志里。
    //
    // v5.0.1 之前这个位置是"层进度"（DCA 的已开层/总层，复用给金字塔档数）。
    // 金字塔默认是关的，所以那一格大部分时候是空的；而"层"本身是摊薄的词汇
    QString hs_s = "—", hs_c = kGrey, hs_tip;
    if (!b.cfg.use_disaster_stop) {
        hs_tip = "未开启委托止损（配置里的 use_disaster_stop）。\n\n"
                 "⚠ 不开的话，守着这个仓位的就只有本地移动止损——\n"
                 "程序崩溃、断电、断网之后它一点都不剩，仓位完全裸奔。";
    } else if (stopped && !has_pos && b.ds_fail_closes >= 2) {
        // 熔断：连续两次因为挂不上而平仓，已经判定为系统性故障
        hs_s = "已熔断"; hs_c = kRed;
        hs_tip = QString("已连续 %1 次因挂不上委托止损而平仓，判定为系统性故障，"
                         "该 bot 已停止。\n\n"
                         "继续重试只会不断支付开平手续费。请检查：\n"
                         "  · 账户是否有下条件单的权限\n"
                         "  · 该品种是否支持 closePosition\n"
                         "  · 止损触发价的精度与方向")
                     .arg(b.ds_fail_closes);
    } else if (!has_pos) {
        hs_tip = "空仓时没有委托止损——没有仓位就没有可平的。\n"
                 "开仓成交后会立即挂一张，挂上之后【永不移动】。";
    } else if (!b.disaster_stop_id.empty()) {
        hs_s = fmt_px(b.disaster_stop_price); hs_c = kGreen;
        hs_tip = QString("✓ 已挂在交易所：触发价 %1（单号 %2）\n"
                         "开仓时挂一次，此后【不随移动止损走】——它的职责是"
                         "最大风险兜底，不是第二条移动止损。\n\n"
                         "进程崩溃、断电、断网之后它依然有效。\n"
                         "触发用【标记价】，所以比移动止损外扩了 %3%%，"
                         "让本地先触发、交易所只在进程真的不在时才兜底。")
                     .arg(hs_s).arg(QString::fromStdString(b.disaster_stop_id))
                     .arg(b.cfg.disaster_stop_buffer_pct, 0, 'f', 2);
    } else if (b.ds_unprotected) {
        hs_s = "⚠ 无保护"; hs_c = kRed;
        hs_tip = QString("⚠⚠ 已连续 %1 次挂不上委托止损。\n"
                         "此刻这个仓位【没有任何进程外保护】——本地移动止损仍在"
                         "工作，但程序一旦不在，仓位就没有底了。\n\n"
                         "再失败几次就会【立即平掉该仓位】：开仓的前提是"
                         "「断电断网也有交易所侧的底」，前提不成立就不该继续持有。")
                     .arg(b.ds_attempts);
    } else if (b.ds_attempts > 0) {
        hs_s = QString("重试 %1/13").arg(b.ds_attempts); hs_c = kAmber;
        hs_tip = QString("委托止损挂单失败 %1 次，正在按退避重试"
                         "（0.5秒×3 → 2秒×7 → 3秒×3，约 25 秒跑完）。\n\n"
                         "此刻该仓位没有进程外保护，但本地移动止损照常工作。")
                     .arg(b.ds_attempts);
    } else {
        hs_s = "挂单中…"; hs_c = kAmber;
        hs_tip = "刚开仓，正在挂委托止损。\n"
                 "顺序只能是「先开仓再挂止损」——closePosition 单在没有仓位时"
                 "会被交易所拒，所以这个窗口是结构性的，只能缩到最短。";
    }
    auto* hs_it = mk(hs_s, hs_c);
    hs_it->setToolTip(hs_tip);
    botTable_->setItem(row, ColHardStop, hs_it);

    // ── 开仓价 ──（金字塔档数从原「层进度」列挪进这里的悬停）
    auto* entry_it = mk(has_pos ? fmt_px(b.st.entry_price) : "—");
    if (has_pos) {
        // 金字塔移除后这里永远是单笔成交价，不再是加权均价
        entry_it->setToolTip("开仓价 —— 本轮建仓的成交价。\n"
                             "三个策略都不加仓，所以一笔仓位从开到平数量不变。");
    }
    botTable_->setItem(row, ColEntry, entry_it);

    // ── 9 浮动P&L ──
    botTable_->setItem(row, ColUnreal,
        mk(has_pos ? fmt_signed(upnl) : "—", pnl_color(upnl)));

    // ── 10 保证金 / 11 收益率 ──
    double margin = 0, roe = 0;
    if (has_pos && b.current_price > 0 && b.cfg.leverage > 0) {
        margin = b.current_price * b.qty / b.cfg.leverage;
        if (margin > 0) roe = upnl / margin * 100.0;
    }
    botTable_->setItem(row, ColMargin,
        mk(margin > 0 ? QString("$%1").arg(margin, 0, 'f', 2) : "—", kGrey));
    auto* roe_it = mk(margin > 0 ? QString("%1%").arg(roe, 0, 'f', 2) : "—",
                      pnl_color(margin > 0 ? roe : 0));
    if (margin > 0)
        roe_it->setToolTip(QString("浮动盈亏 $%1 / 保证金 $%2（已按 %3x 杠杆放大）\n\n"
                                   "趋势 SAR 没有固定止盈：收益全来自少数跑得很远的单子，\n"
                                   "所以这个数没有「该止盈了」的阈值，只看止损线跟到哪。")
                              .arg(upnl, 0, 'f', 2).arg(margin, 0, 'f', 2)
                              .arg(b.cfg.leverage));
    botTable_->setItem(row, ColRoe, roe_it);

    // ── 移动止损（本地棘轮线）──
    // 持仓时最该盯的一格。显示 价格(距现价%)：价格用来对照K线图，
    // 而【距离才是可操作的信息】——一眼知道还有多少余地。
    // 线越过成本价 = 这笔已锁定盈利，转绿
    QString stop_s = "—";
    QString stop_c = kAmber;
    QString stop_tip;
    if (has_pos && b.st.stop > 0) {
        const bool locked = is_long ? (b.st.stop >= b.st.entry_price)
                                    : (b.st.stop <= b.st.entry_price);
        stop_s = fmt_px(b.st.stop);
        stop_c = locked ? kGreen : kAmber;
        stop_tip = QString("移动止损线 %1（棘轮，只朝有利方向移动）\n").arg(stop_s);
        if (b.current_price > 0) {
            const double d = std::fabs(b.current_price - b.st.stop) / b.current_price * 100.0;
            stop_s += QString(" (%1%)").arg(d, 0, 'f', 1);
            stop_tip += QString("距现价 %1%　—— 现在被打掉就按这条线出场\n")
                            .arg(d, 0, 'f', 2);
        }
        stop_tip += locked
            ? "✓ 线已越过成本价：这笔已锁定盈利，最坏情况也是赚\n"
            : "线还在成本价的亏损侧：被打掉是亏损出场，可能触发反手\n";
        stop_tip += "\n⚠ 这条线活在本进程里。程序不在了，守着仓位的就只剩"
                    "「委托止损」那一列的交易所单。";
    } else if (!has_pos) {
        stop_tip = "空仓时没有止损线——没有仓位就没有保护。";
    }
    auto* stop_it = mk(stop_s, stop_c);
    stop_it->setToolTip(stop_tip);
    botTable_->setItem(row, ColTrailStop, stop_it);

    // ── 已实现（悬停带胜率与连续反手）──
    auto* real_it = mk(fmt_signed(b.realized_pnl), pnl_color(b.realized_pnl));
    {
        QString tip = QString("已实现盈亏 %1U\n").arg(fmt_signed(b.realized_pnl));
        // 胜率：趋势跟随天然只有 30~40%，低不代表策略坏了。分子分母都摆出来，
        // 免得只看一个百分比就急着改参数
        if (b.trade_count > 0)
            tip += QString("胜率 %1/%2（%3%）—— 趋势跟随天然只有 30~40%，"
                           "低不代表策略坏了\n")
                       .arg(b.win_count).arg(b.trade_count)
                       .arg(100.0 * b.win_count / b.trade_count, 0, 'f', 0);
        else
            tip += "尚无成交\n";
        tip += QString("连续反手 %1/%2")
                   .arg(b.st.consec_reverses).arg(b.cfg.rule.max_consecutive_reverses);
        if (b.st.consec_reverses >= b.cfg.rule.max_consecutive_reverses &&
            b.cfg.rule.max_consecutive_reverses > 0)
            tip += "（已到上限，下次亏损出场将转冷却）";
        else if (b.st.consec_reverses >= 2)
            tip += "　⚠ 连续反手本身就是「现在是震荡市」的信号";
        real_it->setToolTip(tip);
    }
    botTable_->setItem(row, ColReal, real_it);

    // ── 状态（悬停带决策文字 + ATR 数据灯）──
    // 空仓时直接显示【为什么不开仓】的短标签。
    // "信号来了怎么没开" 是最常见的疑问，而答案此前只埋在悬停的决策文字里。
    // 账户级闸门的拦截原因由引擎写进 last_decision，这里把它提成可见标签
    QString st_s;
    if (stopped)            st_s = "已停止";
    else if (b.pending)     st_s = "下单中";
    else if (has_pos)       st_s = "持仓中";
    else if (b.st.cooldown_left > 0)
        st_s = QString("冷却%1根").arg(b.st.cooldown_left);
    else {
        // 引擎把闸门拦截写成 "空仓 | 账户总保证金将达…" / "…品种数已达上限…"
        const QString dec = QString::fromStdString(b.last_decision);
        if (dec.contains("账户总保证金"))   st_s = "闸门·保证金";
        else if (dec.contains("品种数"))    st_s = "闸门·品种数";
        else if (dec.contains("信号过期"))  st_s = "信号过期";
        else                                st_s = "等信号";
    }
    // 被闸门拦住不是"正常等待"，标琥珀让它和"等信号"区分开
    const bool gated = st_s.startsWith("闸门") || st_s == "信号过期";
    QString st_c = stopped ? kRed : has_pos ? kBlue : gated ? kAmber : kGrey;

    // ATR 数据灯：原先是独立一列，合表后并进状态列。
    // ⚠ 这盏灯的含义【随模式而变】——裸K线模式的止损是摆动低点，与 ATR 无关，
    //   没有 ATR 照样开仓（引擎侧 data_ready 对这个模式只看K线）。原先那一列的
    //   提示写死"没有 ATR 就没有止损线、引擎不会开新仓"，在裸K线模式下是假话
    const bool data_missing = bar_mode ? !b.bar_ok : !(b.atr_pct > 0);
    if (data_missing && !stopped) {
        st_s += " ⚠";
        st_c  = kAmber;
    }
    auto* st_it = mk(st_s, st_c);
    {
        QString tip = QString::fromStdString(b.last_decision);
        if (tip.isEmpty()) tip = "尚未产生决策";
        tip += "\n\n";
        if (!b.last_action.empty())
            tip += QString("最近动作：%1\n").arg(QString::fromStdString(b.last_action));
        if (data_missing) {
            tip += bar_mode
                ? "⚠ 摆动窗口未就绪：K 线还没拉到，算不出止损线，引擎不会开新仓。\n"
                  "（裸K线模式不需要 ATR，所以 ATR 缺失不影响它）\n"
                : "⚠ 尚未拉到 K 线：没有 ATR 就没有止损线，引擎不会开新仓。\n";
            tip += "刚添加的品种最多等一个信号周期；持续如此请看日志里的"
                   "「SAR 信号拉取失败」告警";
        } else if (b.atr_pct > 0) {
            tip += QString("%1 周期 ATR = %2%（跨品种可比口径）")
                       .arg(QString::fromStdString(b.cfg.interval))
                       .arg(b.atr_pct, 0, 'f', 2);
            // k×ATR 只在唐奇安模式下【就是】止损距离；裸K线模式的止损是摆动
            // 低点，把 k×ATR 说成止损距离是假数
            if (!bar_mode)
                tip += QString("\n当前 k=%1 ⇒ 止损距离约 %2%")
                           .arg(b.cfg.rule.atr_mult, 0, 'f', 1)
                           .arg(b.atr_pct * b.cfg.rule.atr_mult, 0, 'f', 1);
        }
        st_it->setToolTip(tip);
    }
    botTable_->setItem(row, ColState, st_it);

    // ── 操作 ──
    // 键没变就不重建控件，否则每 3 秒重建一次会把点击吞掉。
    // 前缀是两套策略同表时代的遗留（那时同一个行号可能从 DCA 换成 SAR，
    // 按钮组要整套重建）。现在只有一个引擎，前缀不再有区分作用，但留着无害，
    // 而改它会让升级后第一次刷新时全部按钮重建一次——没必要
    const QString opKey = QString("sar|%1|%2|%3|%4")
                              .arg(QString::fromStdString(b.bot_id))
                              .arg((int)b.state).arg(has_pos).arg(b.pending);
    if (row < (int)opRowKeys_.size() && opRowKeys_[row] == opKey
        && botTable_->cellWidget(row, ColOps) != nullptr) {
        return;
    }
    if (row < (int)opRowKeys_.size()) opRowKeys_[row] = opKey;

    auto* opw = new QWidget();
    auto* opl = new QHBoxLayout(opw);
    opl->setContentsMargins(3, 1, 3, 1);
    opl->setSpacing(4);
    const std::string id  = b.bot_id;
    const QString     sym = QString::fromStdString(b.cfg.symbol);

    if (!stopped) {
        auto* bp = new QPushButton("停止");
        bp->setFixedHeight(20);
        bp->setStyleSheet("QPushButton{background:#3d1a1a;color:#f85149;"
                          "font-size:11px;padding:0 6px;}");
        connect(bp, &QPushButton::clicked, this, [this, id]() {
            if (!trend_engine_) return;
            trend_engine_->stop_bot(id);
            refreshBotTable();
            save_trend_bots();
        });
        opl->addWidget(bp);
    } else {
        auto* br = new QPushButton("继续");
        br->setFixedHeight(20);
        br->setStyleSheet("QPushButton{background:#1a3d1a;color:#3fb950;"
                          "font-size:11px;padding:0 6px;}");
        connect(br, &QPushButton::clicked, this, [this, id]() {
            if (!trend_engine_) return;
            trend_engine_->resume_bot(id);
            // 「全部停止」会关掉 tick 定时器——单个 bot 恢复时必须把它拉起来，
            // 否则 bot 显示"运行中"但引擎永远不被驱动，止损线永远不会触发
            if (tick_timer_ && !tick_timer_->isActive()) {
                tick_timer_->start();
                log("Tick 定时器已重新启动");
            }
            trendSigForce_.store(true);   // 下一拍立刻拉信号，不干等一个周期
            refreshBotTable();
            save_trend_bots();
        });
        opl->addWidget(br);
    }

    if (has_pos && !b.pending) {
        auto* bc = new QPushButton("平仓");
        bc->setFixedHeight(20);
        bc->setStyleSheet("QPushButton{color:#d29922;font-size:11px;padding:0 6px;}");
        connect(bc, &QPushButton::clicked, this, [this, id, sym]() {
            if (!trend_engine_) return;
            if (!confirmDanger("确认平仓",
                               sym + " 将以市价立即平掉当前 SAR 仓位。", "平仓")) return;
            trend_engine_->close_bot(id);
        });
        opl->addWidget(bc);
    }

    auto* bd = new QPushButton("删除");
    bd->setFixedHeight(20);
    bd->setStyleSheet("QPushButton{color:#f85149;font-size:11px;padding:0 6px;}");
    connect(bd, &QPushButton::clicked, this, [this, id, sym, has_pos]() {
        if (!trend_engine_) return;
        // 有持仓时删除 = 交易所上留下一笔【没有止损线守着】的裸仓位。
        // 必须说清楚，不能只问"确定删除吗"
        const QString body = has_pos
            ? sym + " 当前有持仓。删除后程序不再跟踪它，"
                    "交易所上的仓位会失去追踪止损的保护，需要你手动处理。"
            : sym + " 将从监控列表中移除。";
        if (!confirmDanger("确认删除", body, "删除")) return;
        trend_engine_->remove_bot(id);
        unsubscribeIfUnused(sym.toStdString());
        refreshBotTable();
        save_trend_bots();
    });
    opl->addWidget(bd);
    opl->addStretch(1);
    botTable_->setCellWidget(row, ColOps, opw);
}

// ─────────────────────────────────────────────────────────────────────────────
// SAR 配置表单
// ─────────────────────────────────────────────────────────────────────────────
// 拆成"构建"和"回读"两半，是为了让 openStrategyDialog 能把整张表单嵌进自己的
// 策略分页里，而不必把这 30 多个控件的构造逻辑复制一份。
// 结构体定义留在本 .cpp——头文件里只有前向声明，改一个 spinbox 不会触发全量重编。
struct TrendFormWidgets {
    // ── 共享 ──
    QComboBox*      strategy = nullptr;
    QStackedWidget* pages    = nullptr;   // 三个策略各自的参数页，只显示当前这个
    QComboBox*      interval = nullptr;
    QComboBox*      sizeMode = nullptr;
    QDoubleSpinBox* budget   = nullptr;
    QDoubleSpinBox* risk     = nullptr;
    QSpinBox*       lev      = nullptr;
    QComboBox*      reverse  = nullptr;
    QLabel*         revNote  = nullptr;   // PSAR 被锁成"立即反手"时的说明
    QCheckBox*      disStop  = nullptr;
    QDoubleSpinBox* disBuf   = nullptr;
    QSpinBox*       maxAge   = nullptr;
    QLabel*         maxAgeHint = nullptr;

    // ── ① 海龟 ──
    QSpinBox*       dcPeriod  = nullptr;
    QSpinBox*       atrPeriod = nullptr;
    QDoubleSpinBox* k         = nullptr;

    // ── ② 抛物线 SAR ──
    QDoubleSpinBox* afStart = nullptr;
    QDoubleSpinBox* afStep  = nullptr;
    QDoubleSpinBox* afMax   = nullptr;

    // ── ③ 纯裸K ──
    QComboBox*      bareEntry  = nullptr;
    QCheckBox*      oncePerBar = nullptr;
    QSpinBox*       swing      = nullptr;
    QSpinBox*       maxRev     = nullptr;
    QSpinBox*       cooldown   = nullptr;

    TrendConfig     base;      // 弹窗没有控件的字段从这里继承
};

// 策略 ↔ 分页索引。写成两个函数而不是靠 enum 的整数值，是因为 Strategy 的
// 声明顺序将来可能变，而分页顺序是界面语义
static int strategy_page(trend::Strategy s) {
    switch (s) {
        case trend::Strategy::Turtle:       return 0;
        case trend::Strategy::ParabolicSar: return 1;
        case trend::Strategy::BareK:        return 2;
    }
    return 0;
}

std::shared_ptr<TrendFormWidgets> MainWindow::buildTrendForm(QVBoxLayout* into,
                                                          const TrendConfig& c) {
    auto w = std::make_shared<TrendFormWidgets>();
    w->base = c;

    // 分组工厂。和 DCA 那边同款外观，否则同一个弹窗里切两套策略会像两个程序
    auto mkGroup = [into](const QString& title, const QString& color) {
        auto* box = new QGroupBox(title);
        box->setStyleSheet(QString("QGroupBox{color:%1;font-size:11px;font-weight:bold;"
                                   "border:1px solid #21262d;border-radius:4px;"
                                   "margin-top:8px;padding:10px 10px 6px 10px;}"
                                   "QGroupBox::title{subcontrol-origin:margin;left:8px;"
                                   "padding:0 4px;}").arg(color));
        auto* f = new QFormLayout(box);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        f->setSpacing(6);
        into->addWidget(box);
        return f;
    };

    auto* sigForm  = mkGroup("策略与信号", "#a371f7");
    auto* posForm  = mkGroup("仓位与杠杆", "#58a6ff");
    auto* revForm  = mkGroup("止损后怎么办", "#3fb950");
    auto* riskForm = mkGroup("进程外保护", "#f85149");

    // ── 策略与信号 ──────────────────────────────────────────────────────────
    // 策略排在整张表单最前面：它决定了下面哪一页参数有意义。三套参数【不混搭】，
    // 所以用分页而不是灰掉——灰掉的控件仍然在视野里，会让人以为"调了也有用"
    w->strategy = new QComboBox();
    w->strategy->addItem("① 海龟（唐奇安突破 + ATR 止损）", (int)trend::Strategy::Turtle);
    w->strategy->addItem("② 趋势 SAR（Wilder 抛物线）",     (int)trend::Strategy::ParabolicSar);
    w->strategy->addItem("③ 纯裸K（摆动极值止损）",          (int)trend::Strategy::BareK);
    w->strategy->setCurrentIndex(strategy_page(c.rule.strategy));
    w->strategy->setToolTip(
        "① 海龟：价格创 N 根新高/新低入场，止损 = k×ATR 的棘轮线。\n"
        "   慢、信号少、对参数最不敏感，是三个里最适合 4h 以上周期的。\n\n"
        "② 趋势 SAR：SAR 点既是止损也是反转点，趋势越强它加速贴近价格。\n"
        "   天然 stop-and-reverse（永远有仓位），震荡市会连续翻转——\n"
        "   这是 PSAR 公认的弱点，靠「连续反手上限」之外无解。\n\n"
        "③ 纯裸K：只看开盘/收盘/前高前低，没有任何指标。最快也最吵。");
    sigForm->addRow("策略", w->strategy);

    w->interval = new QComboBox();
    // 只列币安真实支持的 K 线周期。10m 不在其中（币安没有这一档），
    // 最接近的是 5m 和 15m
    w->interval->addItems({"1m", "3m", "5m", "15m", "30m", "1h", "2h", "4h", "12h", "1d"});
    w->interval->setCurrentText(QString::fromStdString(c.interval));
    w->interval->setToolTip("信号K线周期。周期越短信号越多，假突破也越多。\n"
                            "⚠ 1m/3m 上单笔手续费（来回 0.10% + 滑点）会吃掉大半边缘。");
    sigForm->addRow("信号周期", w->interval);

    // 三页参数
    w->pages = new QStackedWidget();
    auto mkPage = [](QStackedWidget* st) {
        auto* page = new QWidget();
        auto* f = new QFormLayout(page);
        f->setContentsMargins(0, 0, 0, 0);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        f->setSpacing(6);
        st->addWidget(page);
        return f;
    };
    auto* p1 = mkPage(w->pages);   // 海龟
    auto* p2 = mkPage(w->pages);   // SAR
    auto* p3 = mkPage(w->pages);   // 裸K

    // ── ① 海龟 ──
    w->dcPeriod = new QSpinBox();
    w->dcPeriod->setRange(2, 200);
    w->dcPeriod->setValue(c.rule.donchian_period);
    w->dcPeriod->setToolTip("唐奇安通道周期（海龟原版 20）。\n"
                            "上破 N 根最高 → 做多，下破 N 根最低 → 做空。\n"
                            "通道只由已收盘K线构成，当前这根去撞它。");
    p1->addRow("通道周期", w->dcPeriod);

    w->atrPeriod = new QSpinBox();
    w->atrPeriod->setRange(2, 100);
    w->atrPeriod->setValue(c.rule.atr_period);
    w->atrPeriod->setToolTip("ATR 周期（海龟原版 14/20）。只有本策略用 ATR。");
    p1->addRow("ATR 周期", w->atrPeriod);

    w->k = new QDoubleSpinBox();
    w->k->setRange(0.5, 20.0);
    w->k->setSingleStep(0.5);
    w->k->setDecimals(1);
    w->k->setValue(c.rule.atr_mult);
    w->k->setToolTip("止损距离 = k × ATR（Chandelier Exit）。\n"
                     "棘轮，只朝有利方向移动，绝不回退。\n"
                     "k 越小假突破越多，越大回吐越多。经典值 2.5~3.5。\n"
                     "表格「状态」列悬停可看当前 k 对应的实际止损距离%。");
    p1->addRow("ATR 倍数 k", w->k);

    // ── ② 抛物线 SAR ──
    w->afStart = new QDoubleSpinBox();
    w->afStart->setRange(0.001, 0.5);
    w->afStart->setSingleStep(0.005);
    w->afStart->setDecimals(3);
    w->afStart->setValue(c.rule.af_start);
    w->afStart->setToolTip(
        "加速因子初值（Wilder 原版 0.02）。\n"
        "刚开仓时 SAR 每根只朝极值点靠拢 2%，给趋势留出呼吸空间。\n"
        "调大 = 一开仓就贴得紧 = 更容易被正常回调扫掉。");
    p2->addRow("AF 初值", w->afStart);

    w->afStep = new QDoubleSpinBox();
    w->afStep->setRange(0.001, 0.5);
    w->afStep->setSingleStep(0.005);
    w->afStep->setDecimals(3);
    w->afStep->setValue(c.rule.af_step);
    w->afStep->setToolTip(
        "每当刷新极值点 EP（多头创新高 / 空头创新低）时，AF 加这么多。\n"
        "⚠ 只在【刷新 EP】那一根加，不是每根都加——这是 Wilder 原版的定义，\n"
        "也是「趋势走得越强 SAR 收得越快」的全部来源。横盘时 AF 不动。");
    p2->addRow("AF 步长", w->afStep);

    w->afMax = new QDoubleSpinBox();
    w->afMax->setRange(0.01, 1.0);
    w->afMax->setSingleStep(0.02);
    w->afMax->setDecimals(3);
    w->afMax->setValue(c.rule.af_max);
    w->afMax->setToolTip(
        "AF 上限（Wilder 原版 0.20）。\n"
        "封顶之后 SAR 每根固定吃掉「当前距极值点」的 20%——这就是它\n"
        "「防止利润回吐太多」的机制：趋势越久，止损贴得越近。");
    p2->addRow("AF 上限", w->afMax);
    {
        auto* n = new QLabel(
            "SAR 值同时是止损线和反转点：价格一触及就平仓并【立即】反向开仓，"
            "所以这个策略永远持有一个方向的仓位，没有空仓期。震荡市里它会连续"
            "翻转——这是 PSAR 的固有弱点，不是实现缺陷。");
        n->setWordWrap(true);
        n->setStyleSheet("color:#8b949e;font-size:11px;");
        p2->addRow(n);
    }

    // ── ③ 纯裸K ──
    w->bareEntry = new QComboBox();
    w->bareEntry->addItem("立即顺势（实时价 vs 本根开盘价）", (int)trend::BareEntry::Immediate);
    w->bareEntry->addItem("等收盘突破前一根高/低",           (int)trend::BareEntry::BreakPrevBar);
    w->bareEntry->setCurrentIndex(c.rule.bare_entry == trend::BareEntry::Immediate ? 0 : 1);
    w->bareEntry->setToolTip(
        "立即顺势：实时价 > 本根开盘价 ⇒ 做多，< ⇒ 做空。不等收盘。\n"
        "  ⚠ 新K线刚开盘时价格≈开盘价，微小波动会让方向反复翻转。\n"
        "    务必配下面的「每根K线最多开一次」，否则一根K线内来回开平数次。\n\n"
        "等收盘突破：本根收盘价 > 上一根最高价 ⇒ 做多，< 上一根最低价 ⇒ 做空。\n"
        "  严格得多——下跌趋势里的一根阳线只是噪音，突破前高才是动能。");
    p3->addRow("入场模式", w->bareEntry);

    w->oncePerBar = new QCheckBox("每根K线最多开一次（配「立即顺势」时强烈建议）");
    w->oncePerBar->setChecked(c.rule.once_per_bar);
    w->oncePerBar->setToolTip(
        "同一根K线里被止损出场后不再重新入场，等下一根。\n"
        "默认【关】，因为「等收盘突破」模式本来一根最多触发一次，不需要它。");
    p3->addRow(w->oncePerBar);

    w->swing = new QSpinBox();
    w->swing->setRange(1, 50);
    w->swing->setValue(c.rule.swing_bars);
    w->swing->setToolTip(
        "止损用最近几根的最低/最高价。信号根【不算】在内。\n\n"
        "⚠ 这个数直接决定持仓时长和交易频率：随机游走下平均持仓约 N+1 根。\n"
        "N=3 ⇒ 4 根就被打掉一次。配短周期时交易次数会非常高。");
    p3->addRow("摆动止损根数", w->swing);

    w->maxRev = new QSpinBox();
    w->maxRev->setRange(0, 20);
    w->maxRev->setValue(c.rule.max_consecutive_reverses);
    w->maxRev->setSpecialValueText("不限");
    w->maxRev->setToolTip("连续反手上限，超过即强制冷却。0 = 不限（默认）。\n"
                          "真趋势不需要连续反手——连续反手本身就是"
                          "「现在是震荡市」的信号。");
    p3->addRow("连续反手上限", w->maxRev);

    w->cooldown = new QSpinBox();
    w->cooldown->setRange(0, 100);
    w->cooldown->setValue(c.rule.cooldown_bars);
    w->cooldown->setSpecialValueText("不冷却");
    w->cooldown->setToolTip("触顶后冷却多少根【K线】（不是 tick）。0 = 不冷却（默认）。");
    p3->addRow("冷却K线数", w->cooldown);

    sigForm->addRow(w->pages);

    auto* note = new QLabel(
        "三个策略都没有固定止盈，这是设计而非遗漏：趋势跟随胜率天然只有 30~40%，"
        "收益全来自少数几笔跑得很远的单子。固定止盈会砍断它们，"
        "而亏损笔大小不变——等于单方面砍掉盈利分布的右尾。");
    note->setStyleSheet("color:#8b949e;font-size:11px;");
    note->setWordWrap(true);
    sigForm->addRow(note);

    // ── 仓位与杠杆 ──────────────────────────────────────────────────────────
    // ⚠ 仓位算法排在最前面：它决定了下面两个框各自的含义（名义价值 vs 名义上限），
    //   放在后面的话用户会先填完再发现填错了地方
    w->sizeMode = new QComboBox();
    w->sizeMode->addItem("固定名义", (int)TrendConfig::SizeMode::Notional);
    w->sizeMode->addItem("固定单笔风险", (int)TrendConfig::SizeMode::RiskBased);
    w->sizeMode->setCurrentIndex(c.size_mode == TrendConfig::SizeMode::RiskBased ? 1 : 0);
    w->sizeMode->setToolTip(
        "固定名义：每笔都是同样的名义价值。\n"
        "固定单笔风险：名义 = 单次愿亏 ÷ 入场价到止损线的距离%，每笔止损亏的钱一样。\n\n"
        "⚠ 用的是【当前策略算出来的那条止损线】，不是某个固定的 ATR 倍数：\n"
        "  海龟是 k×ATR，SAR 是首根 SAR 值，裸K 是最近 N 根的摆动极值。\n"
        "  三个策略都适用，所以这一项放在共享区而不是某一页里。\n\n"
        "为什么需要它：同一份名义，在 4h 波动 1.6% 的 LTC 和 5.3% 的 COTI 上，"
        "单次止损亏的钱差 3.3 倍。固定名义 = 风险全压在高波动那几个品种上，"
        "而「铺开品种分散风险」就此失效。这是海龟的「单位」概念。");
    posForm->addRow("仓位算法", w->sizeMode);

    w->budget = new QDoubleSpinBox();
    w->budget->setRange(10, 10'000'000);
    w->budget->setDecimals(2);
    w->budget->setValue(c.budget_usdt);
    w->budget->setToolTip("固定名义模式：每笔仓位的名义价值。\n"
                          "固定单笔风险模式：名义价值的【上限】"
                          "（止损距离极小时兜住公式算出的天量仓位）。");
    auto* budgetLabel = new QLabel();
    posForm->addRow(budgetLabel, w->budget);

    w->risk = new QDoubleSpinBox();
    w->risk->setRange(0, 1'000'000);
    w->risk->setDecimals(2);
    w->risk->setValue(c.risk_usdt);
    // 值为 0（= 最小值）时显示这行字而不是"0.00"。此前它是一个灰掉的"0"，
    // 看起来像"这个功能坏了"，而真实原因只是仓位算法还没切过去
    w->risk->setSpecialValueText("未设置");
    w->risk->setToolTip("单次止损愿意亏多少钱（USDT）。仅「固定单笔风险」模式使用。\n"
                        "账户 10000U、每次探测愿亏 1% ⇒ 填 100。");
    auto* riskLabel = new QLabel();
    posForm->addRow(riskLabel, w->risk);

    // 两个框的标签和可用性都跟着算法走。写成 lambda 是因为初始化和切换时
    // 要做完全相同的事——分开写必然有一天只改一处
    auto* riskEdit = w->risk;
    auto* budgetBox = w->budget;
    auto syncSizeMode = [budgetBox, budgetLabel, riskEdit, riskLabel](int idx) {
        const bool risk = (idx == 1);
        budgetLabel->setText(risk ? "　名义上限 (USDT)" : "　仓位名义价值 (USDT)");
        riskLabel->setText(risk ? "　单次愿亏 (USDT)"
                                : "　单次愿亏（切到「固定单笔风险」后可填）");
        riskLabel->setEnabled(risk);
        riskEdit->setEnabled(risk);
        (void)budgetBox;
    };
    syncSizeMode(w->sizeMode->currentIndex());
    connect(w->sizeMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            riskEdit, [syncSizeMode](int i) { syncSizeMode(i); });

    w->lev = new QSpinBox();
    w->lev->setRange(1, 125);
    w->lev->setValue(c.leverage);
    posForm->addRow("杠杆", w->lev);

    // ── 止损后怎么办 ────────────────────────────────────────────────────────
    // v5.1 之前这里是两个 bool（allow_reverse + reverse_needs_signal），四种组合里
    // 有一种（不许反手却又要求信号）根本没有含义。换成枚举后组合天然只剩两个
    w->reverse = new QComboBox();
    w->reverse->addItem("立即反手开反向仓",         (int)trend::ReverseMode::Immediate);
    w->reverse->addItem("只平掉，回到正常入场流程", (int)trend::ReverseMode::None);
    w->reverse->setCurrentIndex(c.rule.reverse == trend::ReverseMode::Immediate ? 0 : 1);
    w->reverse->setToolTip(
        "⚠ 这一项【只管亏损止损】。盈利出场永不反手——力竭不等于反转。\n\n"
        "立即反手：平掉的同一拍就开反向仓，不等任何信号。\n"
        "  在震荡市是绞肉机：亏损止损本就在震荡市最频繁，而无条件反手恰好\n"
        "  在那时最激进。开多→止损→开空→涨回来止损→开多…\n"
        "  止损距离 2% 时单次绞杀约 4% 名义，3 倍杠杆即保证金的 12%。\n\n"
        "只平掉：回到正常入场流程，等下一个信号。方向不限——反向信号先来\n"
        "  就反向开，同向信号先来就同向再进一次。");
    revForm->addRow("亏损止损后", w->reverse);

    w->revNote = new QLabel();
    w->revNote->setWordWrap(true);
    w->revNote->setStyleSheet("color:#8b949e;font-size:11px;");
    revForm->addRow(w->revNote);

    // ── 进程外保护 ──────────────────────────────────────────────────────────
    w->disStop = new QCheckBox("在交易所挂灾难止损单（强烈建议开启）");
    w->disStop->setChecked(c.use_disaster_stop);
    w->disStop->setToolTip(
        "把棘轮止损线镜像成交易所上的一张 STOP_MARKET + closePosition 单。\n\n"
        "为什么 SAR 比 DCA 更需要它：DCA 的保护是「名义 ≤ 权益 ⇒ 不可强平」，那是个\n"
        "不依赖任何订单存在的数学不变量，进程死了仓位也扛得住；而趋势 SAR 的全部\n"
        "保护就是那条活在【本进程里】的止损线。程序崩了、断电了、窗口被误关了，\n"
        "这个仓位就是完全裸奔且没有任何底。\n\n"
        "对 SAR 也不存在 DCA 那边「会把浮亏变实亏」的纠结——止损本来就是这套策略的\n"
        "计划内动作，镜像到交易所只是让计划在进程死后仍然执行。");
    riskForm->addRow(w->disStop);

    w->disBuf = new QDoubleSpinBox();
    w->disBuf->setRange(0.1, 20.0);
    w->disBuf->setSingleStep(0.1);
    w->disBuf->setDecimals(2);
    w->disBuf->setValue(c.disaster_stop_buffer_pct);
    w->disBuf->setToolTip(
        "挂单价 = 止损线再往外扩这么多%（多头往下、空头往上）。\n\n"
        "⚠ 不能填 0。交易所用连续的标记价触发，而本地是每 3 秒采样一次——挂在\n"
        "止损线【上】的话交易所几乎总会先触发，于是主出场路径从「本地平仓」变成\n"
        "「交易所平掉、本地靠对账才发现」，而对账发现外部平仓会【停掉 bot】。\n"
        "等于把一次正常的止损出场变成需要人工介入的事件。\n\n"
        "留出缓冲之后：正常情况本地先平并撤掉这张单，只有进程真的不在了，\n"
        "价格才会继续走到这张单上。1% 对常态 ATR 2% 的品种是合适的起点。");
    riskForm->addRow("外扩缓冲 %", w->disBuf);
    {
        auto* h = new QLabel(
            "止损线每根K线都可能棘轮上移，但这张单只在线移动超过 0.5% 时才重挂——"
            "逐次撤挂会吃光限流额度，而它的职责只是「进程死了兜住」，不需要贴着本地线走。");
        h->setWordWrap(true);
        h->setStyleSheet("color:#8b949e;font-size:10px;");
        riskForm->addRow(h);
    }

    w->maxAge = new QSpinBox();
    w->maxAge->setRange(0, 86400);
    w->maxAge->setSingleStep(30);
    w->maxAge->setSuffix(" 秒");
    w->maxAge->setValue(c.signal_max_age_sec);
    w->maxAge->setSpecialValueText("自动（按周期推算）");
    w->maxAge->setToolTip(
        "信号最后一次成功刷新距今超过这么久，就【停止开新仓】（已有仓位照常\n"
        "跑止损，绝不会因为拿不到数据就撤掉保护）。\n\n"
        "为什么需要它：K线信号是每隔几秒拉一次 REST 拿到的。网络断了、限流了、\n"
        "币安在维护，拉不到的那段时间里本地那份「上破通道」会一直是 true，\n"
        "于是 bot 会照着一份【几小时前】的判断去开仓。这个上限就是它的保质期。\n\n"
        "0 = 自动：取 max(60秒, 2.5×K线周期)。1m 周期给 150 秒，4h 给 10 小时。\n"
        "填死值只在你明确知道自己想要什么时才有必要。");
    riskForm->addRow("信号新鲜度上限", w->maxAge);

    w->maxAgeHint = new QLabel();
    w->maxAgeHint->setStyleSheet("color:#8b949e;font-size:10px;");
    riskForm->addRow(w->maxAgeHint);

    // ── 联动 ────────────────────────────────────────────────────────────────
    // 三处联动写在一起收尾，而不是散在各自的 group 里：它们都依赖"全部控件已
    // 构造完"，分散写的话将来插一个控件就可能读到 nullptr
    auto* pagesW  = w->pages;
    auto* revW    = w->reverse;
    auto* revNote = w->revNote;
    auto syncStrategy = [pagesW, revW, revNote](int idx) {
        pagesW->setCurrentIndex(idx);
        const bool psar = (idx == 1);
        // PSAR 的定义就是 stop-and-reverse：触及 SAR ⇒ 平仓并立即反向。
        // 不反手的 PSAR 不是"保守版 PSAR"，而是一个没有入场规则的策略——
        // 因为它的入场信号【只有】翻转这一个来源，不反手就永远空仓
        if (psar) {
            revW->setCurrentIndex(0);
            revW->setEnabled(false);
            revNote->setText(
                "② 趋势 SAR 固定为立即反手，不可更改：它的入场信号【只有】翻转"
                "这一个来源，不反手就会永远空仓。震荡市的连续翻转靠缩短周期或"
                "调小 AF 上限缓解，不靠关掉反手。");
        } else {
            revW->setEnabled(true);
            revNote->setText(idx == 2
                ? "③ 裸K 的「连续反手上限 / 冷却K线数」在上面策略页里，默认都是关的。"
                : "① 海龟的信号密度本来就低，连续反手几乎不会发生，因此不提供上限与冷却。");
        }
    };
    syncStrategy(w->strategy->currentIndex());
    connect(w->strategy, QOverload<int>::of(&QComboBox::currentIndexChanged),
            pagesW, [syncStrategy](int i) { syncStrategy(i); });

    // 信号新鲜度提示要跟着周期走：同一个"自动"在 1m 和 4h 上差 240 倍
    auto* ageW    = w->maxAge;
    auto* ageHint = w->maxAgeHint;
    auto* ivW     = w->interval;
    auto syncAge = [ageW, ageHint, ivW]() {
        if (ageW->value() != 0) { ageHint->setText(""); return; }
        const int sec = TrendConfig::bar_seconds(ivW->currentText().toStdString());
        const int eff = std::max(60, sec * 5 / 2);
        ageHint->setText(QString("　当前周期 %1 ⇒ 自动取 %2 秒（%3 分钟）")
                             .arg(ivW->currentText()).arg(eff).arg(eff / 60.0, 0, 'f', 1));
    };
    syncAge();
    connect(ageW, QOverload<int>::of(&QSpinBox::valueChanged), ageHint,
            [syncAge](int) { syncAge(); });
    connect(ivW, QOverload<int>::of(&QComboBox::currentIndexChanged), ageHint,
            [syncAge](int) { syncAge(); });

    return w;
}

bool MainWindow::collectTrendForm(const std::shared_ptr<TrendFormWidgets>& w, TrendConfig& out) {
    if (!w) return false;

    // 从 base 起算：弹窗里没有控件的字段（signal_max_age_sec 等）必须从原配置
    // 继承，否则手改过 JSON 的值会被静默重置回默认
    out = w->base;
    out.budget_usdt = w->budget->value();
    out.leverage    = w->lev->value();
    out.interval    = w->interval->currentText().toStdString();
    out.size_mode   = (TrendConfig::SizeMode)w->sizeMode->currentData().toInt();
    out.risk_usdt   = w->risk->value();
    out.signal_max_age_sec            = w->maxAge->value();
    out.use_disaster_stop             = w->disStop->isChecked();
    out.disaster_stop_buffer_pct      = w->disBuf->value();

    out.rule.strategy = (trend::Strategy)w->strategy->currentData().toInt();

    // ⚠ 每个策略【只回读自己那一页】的参数，其余从 base 继承原样带走。
    // 不这么做的话，切到 SAR 保存一次，海龟的通道周期就会被这一页上那个
    // 从没显示给用户看过的默认值悄悄覆盖掉——而用户根本没碰过它
    switch (out.rule.strategy) {
        case trend::Strategy::Turtle:
            out.rule.donchian_period = w->dcPeriod->value();
            out.rule.atr_period      = w->atrPeriod->value();
            out.rule.atr_mult        = w->k->value();
            break;
        case trend::Strategy::ParabolicSar:
            out.rule.af_start = w->afStart->value();
            out.rule.af_step  = w->afStep->value();
            out.rule.af_max   = w->afMax->value();
            break;
        case trend::Strategy::BareK:
            out.rule.bare_entry   = (trend::BareEntry)w->bareEntry->currentData().toInt();
            out.rule.once_per_bar = w->oncePerBar->isChecked();
            out.rule.swing_bars   = w->swing->value();
            out.rule.max_consecutive_reverses = w->maxRev->value();
            out.rule.cooldown_bars            = w->cooldown->value();
            break;
    }

    // 反手：PSAR 的下拉是灰的（固定立即反手），这里显式写死而不是读控件——
    // 灰掉的控件仍然可以被 setCurrentIndex 改，靠界面状态保证语义太脆
    out.rule.reverse = (out.rule.strategy == trend::Strategy::ParabolicSar)
                           ? trend::ReverseMode::Immediate
                           : (trend::ReverseMode)w->reverse->currentData().toInt();

    // 连续反手上限只对裸K开放，但 PSAR 是唯一真正会连续翻转的策略。
    // 它的上限不在界面上，靠 headless 配置或手改 JSON——这里保持 base 的值不动

    if (out.size_mode == TrendConfig::SizeMode::RiskBased && out.risk_usdt <= 0) {
        QMessageBox::warning(this, "缺少参数",
            "选择了「固定单笔风险」，但没有填「单次愿亏」。\n\n"
            "这个模式用 单次愿亏 ÷ 真实止损距离 反推仓位，没有它算不出任何数量。");
        return false;
    }
    if (out.rule.strategy == trend::Strategy::ParabolicSar &&
        out.rule.af_start > out.rule.af_max) {
        QMessageBox::warning(this, "参数矛盾",
            "AF 初值大于 AF 上限。\n\n"
            "SAR 一开仓就会被夹到上限，「趋势越强收得越快」这个机制直接失效——"
            "等于把抛物线 SAR 退化成一条固定比例的追踪止损。");
        return false;
    }
    return true;
}

// 校验 + 落地。返回 false = 已经向用户报过错，且【一点状态都没动】
bool MainWindow::applyTrendConfig(TrendConfig c, const TrendBot* existing) {
    if (!trend_engine_) {
        QMessageBox::information(this, "未连接", "请先连接交易所后再配置策略。");
        return false;
    }

    if (existing) {
        // 编辑已有：先删再建会丢掉持仓跟踪，所以有持仓时不允许改。
        // 提示要说清楚为什么，不能只是"不能改"
        if (existing->st.pos != trend::Pos::Flat) {
            QMessageBox::warning(this, "有持仓，无法修改",
                QString::fromStdString(c.symbol) +
                " 当前有持仓。改参数会重建止损线基准，"
                "可能让线瞬间跳到现价另一侧而立刻触发平仓。\n\n"
                "请先平仓，再修改参数。");
            return false;
        }
    }

    // 品种是否真的存在。补全后缀只能救 "BTC" 这类漏写，救不了拼错的币名——
    // 而拼错的后果是永远拉不到 K 线、永远"等信号"，除非去翻日志否则看不出来。
    // 这里一次同步查询（有缓存，通常是内存命中），比让用户干等一小时划算
    if (client_) {
        auto info = client_->get_symbol_info(c.symbol);
        if (!info.valid) {
            QMessageBox::warning(this, "品种不存在",
                QString::fromStdString(c.symbol) +
                " 在币安 USDT-M 合约上不存在。\n\n"
                "请检查拼写。只需要输入代币符号（如 BTC / ETH / SUI），"
                "程序会自动补全 USDT 后缀。");
            return false;
        }
    }

    // 校验全过了才动状态：先删旧的再建新的。顺序不能反——add_bot 对同品种
    // 已存在未停止的 bot 会直接返回空串
    if (existing) trend_engine_->remove_bot(existing->bot_id);

    const auto id = trend_engine_->add_bot(c);
    if (id.empty()) {
        QMessageBox::warning(this, "添加失败", "该品种已存在一个未停止的趋势 bot。");
        return false;
    }
    if (ticker_) ticker_->subscribe(c.symbol);
    trendSigForce_.store(true);   // 下一个 tick 立刻拉信号，不等满一整轮

    QString params;
    switch (c.rule.strategy) {
        case trend::Strategy::Turtle:
            params = QString("海龟 唐奇安%1 ATR%2 k=%3")
                         .arg(c.rule.donchian_period).arg(c.rule.atr_period)
                         .arg(c.rule.atr_mult, 0, 'f', 1);
            break;
        case trend::Strategy::ParabolicSar:
            params = QString("趋势SAR AF %1/%2/%3")
                         .arg(c.rule.af_start, 0, 'f', 3)
                         .arg(c.rule.af_step,  0, 'f', 3)
                         .arg(c.rule.af_max,   0, 'f', 3);
            break;
        case trend::Strategy::BareK:
            params = QString("裸K %1 摆动N=%2%3")
                         .arg(c.rule.bare_entry == trend::BareEntry::Immediate
                                  ? "立即顺势" : "等收盘突破")
                         .arg(c.rule.swing_bars)
                         .arg(c.rule.once_per_bar ? " 每根限一次" : "");
            break;
    }
    log(QString("趋势 %1 已配置：%2 / %3 / %4")
            .arg(QString::fromStdString(c.symbol))
            .arg(params)
            .arg(QString::fromStdString(c.interval))
            .arg(c.rule.reverse == trend::ReverseMode::Immediate
                     ? "止损后立即反手" : "止损后等信号"), "OK");
    refreshBotTable();
    save_trend_bots();
    return true;
}

// ── 持久化 ──────────────────────────────────────────────────────────────────
// 配置与运行时状态存在同一个文件里。DCA 那边分了两个文件（bots.json + 状态），
// 这里不分是因为 SAR 的运行时状态只有 6 个标量，单独一个文件不值当

std::string MainWindow::trend_cfg_path() const {
    return (portable_data_dir() + "/sar_bots.json").toStdString();
}

void MainWindow::save_trend_bots() {
    if (!trend_engine_) return;
    QJsonArray arr;
    for (const auto& b : trend_engine_->get_bots()) {
        QJsonObject o;
        o["symbol"]      = QString::fromStdString(b.cfg.symbol);
        o["budget_usdt"] = b.cfg.budget_usdt;
        o["leverage"]    = b.cfg.leverage;
        o["interval"]    = QString::fromStdString(b.cfg.interval);
        o["strategy"]        = (int)b.cfg.rule.strategy;
        o["donchian_period"] = b.cfg.rule.donchian_period;
        o["atr_period"]      = b.cfg.rule.atr_period;
        o["atr_mult"]        = b.cfg.rule.atr_mult;
        o["af_start"]        = b.cfg.rule.af_start;
        o["af_step"]         = b.cfg.rule.af_step;
        o["af_max"]          = b.cfg.rule.af_max;
        o["bare_entry"]      = (int)b.cfg.rule.bare_entry;
        o["once_per_bar"]    = b.cfg.rule.once_per_bar;
        o["reverse"]         = (int)b.cfg.rule.reverse;
        o["max_consecutive_reverses"] = b.cfg.rule.max_consecutive_reverses;
        o["cooldown_bars"]   = b.cfg.rule.cooldown_bars;
        o["size_mode"]       = (int)b.cfg.size_mode;
        o["risk_usdt"]       = b.cfg.risk_usdt;
        o["swing_bars"]      = b.cfg.rule.swing_bars;
        o["signal_max_age_sec"] = b.cfg.signal_max_age_sec;
        o["use_disaster_stop"]  = b.cfg.use_disaster_stop;
        o["disaster_stop_buffer_pct"] = b.cfg.disaster_stop_buffer_pct;
        // 单号必须跨重启存活：不存的话重启后会遗留一张触发价对不上的
        // 孤儿单，而 closePosition 同方向只能有一张，新的挂不上去
        o["disaster_stop_id"]    = QString::fromStdString(b.disaster_stop_id);
        o["disaster_stop_price"] = b.disaster_stop_price;
        o["state"]           = (int)b.state;
        // ── 运行时状态 ──
        // Qt 的 QJsonValue(double) 是全精度往返，不像 ostringstream 会截到 6 位。
        // 这点在 SAR 上尤其要紧：止损线【就是】出场价
        o["pos"]             = (int)b.st.pos;
        o["entry_price"]     = b.st.entry_price;
        o["peak"]            = b.st.peak;
        o["stop"]            = b.st.stop;
        // ② SAR 专用：AF 丢了的话重启后 SAR 会从 0.02 重新加速，止损线瞬间
        // 从"贴着价格"退回到远处——等于白白让出已经锁住的利润
        o["af"]              = b.st.af;
        o["consec_reverses"] = b.st.consec_reverses;
        o["cooldown_left"]   = b.st.cooldown_left;
        // ③ 裸K 专用：once_per_bar 的判据（最后一次开仓所在K线的开盘时间）。
        // 用 qint64 重载而不是 double：毫秒时间戳虽然还在 double 的精确整数
        // 范围内，但这是个"现在够用"的巧合，不值得把它写进持久化格式
        o["last_entry_bar_ms"] = (qint64)b.st.last_entry_bar_ms;
        o["qty"]             = b.qty;
        o["realized_pnl"]    = b.realized_pnl;
        o["trade_count"]     = b.trade_count;
        o["win_count"]       = b.win_count;
        arr.append(o);
    }
    // QSaveFile = 原子写（写临时文件 + commit 时改名）。崩在写入中途不会
    // 留下半截 JSON——而半截 JSON 丢掉的正是那条止损线
    QSaveFile f(QString::fromStdString(trend_cfg_path()));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    f.commit();
}

void MainWindow::load_and_restore_trend() {
    if (!trend_engine_) return;
    QFile f(QString::fromStdString(trend_cfg_path()));
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;

    int n = 0;
    for (const auto& v : doc.array()) {
        if (!v.isObject()) continue;
        const auto o = v.toObject();
        auto sym = o["symbol"].toString();
        if (sym.isEmpty()) continue;

        // 迁移：v4.1.1 之前 SAR 的加品种框不补 USDT 后缀，存进来的可能是 "BTC"。
        // 这种条目永远拉不到 K 线、永远"等信号"。补全并明确告知，
        // 否则用户升级后还得自己找出来删掉重加
        if (!sym.endsWith("USDT")) {
            const QString fixed = sym + "USDT";
            log(QString("趋势品种 %1 缺少 USDT 后缀，已自动更正为 %2"
                        "（币安合约的代码形如 BTCUSDT）").arg(sym, fixed), "WARN");
            sym = fixed;
        }

        TrendBot b;
        b.cfg.symbol      = sym.toStdString();
        b.cfg.budget_usdt = o["budget_usdt"].toDouble(1000);
        b.cfg.leverage    = o["leverage"].toInt(3);
        b.cfg.interval    = o["interval"].toString("4h").toStdString();
        b.cfg.rule.donchian_period = o["donchian_period"].toInt(20);
        b.cfg.rule.atr_period      = o["atr_period"].toInt(14);
        b.cfg.rule.atr_mult        = o["atr_mult"].toDouble(3.0);
        b.cfg.rule.af_start        = o["af_start"].toDouble(0.02);
        b.cfg.rule.af_step         = o["af_step"].toDouble(0.02);
        b.cfg.rule.af_max          = o["af_max"].toDouble(0.20);
        b.cfg.rule.swing_bars      = o["swing_bars"].toInt(3);
        b.cfg.rule.once_per_bar    = o["once_per_bar"].toBool(false);
        b.cfg.rule.max_consecutive_reverses = o["max_consecutive_reverses"].toInt(0);
        b.cfg.rule.cooldown_bars   = o["cooldown_bars"].toInt(0);
        b.cfg.size_mode = (TrendConfig::SizeMode)o["size_mode"].toInt(0);
        b.cfg.risk_usdt = o["risk_usdt"].toDouble(0);

        // ── 迁移：v5.0.x 的 sar_mode(0=唐奇安,1=裸K) → strategy ────────────
        // 旧存档里没有 strategy 键。默认值不能拍脑袋填 Turtle：旧的 mode=1
        // 是裸K，静默当成海龟会把一个 3 根摆动止损的高频配置变成 3×ATR 的
        // 慢速配置，而仓位大小照旧——风险敞口直接变几倍
        if (o.contains("strategy")) {
            const int s = o["strategy"].toInt(0);
            b.cfg.rule.strategy = (s == (int)trend::Strategy::ParabolicSar)
                                      ? trend::Strategy::ParabolicSar
                                  : (s == (int)trend::Strategy::BareK)
                                      ? trend::Strategy::BareK
                                      : trend::Strategy::Turtle;
        } else if (o["sar_mode"].toInt(0) == 1) {
            b.cfg.rule.strategy   = trend::Strategy::BareK;
            // 旧裸K的入场规则是"刚收盘那根是阳线就做多"，v5.1 已删除。
            // 最接近的替代是"等收盘突破前一根高/低"——同样只在收盘判定，
            // 但严格得多。这是【行为变化】，必须说出来
            b.cfg.rule.bare_entry = trend::BareEntry::BreakPrevBar;
            log(QString("趋势 %1：旧的「裸K线 阳线入场」规则已在 v5.1 移除，"
                        "已迁移为「纯裸K · 等收盘突破前一根高/低」。"
                        "入场条件比原来严格，信号会明显变少——"
                        "这是有意的，旧规则在下跌趋势里会把一根反弹阳线当成做多信号。")
                    .arg(sym), "WARN");
        } else {
            b.cfg.rule.strategy = trend::Strategy::Turtle;
        }

        // ── 迁移：两个 bool → ReverseMode ──────────────────────────────────
        if (o.contains("reverse")) {
            b.cfg.rule.reverse = (o["reverse"].toInt(1) == (int)trend::ReverseMode::Immediate)
                                     ? trend::ReverseMode::Immediate
                                     : trend::ReverseMode::None;
        } else {
            // 旧默认是 allow_reverse=true + reverse_needs_signal=true，
            // 即"反手但要等反向信号"。新枚举里没有这一档，最接近的是 None
            // （平掉后回到正常入场流程，反向信号来了照样反向开）
            const bool allow = o["allow_reverse"].toBool(true);
            const bool needs = o["reverse_needs_signal"].toBool(true);
            b.cfg.rule.reverse = (allow && !needs) ? trend::ReverseMode::Immediate
                                                   : trend::ReverseMode::None;
            if (allow && needs)
                log(QString("趋势 %1：旧的「反手需要反向信号」已合并进「只平掉，"
                            "回到正常入场流程」。行为几乎不变，唯一差别是现在"
                            "同向信号先来也会再进一次。").arg(sym), "WARN");
        }
        // PSAR 的定义就是 stop-and-reverse，不反手它就永远空仓
        if (b.cfg.rule.strategy == trend::Strategy::ParabolicSar)
            b.cfg.rule.reverse = trend::ReverseMode::Immediate;
        // 默认改成 0（自动按周期推算）。旧存档里存着的 900 会原样读回来，
        // 那是个对 4h 周期【太短】的值——v5.0.x 的 4h bot 在网络稍有波动时
        // 就会误判"信号过期"而停开新仓。想用自动值的话到弹窗里清成 0
        b.cfg.signal_max_age_sec   = o["signal_max_age_sec"].toInt(0);
        b.cfg.use_disaster_stop    = o["use_disaster_stop"].toBool(false);
        b.cfg.disaster_stop_buffer_pct =
            o["disaster_stop_buffer_pct"].toDouble(1.0);
        b.disaster_stop_id    = o["disaster_stop_id"].toString().toStdString();
        b.disaster_stop_price = o["disaster_stop_price"].toDouble(0.0);
        b.state = (TrendBot::State)o["state"].toInt(0);

        const int pos = o["pos"].toInt((int)trend::Pos::Flat);
        b.st.pos = (pos == (int)trend::Pos::Long)  ? trend::Pos::Long
                 : (pos == (int)trend::Pos::Short) ? trend::Pos::Short
                                                 : trend::Pos::Flat;
        b.st.entry_price     = o["entry_price"].toDouble(0);
        b.st.peak            = o["peak"].toDouble(0);
        b.st.stop            = o["stop"].toDouble(0);
        // 旧存档没有 af。默认取 af_start 而不是 0：0 会让 SAR 完全不动
        // （SAR += 0×(EP−SAR)），止损线就此冻在重启那一刻的位置
        b.st.af              = o["af"].toDouble(b.cfg.rule.af_start);
        b.st.consec_reverses = o["consec_reverses"].toInt(0);
        b.st.cooldown_left   = o["cooldown_left"].toInt(0);
        b.st.last_entry_bar_ms = (int64_t)o["last_entry_bar_ms"].toInteger(0);
        b.qty          = o["qty"].toDouble(0);
        b.realized_pnl = o["realized_pnl"].toDouble(0);
        b.trade_count  = o["trade_count"].toInt(0);
        b.win_count    = o["win_count"].toInt(0);

        // 半截状态防御，与 headless 侧同款：有方向却缺开仓价/止损线/数量的，
        // 当成空仓丢弃。stop=0 时多头的 price<=stop 永远不成立，
        // 仓位会一直裸着没人管——比"空仓"危险得多。交给对账去发现真实仓位
        if (b.st.pos != trend::Pos::Flat &&
            (b.st.entry_price <= 0 || b.st.stop <= 0 || b.qty <= 0)) {
            b.st = trend::State{};
            b.qty = 0;
        }

        // v4.7.1 之前这里还要拦"该品种已由网格 DCA 接管"（弹窗里拦了，但恢复
        // 路径没拦，手改过的落盘文件会漏进来）。只剩一套策略之后，这类跨引擎
        // 冲突不存在了——同品种只能有一个 bot，由 restore_bot 自己保证

        if (!trend_engine_->restore_bot(b).empty()) {
            ++n;
            if (ticker_) ticker_->subscribe(b.cfg.symbol);
        }
    }
    if (n > 0) {
        log(QString("已恢复 %1 个趋势 bot").arg(n), "OK");
        trendSigForce_.store(true);
    }
    refreshBotTable();
}

} // namespace ccg
