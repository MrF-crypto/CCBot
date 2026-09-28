// MainWindow 的 SAR 部分：行序、同表行填充、配置表单、落地、持久化。
//
// 单独一个翻译单元，不塞进 main_window.cpp——那个文件已经 3800+ 行，
// 而且是本项目历史上几乎每一个 GUI bug 的出处。
//
// v4.5.0 起 SAR 不再有自己的标签页和表格，与 DCA 共用 botTable_ 的 16 列。
// 合并只发生在【界面层】：两个引擎各自照原样持有自己的 bot、各自落盘自己的
// 文件（bots.json / sar_bots.json），这里只负责把 SAR 的 bot 画进那张表。
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
// 行序
// ─────────────────────────────────────────────────────────────────────────────
// 排序键刻意让 DCA 的行序【一行都不变】：CcgEngine 的 bot_id 形如
// "BTCUSDT_L_3"，装在 std::map 里本来就是按 (品种字典序, 后缀 _B<_L<_S) 排的。
// 这里用同一个口径，再把 SAR 排在同品种 DCA 之后（sort_key=3）——同一个品种只
// 可能属于一套策略，所以这一档实际上永远不会和前三档同时出现，写成 3 只是为了
// 让"万一真出现了"的顺序也是确定的，而不是取决于哈希或插入顺序。
std::vector<BotRow> MainWindow::buildRows(const std::vector<CcgBot>& dca,
                                          const std::vector<SarBot>& sar) const {
    std::vector<BotRow> rows;
    rows.reserve(dca.size() + sar.size());

    for (const auto& b : dca) {
        BotRow r;
        r.kind     = BotRow::Kind::Dca;
        r.bot_id   = b.bot_id;
        r.symbol   = b.cfg.symbol;
        r.sort_key = (b.cfg.direction == CcgConfig::Direction::Both)  ? 0
                   : (b.cfg.direction == CcgConfig::Direction::Long)  ? 1
                                                                      : 2;
        r.dca      = &b;
        rows.push_back(r);
    }
    for (const auto& b : sar) {
        BotRow r;
        r.kind     = BotRow::Kind::Sar;
        r.bot_id   = b.bot_id;
        r.symbol   = b.cfg.symbol;
        r.sort_key = 3;
        r.sar      = &b;
        rows.push_back(r);
    }

    // stable_sort 而不是 sort：同品种同 sort_key（只可能是同品种两个 _L，不该
    // 发生但引擎不禁止）时保持引擎给出的原顺序，避免行在两次刷新间无故换位——
    // 换位会让正在点的按钮跑到别的 bot 上
    std::stable_sort(rows.begin(), rows.end(), [](const BotRow& a, const BotRow& b) {
        if (a.symbol != b.symbol) return a.symbol < b.symbol;
        return a.sort_key < b.sort_key;
    });
    return rows;
}

// ─────────────────────────────────────────────────────────────────────────────
// SAR 行填充
// ─────────────────────────────────────────────────────────────────────────────
// 列位复用 DCA 的 16 列。对照表（6/7/8 由调用方在分派前统一填好，这里不碰）：
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
void MainWindow::fillSarRow(int row, const SarBot& b, RowTotals& t) {
    const bool is_long = (b.st.pos == sar::Pos::Long);
    const bool has_pos = (b.st.pos != sar::Pos::Flat && b.qty > 0);
    const bool stopped = (b.state == SarBot::State::Stopped);

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
    botTable_->setItem(row, 0, mk(QString::number(row + 1), kGrey));

    // ── 1 品种 ──
    botTable_->setItem(row, 1, mk(QString::fromStdString(b.cfg.symbol)));

    // ── 2 方向 ──
    // ⚠ 与 DCA 不同义。DCA 那列是【配置】（你选的多/空/双向），SAR 的配置里
    //   根本没有方向——两个方向都由信号决定，所以这里只能显示【当前持仓方向】。
    //   空仓时显示"空仓"而不是"—"，好让这个差别在界面上看得出来
    auto* dir_it = mk(has_pos ? (is_long ? "多" : "空") : "空仓",
                      has_pos ? (is_long ? kGreen : kRed) : kGrey);
    dir_it->setToolTip("趋势 SAR 没有方向配置：上破做多、下破做空由信号决定。\n"
                       "这一格显示的是【当前持仓方向】，不是配置。");
    botTable_->setItem(row, 2, dir_it);

    // ── 3 策略 ──
    const bool bar_mode = (b.cfg.rule.mode == sar::Mode::BarPattern);
    auto* strat_it = mk(bar_mode ? "SAR·裸K线" : "SAR·唐奇安", kBlue);
    {
        QString tip = QString("趋势 SAR —— %1\n信号周期 %2\n")
                          .arg(bar_mode ? "裸K线入场 + 摆动止损"
                                        : "唐奇安突破入场 + ATR 追踪止损")
                          .arg(QString::fromStdString(b.cfg.interval));
        if (bar_mode)
            tip += QString("摆动止损根数 N=%1（信号根不算在内）\n")
                       .arg(b.cfg.rule.swing_bars);
        else
            tip += QString("通道周期 %1 / ATR 周期 %2 / k=%3\n")
                       .arg(b.cfg.rule.donchian_period)
                       .arg(b.cfg.rule.atr_period)
                       .arg(b.cfg.rule.atr_mult, 0, 'f', 1);
        tip += (b.cfg.size_mode == SarConfig::SizeMode::RiskBased)
                   ? QString("仓位：按 ATR 等风险，单次愿亏 %1U（名义上限 %2U）\n")
                         .arg(b.cfg.risk_usdt, 0, 'f', 2).arg(b.cfg.budget_usdt, 0, 'f', 0)
                   : QString("仓位：固定名义 %1U\n").arg(b.cfg.budget_usdt, 0, 'f', 0);
        tip += QString("杠杆 %1x　反手：%2")
                   .arg(b.cfg.leverage)
                   .arg(!b.cfg.rule.allow_reverse ? "关闭"
                        : b.cfg.rule.reverse_needs_signal ? "需反向信号" : "无条件");
        strat_it->setToolTip(tip);
    }
    botTable_->setItem(row, 3, strat_it);

    // ── 4 层进度 → 金字塔档数 ──
    // 持 3 档和持 1 档的敞口差 3 倍，不显示的话看不出这个仓位到底压了多重
    QString tier = "—";
    if (has_pos && b.cfg.rule.pyramid_max_adds > 0)
        tier = QString("%1/%2").arg(b.st.adds_done + 1)
                               .arg(b.cfg.rule.pyramid_max_adds + 1);
    else if (has_pos)
        tier = "1";
    // 加到 2 档以上就标黄：敞口已经不是首档那一份了，值得看见
    auto* tier_it = mk(tier, (has_pos && b.st.adds_done > 0) ? kAmber : kGrey);
    if (has_pos && b.cfg.rule.pyramid_max_adds > 0)
        tier_it->setToolTip(QString("金字塔加仓：已到第 %1 档（首档 + %2 次加仓），"
                                    "最多 %3 档\n每朝有利方向走 %4×ATR 加一档")
                                .arg(b.st.adds_done + 1).arg(b.st.adds_done)
                                .arg(b.cfg.rule.pyramid_max_adds + 1)
                                .arg(b.cfg.rule.pyramid_step_atr, 0, 'f', 2));
    else
        tier_it->setToolTip("趋势 SAR 没有分层摊薄。这一格显示金字塔【顺势】加仓的档数，\n"
                            "方向与 DCA 的补仓相反：DCA 是跌了加，这里是涨了加。\n"
                            "未开启顺势加仓时显示 —。");
    botTable_->setItem(row, 4, tier_it);

    // ── 5 均价 → 开仓价 ──
    auto* entry_it = mk(has_pos ? fmt_px(b.st.entry_price) : "—");
    if (has_pos)
        entry_it->setToolTip(b.st.adds_done > 0
            ? "开仓价（已按数量加权的均价——加过仓，所以不是首档的成交价）"
            : "开仓价");
    botTable_->setItem(row, 5, entry_it);

    // ── 9 浮动P&L ──
    botTable_->setItem(row, 9,
        mk(has_pos ? fmt_signed(upnl) : "—", pnl_color(upnl)));

    // ── 10 保证金 / 11 收益率 ──
    double margin = 0, roe = 0;
    if (has_pos && b.current_price > 0 && b.cfg.leverage > 0) {
        margin = b.current_price * b.qty / b.cfg.leverage;
        if (margin > 0) roe = upnl / margin * 100.0;
    }
    botTable_->setItem(row, 10,
        mk(margin > 0 ? QString("$%1").arg(margin, 0, 'f', 2) : "—", kGrey));
    auto* roe_it = mk(margin > 0 ? QString("%1%").arg(roe, 0, 'f', 2) : "—",
                      pnl_color(margin > 0 ? roe : 0));
    if (margin > 0)
        roe_it->setToolTip(QString("浮动盈亏 $%1 / 保证金 $%2（已按 %3x 杠杆放大）\n\n"
                                   "趋势 SAR 没有固定止盈：收益全来自少数跑得很远的单子，\n"
                                   "所以这个数没有「该止盈了」的阈值，只看止损线跟到哪。")
                              .arg(upnl, 0, 'f', 2).arg(margin, 0, 'f', 2)
                              .arg(b.cfg.leverage));
    botTable_->setItem(row, 11, roe_it);

    // ── 12 强平/止损 → 追踪止损线 ──
    // 这是持仓时最该盯的一格：止损线越过成本价 = 这笔已锁定盈利，转绿。
    // 距离（现价离线还有多远，也就是"现在被打掉会亏/赚多少"）放悬停
    QString stop_s = "—";
    QString stop_c = kAmber;
    QString stop_tip;
    if (has_pos && b.st.stop > 0) {
        const bool locked = is_long ? (b.st.stop >= b.st.entry_price)
                                    : (b.st.stop <= b.st.entry_price);
        stop_s = fmt_px(b.st.stop);
        stop_c = locked ? kGreen : kAmber;
        stop_tip = QString("追踪止损线 %1（棘轮，只朝有利方向移动）\n").arg(stop_s);
        if (b.current_price > 0) {
            const double d = std::fabs(b.current_price - b.st.stop) / b.current_price * 100.0;
            stop_tip += QString("距离 %1%　—— 现在被打掉就按这条线出场\n")
                            .arg(d, 0, 'f', 2);
        }
        stop_tip += locked
            ? "✓ 线已越过成本价：这笔已锁定盈利，最坏情况也是赚"
            : "线还在成本价的亏损侧：被打掉是亏损出场，可能触发反手";
    } else if (!has_pos) {
        stop_tip = "空仓时没有止损线——没有仓位就没有保护。\n"
                   "这一列对 DCA 行显示强平价，对 SAR 行显示追踪止损线。";
    }
    auto* stop_it = mk(stop_s, stop_c);
    stop_it->setToolTip(stop_tip);
    botTable_->setItem(row, 12, stop_it);

    // ── 13 已实现（悬停带胜率与连续反手）──
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
    botTable_->setItem(row, 13, real_it);

    // ── 14 状态（悬停带决策文字 + ATR 数据灯）──
    QString st_s = stopped        ? "已停止"
                 : b.pending      ? "下单中"
                 : has_pos        ? "持仓中"
                 : (b.st.cooldown_left > 0)
                       ? QString("冷却%1根").arg(b.st.cooldown_left)
                       : "等信号";
    QString st_c = stopped ? kRed : has_pos ? kBlue : kGrey;

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
    botTable_->setItem(row, 14, st_it);

    // ── 15 操作 ──
    // 键没变就不重建控件，否则每 3 秒重建一次会把点击吞掉。
    // 键里必须带 "sar|" 前缀：同一个行号从 DCA 换成 SAR 时按钮组要整套重建
    const QString opKey = QString("sar|%1|%2|%3|%4")
                              .arg(QString::fromStdString(b.bot_id))
                              .arg((int)b.state).arg(has_pos).arg(b.pending);
    if (row < (int)opRowKeys_.size() && opRowKeys_[row] == opKey
        && botTable_->cellWidget(row, 15) != nullptr) {
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
            if (!sar_engine_) return;
            sar_engine_->stop_bot(id);
            refreshBotTable();
            save_sar_bots();
        });
        opl->addWidget(bp);
    } else {
        auto* br = new QPushButton("继续");
        br->setFixedHeight(20);
        br->setStyleSheet("QPushButton{background:#1a3d1a;color:#3fb950;"
                          "font-size:11px;padding:0 6px;}");
        connect(br, &QPushButton::clicked, this, [this, id]() {
            if (!sar_engine_) return;
            sar_engine_->resume_bot(id);
            // 「全部停止」会关掉 tick 定时器——单个 bot 恢复时必须把它拉起来，
            // 否则 bot 显示"运行中"但引擎永远不被驱动，止损线永远不会触发
            if (tick_timer_ && !tick_timer_->isActive()) {
                tick_timer_->start();
                log("Tick 定时器已重新启动");
            }
            sarSigForce_.store(true);   // 下一拍立刻拉信号，不干等一个周期
            refreshBotTable();
            save_sar_bots();
        });
        opl->addWidget(br);
    }

    if (has_pos && !b.pending) {
        auto* bc = new QPushButton("平仓");
        bc->setFixedHeight(20);
        bc->setStyleSheet("QPushButton{color:#d29922;font-size:11px;padding:0 6px;}");
        connect(bc, &QPushButton::clicked, this, [this, id, sym]() {
            if (!sar_engine_) return;
            if (!confirmDanger("确认平仓",
                               sym + " 将以市价立即平掉当前 SAR 仓位。", "平仓")) return;
            sar_engine_->close_bot(id);
        });
        opl->addWidget(bc);
    }

    auto* bd = new QPushButton("删除");
    bd->setFixedHeight(20);
    bd->setStyleSheet("QPushButton{color:#f85149;font-size:11px;padding:0 6px;}");
    connect(bd, &QPushButton::clicked, this, [this, id, sym, has_pos]() {
        if (!sar_engine_) return;
        // 有持仓时删除 = 交易所上留下一笔【没有止损线守着】的裸仓位。
        // 必须说清楚，不能只问"确定删除吗"
        const QString body = has_pos
            ? sym + " 当前有持仓。删除后程序不再跟踪它，"
                    "交易所上的仓位会失去追踪止损的保护，需要你手动处理。"
            : sym + " 将从监控列表中移除。";
        if (!confirmDanger("确认删除", body, "删除")) return;
        sar_engine_->remove_bot(id);
        unsubscribeIfUnused(sym.toStdString());
        refreshBotTable();
        save_sar_bots();
    });
    opl->addWidget(bd);
    opl->addStretch(1);
    botTable_->setCellWidget(row, 15, opw);
}

// ─────────────────────────────────────────────────────────────────────────────
// SAR 配置表单
// ─────────────────────────────────────────────────────────────────────────────
// 拆成"构建"和"回读"两半，是为了让 openStrategyDialog 能把整张表单嵌进自己的
// 策略分页里，而不必把这 30 多个控件的构造逻辑复制一份。
// 结构体定义留在本 .cpp——头文件里只有前向声明，改一个 spinbox 不会触发全量重编。
struct SarFormWidgets {
    QComboBox*      sizeMode = nullptr;
    QDoubleSpinBox* budget   = nullptr;
    QDoubleSpinBox* risk     = nullptr;
    QSpinBox*       lev      = nullptr;
    QComboBox*      mode     = nullptr;
    QSpinBox*       swing    = nullptr;
    QComboBox*      interval = nullptr;
    QSpinBox*       dcPeriod = nullptr;
    QSpinBox*       atrPeriod = nullptr;
    QDoubleSpinBox* k        = nullptr;
    QCheckBox*      rev      = nullptr;
    QCheckBox*      revSig   = nullptr;
    QSpinBox*       maxRev   = nullptr;
    QSpinBox*       cooldown = nullptr;
    QSpinBox*       pyrMax   = nullptr;
    QDoubleSpinBox* pyrStep  = nullptr;
    SarConfig       base;      // 弹窗没有控件的字段（signal_max_age_sec 等）从这里继承
};

std::shared_ptr<SarFormWidgets> MainWindow::buildSarForm(QVBoxLayout* into,
                                                          const SarConfig& c) {
    auto w = std::make_shared<SarFormWidgets>();
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

    auto* posForm  = mkGroup("仓位与杠杆", "#58a6ff");
    auto* sigForm  = mkGroup("入场与出场", "#a371f7");
    auto* revForm  = mkGroup("反手与加仓", "#3fb950");

    // ── 仓位与杠杆 ──────────────────────────────────────────────────────────
    // ⚠ 仓位算法排在最前面：它决定了下面两个框各自的含义（名义价值 vs 名义上限），
    //   放在后面的话用户会先填完再发现填错了地方
    w->sizeMode = new QComboBox();
    w->sizeMode->addItem("固定名义", (int)SarConfig::SizeMode::Notional);
    w->sizeMode->addItem("按 ATR 等风险", (int)SarConfig::SizeMode::RiskBased);
    w->sizeMode->setCurrentIndex(c.size_mode == SarConfig::SizeMode::RiskBased ? 1 : 0);
    w->sizeMode->setToolTip(
        "固定名义：每笔都是同样的名义价值。\n"
        "等风险：名义 = 单次愿亏 / (k×ATR%)，每笔止损亏的钱固定。\n\n"
        "为什么需要等风险：同一份名义，在 4h ATR 1.6% 的 LTC 和 5.3% 的 COTI 上，"
        "单次止损亏的钱差 3.3 倍。固定名义 = 风险全压在高波动那几个品种上，"
        "而「铺开品种分散风险」就此失效。这是海龟的「单位」概念。");
    posForm->addRow("仓位算法", w->sizeMode);

    w->budget = new QDoubleSpinBox();
    w->budget->setRange(10, 10'000'000);
    w->budget->setDecimals(2);
    w->budget->setValue(c.budget_usdt);
    w->budget->setToolTip("固定名义模式：每笔仓位的名义价值。\n"
                          "等风险模式：名义价值的【上限】"
                          "（ATR 极小时兜住公式算出的天量仓位）。");
    auto* budgetLabel = new QLabel();
    posForm->addRow(budgetLabel, w->budget);

    w->risk = new QDoubleSpinBox();
    w->risk->setRange(0, 1'000'000);
    w->risk->setDecimals(2);
    w->risk->setValue(c.risk_usdt);
    // 值为 0（= 最小值）时显示这行字而不是"0.00"。此前它是一个灰掉的"0"，
    // 看起来像"这个功能坏了"，而真实原因只是仓位算法还没切过去
    w->risk->setSpecialValueText("未设置");
    w->risk->setToolTip("单次止损愿意亏多少钱（USDT）。仅「按 ATR 等风险」模式使用。\n"
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
                                : "　单次愿亏（切到「按 ATR 等风险」后可填）");
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

    // ── 入场与出场 ──────────────────────────────────────────────────────────
    // 入场/止损算法：两套成套的组合，不是可以混搭的两个旋钮
    w->mode = new QComboBox();
    w->mode->addItem("唐奇安突破 + ATR 止损", (int)sar::Mode::Donchian);
    w->mode->addItem("裸K线 + 摆动止损",       (int)sar::Mode::BarPattern);
    w->mode->setCurrentIndex(c.rule.mode == sar::Mode::BarPattern ? 1 : 0);
    w->mode->setToolTip(
        "唐奇安突破：价格创 N 根新高/新低入场，止损用 k×ATR 的棘轮线。海龟原版。\n\n"
        "裸K线：刚收盘那根是阳线就做多、阴线就做空；止损用【它之前 N 根】的\n"
        "最低价（做多）/ 最高价（做空），窗口随K线右移，棘轮只朝有利方向。\n"
        "入场只在K线收盘那一拍判定，不看盘中。");
    sigForm->addRow("入场/止损算法", w->mode);

    w->swing = new QSpinBox();
    w->swing->setRange(1, 50);
    w->swing->setValue(c.rule.swing_bars);
    w->swing->setToolTip(
        "止损用最近几根的最低/最高价。信号根【不算】在内。\n\n"
        "⚠ 这个数直接决定持仓时长和交易频率：随机游走下平均持仓约 N+1 根。\n"
        "N=3 ⇒ 4 根就被打掉一次。配短周期时交易次数会非常高。");
    sigForm->addRow("摆动止损根数", w->swing);

    w->interval = new QComboBox();
    w->interval->addItems({"3m", "5m", "15m", "30m", "1h", "4h", "12h", "1d"});
    w->interval->setCurrentText(QString::fromStdString(c.interval));
    w->interval->setToolTip("信号K线周期。周期越短信号越多，假突破也越多。");
    sigForm->addRow("信号周期", w->interval);

    w->dcPeriod = new QSpinBox();
    w->dcPeriod->setRange(2, 200);
    w->dcPeriod->setValue(c.rule.donchian_period);
    w->dcPeriod->setToolTip("唐奇安通道周期（海龟原版 20）。\n"
                            "上破 N 根最高 → 做多，下破 N 根最低 → 做空。\n"
                            "通道只由已收盘K线构成，当前这根去撞它。");
    sigForm->addRow("通道周期", w->dcPeriod);

    w->atrPeriod = new QSpinBox();
    w->atrPeriod->setRange(2, 100);
    w->atrPeriod->setValue(c.rule.atr_period);
    sigForm->addRow("ATR 周期", w->atrPeriod);

    w->k = new QDoubleSpinBox();
    w->k->setRange(0.5, 20.0);
    w->k->setSingleStep(0.5);
    w->k->setDecimals(1);
    w->k->setValue(c.rule.atr_mult);
    w->k->setToolTip("止损距离 = k × ATR（Chandelier Exit）。\n"
                     "棘轮，只朝有利方向移动，绝不回退。\n"
                     "k 越小假突破越多，越大回吐越多。经典值 2.5~3.5。\n"
                     "表格「状态」列悬停可看当前 k 对应的实际止损距离%。");
    sigForm->addRow("ATR 倍数 k", w->k);

    // 两种模式各自只用到一部分参数。用不到的灰掉而不是藏起来：藏起来会让弹窗
    // 高度随模式跳变，而灰掉能让人看见"这个参数在另一种模式下才生效"
    auto* swingW = w->swing;
    auto* dcW    = w->dcPeriod;
    auto* atrW   = w->atrPeriod;
    auto* kW     = w->k;
    auto syncMode = [swingW, dcW, atrW, kW](int idx) {
        const bool bar = (idx == 1);
        swingW->setEnabled(bar);
        dcW->setEnabled(!bar);
        kW->setEnabled(!bar);
        // ATR 周期在裸K线模式下仍然有用：等风险下单的显示和金字塔加仓都要 ATR
        atrW->setEnabled(true);
    };
    syncMode(w->mode->currentIndex());
    connect(w->mode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            swingW, [syncMode](int i) { syncMode(i); });

    auto* note = new QLabel(
        "没有固定止盈，这是设计而非遗漏：趋势跟随胜率天然只有 30~40%，"
        "收益全来自少数几笔跑得很远的单子。固定止盈会砍断它们，"
        "而亏损笔大小不变——等于单方面砍掉盈利分布的右尾。");
    note->setStyleSheet("color:#8b949e;font-size:11px;");
    note->setWordWrap(true);
    sigForm->addRow(note);

    // ── 反手与加仓 ──────────────────────────────────────────────────────────
    w->rev = new QCheckBox("亏损止损后反向入场");
    w->rev->setChecked(c.rule.allow_reverse);
    w->rev->setToolTip("盈利出场【永不】反手——力竭不等于反转。这一项只管亏损出场。");
    revForm->addRow(w->rev);

    w->revSig = new QCheckBox("反手需要反向信号成立（强烈建议保持勾选）");
    w->revSig->setChecked(c.rule.reverse_needs_signal);
    w->revSig->setToolTip(
        "不勾 = 无条件反手，在震荡市是绞肉机：\n"
        "亏损止损本就在震荡市最频繁，而无条件反手恰好在那时最激进。\n"
        "开多→跌 k×ATR 止损→反手开空→涨回来 k×ATR 止损→反手开多…\n"
        "ATR 常态 2% 时单次绞杀约 6% 名义，3 倍杠杆即保证金的 18%。");
    revForm->addRow(w->revSig);

    w->maxRev = new QSpinBox();
    w->maxRev->setRange(0, 20);
    w->maxRev->setValue(c.rule.max_consecutive_reverses);
    w->maxRev->setToolTip("连续反手上限，超过即强制冷却。\n"
                          "真趋势不需要连续反手——连续反手本身就是"
                          "「现在是震荡市」的信号。");
    revForm->addRow("连续反手上限", w->maxRev);

    w->cooldown = new QSpinBox();
    w->cooldown->setRange(0, 100);
    w->cooldown->setValue(c.rule.cooldown_bars);
    w->cooldown->setToolTip("触顶后冷却多少根【K线】（不是 tick）。");
    revForm->addRow("冷却K线数", w->cooldown);

    w->pyrMax = new QSpinBox();
    w->pyrMax->setRange(0, 10);
    w->pyrMax->setValue(c.rule.pyramid_max_adds);
    w->pyrMax->setToolTip(
        "金字塔加仓档数（0=关）。探测仓开出来后，每朝有利方向再走若干个 ATR "
        "就加一档。\n\n"
        "它回答的是「怎么低成本试出单边大行情」：错了只亏第一档，对了越骑越重。\n"
        "与 DCA 的补仓方向【相反】——DCA 是跌了加（摊薄），这里是涨了加（顺势）。\n\n"
        "加仓不额外挪止损线：棘轮本来就跟着新高走，加仓时线已经在更高位置了。");
    revForm->addRow("顺势加仓档数", w->pyrMax);

    w->pyrStep = new QDoubleSpinBox();
    w->pyrStep->setRange(0.1, 10.0);
    w->pyrStep->setSingleStep(0.1);
    w->pyrStep->setDecimals(2);
    w->pyrStep->setValue(c.rule.pyramid_step_atr);
    w->pyrStep->setToolTip("每走多少个 ATR 加一档（海龟原版 0.5）。\n"
                           "间距从【上一档的成交价】量起，不是首档——否则越加越密。");
    revForm->addRow("加仓间距 (×ATR)", w->pyrStep);
    {
        auto* stepW = w->pyrStep;
        stepW->setEnabled(w->pyrMax->value() > 0);
        connect(w->pyrMax, QOverload<int>::of(&QSpinBox::valueChanged),
                stepW, [stepW](int v) { stepW->setEnabled(v > 0); });
    }

    return w;
}

bool MainWindow::collectSarForm(const std::shared_ptr<SarFormWidgets>& w, SarConfig& out) {
    if (!w) return false;

    // 从 base 起算：弹窗里没有控件的字段（signal_max_age_sec 等）必须从原配置
    // 继承，否则手改过 JSON 的值会被静默重置回默认
    out = w->base;
    out.budget_usdt = w->budget->value();
    out.leverage    = w->lev->value();
    out.interval    = w->interval->currentText().toStdString();
    out.size_mode   = (SarConfig::SizeMode)w->sizeMode->currentData().toInt();
    out.risk_usdt   = w->risk->value();
    out.rule.mode                     = (sar::Mode)w->mode->currentData().toInt();
    out.rule.swing_bars               = w->swing->value();
    out.rule.donchian_period          = w->dcPeriod->value();
    out.rule.atr_period               = w->atrPeriod->value();
    out.rule.atr_mult                 = w->k->value();
    out.rule.allow_reverse            = w->rev->isChecked();
    out.rule.reverse_needs_signal     = w->revSig->isChecked();
    out.rule.max_consecutive_reverses = w->maxRev->value();
    out.rule.cooldown_bars            = w->cooldown->value();
    out.rule.pyramid_max_adds         = w->pyrMax->value();
    out.rule.pyramid_step_atr         = w->pyrStep->value();

    if (out.size_mode == SarConfig::SizeMode::RiskBased && out.risk_usdt <= 0) {
        QMessageBox::warning(this, "缺少参数",
            "选择了「按 ATR 等风险」，但没有填「单次愿亏」。\n\n"
            "这个模式用 单次愿亏 ÷ 真实止损距离 反推仓位，没有它算不出任何数量。");
        return false;
    }
    return true;
}

// 校验 + 落地。返回 false = 已经向用户报过错，且【一点状态都没动】
bool MainWindow::applySarConfig(SarConfig c, const SarBot* existing) {
    if (!sar_engine_) {
        QMessageBox::information(this, "未连接", "请先连接交易所后再配置策略。");
        return false;
    }

    if (existing) {
        // 编辑已有：先删再建会丢掉持仓跟踪，所以有持仓时不允许改。
        // 提示要说清楚为什么，不能只是"不能改"
        if (existing->st.pos != sar::Pos::Flat) {
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
    if (existing) sar_engine_->remove_bot(existing->bot_id);

    const auto id = sar_engine_->add_bot(c);
    if (id.empty()) {
        QMessageBox::warning(this, "添加失败", "该品种已存在一个未停止的 SAR bot。");
        return false;
    }
    if (ticker_) ticker_->subscribe(c.symbol);
    sarSigForce_.store(true);   // 下一个 tick 立刻拉信号，不等满一整轮

    const bool bar_mode = (c.rule.mode == sar::Mode::BarPattern);
    log(QString("SAR %1 已配置：%2 / %3 / %4")
            .arg(QString::fromStdString(c.symbol))
            .arg(bar_mode ? QString("裸K线 摆动N=%1").arg(c.rule.swing_bars)
                          : QString("唐奇安%1 ATR%2 k=%3")
                                .arg(c.rule.donchian_period).arg(c.rule.atr_period)
                                .arg(c.rule.atr_mult, 0, 'f', 1))
            .arg(QString::fromStdString(c.interval))
            .arg(!c.rule.allow_reverse ? "不反手"
                 : c.rule.reverse_needs_signal ? "反手需信号" : "无条件反手"), "OK");
    refreshBotTable();
    save_sar_bots();
    return true;
}

// ── 持久化 ──────────────────────────────────────────────────────────────────
// 配置与运行时状态存在同一个文件里。DCA 那边分了两个文件（bots.json + 状态），
// 这里不分是因为 SAR 的运行时状态只有 6 个标量，单独一个文件不值当

std::string MainWindow::sar_cfg_path() const {
    return (portable_data_dir() + "/sar_bots.json").toStdString();
}

void MainWindow::save_sar_bots() {
    if (!sar_engine_) return;
    QJsonArray arr;
    for (const auto& b : sar_engine_->get_bots()) {
        QJsonObject o;
        o["symbol"]      = QString::fromStdString(b.cfg.symbol);
        o["budget_usdt"] = b.cfg.budget_usdt;
        o["leverage"]    = b.cfg.leverage;
        o["interval"]    = QString::fromStdString(b.cfg.interval);
        o["donchian_period"] = b.cfg.rule.donchian_period;
        o["atr_period"]      = b.cfg.rule.atr_period;
        o["atr_mult"]        = b.cfg.rule.atr_mult;
        o["allow_reverse"]   = b.cfg.rule.allow_reverse;
        o["reverse_needs_signal"] = b.cfg.rule.reverse_needs_signal;
        o["max_consecutive_reverses"] = b.cfg.rule.max_consecutive_reverses;
        o["cooldown_bars"]   = b.cfg.rule.cooldown_bars;
        o["size_mode"]       = (int)b.cfg.size_mode;
        o["risk_usdt"]       = b.cfg.risk_usdt;
        o["pyramid_max_adds"] = b.cfg.rule.pyramid_max_adds;
        o["pyramid_step_atr"] = b.cfg.rule.pyramid_step_atr;
        o["sar_mode"]        = (int)b.cfg.rule.mode;
        o["swing_bars"]      = b.cfg.rule.swing_bars;
        o["signal_max_age_sec"] = b.cfg.signal_max_age_sec;
        o["state"]           = (int)b.state;
        // ── 运行时状态 ──
        // Qt 的 QJsonValue(double) 是全精度往返，不像 ostringstream 会截到 6 位。
        // 这点在 SAR 上尤其要紧：止损线【就是】出场价
        o["pos"]             = (int)b.st.pos;
        o["entry_price"]     = b.st.entry_price;
        o["peak"]            = b.st.peak;
        o["stop"]            = b.st.stop;
        o["consec_reverses"] = b.st.consec_reverses;
        o["cooldown_left"]   = b.st.cooldown_left;
        o["adds_done"]       = b.st.adds_done;
        o["last_add_price"]  = b.st.last_add_price;
        o["qty"]             = b.qty;
        o["realized_pnl"]    = b.realized_pnl;
        o["trade_count"]     = b.trade_count;
        o["win_count"]       = b.win_count;
        arr.append(o);
    }
    // QSaveFile = 原子写（写临时文件 + commit 时改名）。崩在写入中途不会
    // 留下半截 JSON——而半截 JSON 丢掉的正是那条止损线
    QSaveFile f(QString::fromStdString(sar_cfg_path()));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    f.commit();
}

void MainWindow::load_and_restore_sar() {
    if (!sar_engine_) return;
    QFile f(QString::fromStdString(sar_cfg_path()));
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
            log(QString("SAR 品种 %1 缺少 USDT 后缀，已自动更正为 %2"
                        "（币安合约的代码形如 BTCUSDT）").arg(sym, fixed), "WARN");
            sym = fixed;
        }

        SarBot b;
        b.cfg.symbol      = sym.toStdString();
        b.cfg.budget_usdt = o["budget_usdt"].toDouble(1000);
        b.cfg.leverage    = o["leverage"].toInt(3);
        b.cfg.interval    = o["interval"].toString("4h").toStdString();
        b.cfg.rule.donchian_period = o["donchian_period"].toInt(20);
        b.cfg.rule.atr_period      = o["atr_period"].toInt(14);
        b.cfg.rule.atr_mult        = o["atr_mult"].toDouble(3.0);
        b.cfg.rule.allow_reverse   = o["allow_reverse"].toBool(true);
        b.cfg.rule.reverse_needs_signal = o["reverse_needs_signal"].toBool(true);
        b.cfg.rule.max_consecutive_reverses = o["max_consecutive_reverses"].toInt(2);
        b.cfg.rule.cooldown_bars   = o["cooldown_bars"].toInt(3);
        b.cfg.size_mode = (SarConfig::SizeMode)o["size_mode"].toInt(0);
        b.cfg.risk_usdt = o["risk_usdt"].toDouble(0);
        b.cfg.rule.pyramid_max_adds = o["pyramid_max_adds"].toInt(0);
        b.cfg.rule.pyramid_step_atr = o["pyramid_step_atr"].toDouble(0.5);
        b.cfg.rule.mode = (sar::Mode)o["sar_mode"].toInt(0);
        b.cfg.rule.swing_bars = o["swing_bars"].toInt(3);
        b.cfg.signal_max_age_sec   = o["signal_max_age_sec"].toInt(900);
        b.state = (SarBot::State)o["state"].toInt(0);

        const int pos = o["pos"].toInt((int)sar::Pos::Flat);
        b.st.pos = (pos == (int)sar::Pos::Long)  ? sar::Pos::Long
                 : (pos == (int)sar::Pos::Short) ? sar::Pos::Short
                                                 : sar::Pos::Flat;
        b.st.entry_price     = o["entry_price"].toDouble(0);
        b.st.peak            = o["peak"].toDouble(0);
        b.st.stop            = o["stop"].toDouble(0);
        b.st.consec_reverses = o["consec_reverses"].toInt(0);
        b.st.cooldown_left   = o["cooldown_left"].toInt(0);
        b.st.adds_done       = o["adds_done"].toInt(0);
        b.st.last_add_price  = o["last_add_price"].toDouble(0);
        b.qty          = o["qty"].toDouble(0);
        b.realized_pnl = o["realized_pnl"].toDouble(0);
        b.trade_count  = o["trade_count"].toInt(0);
        b.win_count    = o["win_count"].toInt(0);

        // 半截状态防御，与 headless 侧同款：有方向却缺开仓价/止损线/数量的，
        // 当成空仓丢弃。stop=0 时多头的 price<=stop 永远不成立，
        // 仓位会一直裸着没人管——比"空仓"危险得多。交给对账去发现真实仓位
        if (b.st.pos != sar::Pos::Flat &&
            (b.st.entry_price <= 0 || b.st.stop <= 0 || b.qty <= 0)) {
            b.st = sar::State{};
            b.qty = 0;
        }

        // 跨引擎冲突：弹窗里两个方向都拦了，但【恢复路径没有】——落盘文件是
        // 手工改过的、或者两套配置在不同版本里先后加上的，就会漏进来。
        // 两个引擎接管同一个交易所仓位会互相平掉对方的单，宁可不恢复
        bool taken_by_dca = false;
        if (engine_) {
            for (const auto& db : engine_->get_bots())
                if (db.cfg.symbol == b.cfg.symbol &&
                    db.state != CcgBot::State::Stopped) { taken_by_dca = true; break; }
        }
        if (taken_by_dca) {
            log(QString("SAR %1 未恢复：该品种已由网格 DCA 接管。"
                        "两套策略会在同一个交易所仓位上互相平掉对方的单，必须二选一")
                    .arg(QString::fromStdString(b.cfg.symbol)), "WARN");
            continue;
        }

        if (!sar_engine_->restore_bot(b).empty()) {
            ++n;
            if (ticker_) ticker_->subscribe(b.cfg.symbol);
        }
    }
    if (n > 0) {
        log(QString("已恢复 %1 个 SAR bot").arg(n), "OK");
        sarSigForce_.store(true);
    }
    refreshBotTable();
}

} // namespace ccg
