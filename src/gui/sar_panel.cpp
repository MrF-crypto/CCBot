// MainWindow 的 SAR 标签页：构建、刷新、配置弹窗、持久化。
//
// 单独一个翻译单元，不塞进 main_window.cpp——那个文件已经 3800+ 行，
// 而且是本项目历史上几乎每一个 GUI bug 的出处。SAR 的界面代码与 DCA 的
// 完全没有共享状态，没有理由再往里堆。
#include "gui/main_window.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace ccg {

namespace {

// 表格列。和 DCA 那张表刻意不对齐：SAR 没有层、没有均价、没有强平价
// （名义仓位远小于权益，强平价没有意义），而止损线和它离现价多远才是全部
enum SarCol {
    C_IDX = 0, C_SYM, C_DIR, C_STATE, C_ATR, C_ENTRY, C_MARK,
    C_STOP, C_DIST, C_UPNL, C_REAL, C_WIN, C_REV, C_DECISION, C_OP,
    C_COUNT
};

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

} // namespace

QWidget* MainWindow::buildSarTab() {
    auto* w  = new QWidget();
    auto* v  = new QVBoxLayout(w);
    v->setSpacing(4);

    // 加品种行
    {
        auto* row = new QHBoxLayout();
        addSarEdit_ = new QLineEdit();
        addSarEdit_->setPlaceholderText("品种代码，如 BTCUSDT");
        addSarEdit_->setMaximumWidth(200);
        connect(addSarEdit_, &QLineEdit::returnPressed, this, &MainWindow::onAddSarSymbol);
        row->addWidget(addSarEdit_);

        auto* btnAdd = new QPushButton("添加 SAR 品种");
        btnAdd->setStyleSheet("QPushButton{padding:3px 10px;}");
        connect(btnAdd, &QPushButton::clicked, this, &MainWindow::onAddSarSymbol);
        row->addWidget(btnAdd);
        row->addStretch(1);
        v->addLayout(row);
    }

    sarSummary_ = new QLabel("运行中: 0   持仓: 0   |   浮动: $0.00   已实现: $0.00");
    sarSummary_->setStyleSheet(
        "QLabel{color:#8b949e;font-size:11px;padding:4px 8px;"
        "background:#161b22;border:1px solid #21262d;border-radius:3px;}");
    v->addWidget(sarSummary_);

    auto* hint = new QLabel(
        "趋势跟随 + 止损反转  (右键品种进行策略配置)　"
        "—— 唐奇安通道突破入场，ATR 追踪止损出场，无固定止盈");
    hint->setStyleSheet("color:#58a6ff;font-size:11px;font-weight:bold;padding:2px 0;");
    v->addWidget(hint);

    sarTable_ = new QTableWidget(0, C_COUNT);
    sarTable_->setHorizontalHeaderLabels(
        {"#", "品种", "方向", "状态", "ATR", "开仓价", "标记价",
         "止损线", "距离", "浮动P&L", "已实现", "胜率", "反手", "决策", "操作"});
    auto* hdr = sarTable_->horizontalHeader();
    hdr->setSectionResizeMode(QHeaderView::Stretch);
    for (int c : {C_IDX, C_DIR, C_ATR, C_DIST, C_WIN, C_REV})
        hdr->setSectionResizeMode(c, QHeaderView::Fixed);
    hdr->resizeSection(C_IDX, 26);
    hdr->resizeSection(C_DIR, 44);
    hdr->resizeSection(C_ATR, 62);
    hdr->resizeSection(C_DIST, 60);
    hdr->resizeSection(C_WIN, 64);
    hdr->resizeSection(C_REV, 44);
    hdr->setSectionResizeMode(C_DECISION, QHeaderView::Stretch);
    hdr->setSectionResizeMode(C_OP, QHeaderView::Fixed);
    hdr->resizeSection(C_OP, 150);

    sarTable_->verticalHeader()->setVisible(false);
    sarTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sarTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    sarTable_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    sarTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sarTable_, &QTableWidget::customContextMenuRequested,
            this, &MainWindow::onSarContextMenu);
    v->addWidget(sarTable_, 1);

    return w;
}

void MainWindow::refreshSarTable() {
    if (!sarTable_) return;
    if (!sar_engine_) {
        sarTable_->setRowCount(0);
        sarOpRowKeys_.clear();
        return;
    }

    auto bots = sar_engine_->get_bots();
    if ((int)bots.size() != sarTable_->rowCount()) {
        sarTable_->setRowCount((int)bots.size());
        sarOpRowKeys_.assign(bots.size(), QString());
    }

    int running = 0, holding = 0;
    double total_upnl = 0, total_real = 0;

    for (int i = 0; i < (int)bots.size(); ++i) {
        const auto& b = bots[i];
        const bool is_long = (b.st.pos == sar::Pos::Long);
        const bool has_pos = (b.st.pos != sar::Pos::Flat && b.qty > 0);
        if (b.state == SarBot::State::Running) ++running;
        if (has_pos) ++holding;

        double upnl = 0;
        if (has_pos && b.current_price > 0)
            upnl = (b.current_price - b.st.entry_price) * b.qty * (is_long ? 1.0 : -1.0);
        total_upnl += upnl;
        total_real += b.realized_pnl;

        sarTable_->setItem(i, C_IDX, mk(QString::number(i + 1)));
        sarTable_->setItem(i, C_SYM, mk(QString::fromStdString(b.cfg.symbol)));
        // 方向列带上金字塔档数：持 3 档和持 1 档的敞口差 3 倍，
        // 不显示的话你看不出这个仓位到底压了多重
        QString dir_txt = has_pos ? (is_long ? "多" : "空") : "—";
        if (has_pos && b.cfg.rule.pyramid_max_adds > 0)
            dir_txt += QString("×%1").arg(b.st.adds_done + 1);
        auto* dir_it = mk(dir_txt,
                          has_pos ? (is_long ? "#3fb950" : "#f85149") : "#8b949e");
        if (has_pos && b.cfg.rule.pyramid_max_adds > 0)
            dir_it->setToolTip(QString("已加到 %1/%2 档（首档 + %3 次加仓）")
                                   .arg(b.st.adds_done + 1)
                                   .arg(b.cfg.rule.pyramid_max_adds + 1)
                                   .arg(b.st.adds_done));
        sarTable_->setItem(i, C_DIR, dir_it);

        QString st = (b.state == SarBot::State::Stopped) ? "已停止"
                   : b.pending                            ? "下单中"
                   : has_pos                              ? "持仓中"
                   : (b.st.cooldown_left > 0)             ? "冷却中"
                                                          : "等信号";
        sarTable_->setItem(i, C_STATE,
            mk(st, b.state == SarBot::State::Stopped ? "#f85149"
                 : has_pos                           ? "#58a6ff" : "#8b949e"));

        // ATR 列同时是"信号到没到"的指示灯：显示 — 就是这个品种的 K 线没拉到，
        // 而没有 ATR 就没有止损线，引擎绝不会开新仓。这一列存在的全部理由，
        // 就是让"等信号"和"拉不到数据"在界面上长得不一样
        auto* atr_it = mk(b.atr_pct > 0 ? QString::number(b.atr_pct, 'f', 2) + "%" : "—",
                          b.atr_pct > 0 ? "#8b949e" : "#f85149");
        if (b.atr_pct > 0) {
            atr_it->setToolTip(
                QString("%1 周期 ATR = %2%（跨品种可比口径）\n"
                        "当前 k=%3 ⇒ 止损距离约 %4%")
                    .arg(QString::fromStdString(b.cfg.interval))
                    .arg(b.atr_pct, 0, 'f', 2)
                    .arg(b.cfg.rule.atr_mult, 0, 'f', 1)
                    .arg(b.atr_pct * b.cfg.rule.atr_mult, 0, 'f', 1));
        } else {
            atr_it->setToolTip("尚未拉到 K 线数据。没有 ATR 就没有止损线，"
                               "引擎不会开新仓。\n"
                               "刚添加的品种最多等 60 秒；持续显示 — 请看日志里的"
                               "「SAR 信号拉取失败」告警");
        }
        sarTable_->setItem(i, C_ATR, atr_it);

        sarTable_->setItem(i, C_ENTRY, mk(has_pos ? fmt_px(b.st.entry_price) : "—"));
        sarTable_->setItem(i, C_MARK,  mk(fmt_px(b.current_price)));
        sarTable_->setItem(i, C_STOP,  mk(has_pos ? fmt_px(b.st.stop) : "—", "#d29922"));

        // 距离：现价离止损线还有多远。这是持仓时最该盯的一个数——
        // 它就是"现在被打掉会亏/赚多少"
        QString dist = "—";
        QString dist_col = "#8b949e";
        if (has_pos && b.current_price > 0 && b.st.stop > 0) {
            const double d = std::fabs(b.current_price - b.st.stop) / b.current_price * 100.0;
            dist = QString::number(d, 'f', 2) + "%";
            // 止损线已经越过成本价 = 这笔单子已经锁定盈利，绿色标出来
            const bool locked = is_long ? (b.st.stop >= b.st.entry_price)
                                        : (b.st.stop <= b.st.entry_price);
            dist_col = locked ? "#3fb950" : "#8b949e";
        }
        sarTable_->setItem(i, C_DIST, mk(dist, dist_col));

        sarTable_->setItem(i, C_UPNL,
            mk(has_pos ? QString("%1%2").arg(upnl >= 0 ? "+" : "").arg(upnl, 0, 'f', 2) : "—",
               upnl > 0 ? "#3fb950" : upnl < 0 ? "#f85149" : "#8b949e"));
        sarTable_->setItem(i, C_REAL,
            mk(QString("%1%2").arg(b.realized_pnl >= 0 ? "+" : "").arg(b.realized_pnl, 0, 'f', 2),
               b.realized_pnl > 0 ? "#3fb950" : b.realized_pnl < 0 ? "#f85149" : "#8b949e"));

        // 胜率：趋势跟随天然只有 30~40%，低不代表策略坏了。把分子分母都摆出来，
        // 免得只看一个百分比就急着改参数
        QString win = "—";
        if (b.trade_count > 0)
            win = QString("%1/%2").arg(b.win_count).arg(b.trade_count);
        sarTable_->setItem(i, C_WIN, mk(win));
        sarTable_->setItem(i, C_REV,
            mk(b.st.consec_reverses > 0 ? QString::number(b.st.consec_reverses) : "—",
               b.st.consec_reverses >= 2 ? "#d29922" : "#8b949e"));

        auto* dec = mk(QString::fromStdString(b.last_decision));
        dec->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        dec->setToolTip(QString::fromStdString(b.last_action));
        sarTable_->setItem(i, C_DECISION, dec);

        // 操作列：键没变就不重建控件，否则每 3 秒重建一次会把点击吞掉
        const QString key = QString("%1|%2|%3|%4")
                                .arg(QString::fromStdString(b.bot_id))
                                .arg((int)b.state).arg(has_pos).arg(b.pending);
        if (i < (int)sarOpRowKeys_.size() && sarOpRowKeys_[i] == key) continue;
        if (i < (int)sarOpRowKeys_.size()) sarOpRowKeys_[i] = key;

        auto* opw = new QWidget();
        auto* opl = new QHBoxLayout(opw);
        opl->setContentsMargins(2, 0, 2, 0);
        opl->setSpacing(3);
        const std::string id = b.bot_id;

        if (b.state == SarBot::State::Running) {
            auto* bp = new QPushButton("暂停");
            bp->setStyleSheet("QPushButton{padding:1px 6px;font-size:11px;}");
            connect(bp, &QPushButton::clicked, this, [this, id]() {
                if (sar_engine_) { sar_engine_->stop_bot(id); refreshSarTable(); save_sar_bots(); }
            });
            opl->addWidget(bp);
        } else {
            auto* br = new QPushButton("恢复");
            br->setStyleSheet("QPushButton{padding:1px 6px;font-size:11px;color:#3fb950;}");
            connect(br, &QPushButton::clicked, this, [this, id]() {
                if (sar_engine_) { sar_engine_->resume_bot(id); refreshSarTable(); save_sar_bots(); }
            });
            opl->addWidget(br);
        }

        if (has_pos && !b.pending) {
            auto* bc = new QPushButton("平仓");
            bc->setStyleSheet("QPushButton{padding:1px 6px;font-size:11px;color:#d29922;}");
            const QString sym = QString::fromStdString(b.cfg.symbol);
            connect(bc, &QPushButton::clicked, this, [this, id, sym]() {
                if (!sar_engine_) return;
                if (!confirmDanger("确认平仓", sym + " 将以市价立即平掉当前 SAR 仓位。",
                                   "平仓")) return;
                sar_engine_->close_bot(id);
            });
            opl->addWidget(bc);
        }

        auto* bd = new QPushButton("删除");
        bd->setStyleSheet("QPushButton{padding:1px 6px;font-size:11px;color:#f85149;}");
        const QString sym = QString::fromStdString(b.cfg.symbol);
        const bool pos_warn = has_pos;
        connect(bd, &QPushButton::clicked, this, [this, id, sym, pos_warn]() {
            if (!sar_engine_) return;
            // 有持仓时删除 = 交易所上留下一笔【没有止损线守着】的裸仓位。
            // 必须说清楚，不能只问"确定删除吗"
            const QString body = pos_warn
                ? sym + " 当前有持仓。删除后程序不再跟踪它，"
                        "交易所上的仓位会失去追踪止损的保护，需要你手动处理。"
                : sym + " 将从 SAR 列表中移除。";
            if (!confirmDanger("确认删除", body, "删除")) return;
            sar_engine_->remove_bot(id);
            unsubscribeIfUnused(sym.toStdString());
            refreshSarTable();
            save_sar_bots();
        });
        opl->addWidget(bd);
        opl->addStretch(1);
        sarTable_->setCellWidget(i, C_OP, opw);
    }

    if (sarSummary_) {
        sarSummary_->setText(
            QString("运行中: %1   持仓: %2   |   浮动: %3$%4   已实现: %5$%6")
                .arg(running).arg(holding)
                .arg(total_upnl >= 0 ? "+" : "").arg(std::fabs(total_upnl), 0, 'f', 2)
                .arg(total_real >= 0 ? "+" : "").arg(std::fabs(total_real), 0, 'f', 2));
    }
}

void MainWindow::onAddSarSymbol() {
    if (!addSarEdit_) return;
    QString raw = addSarEdit_->text().trimmed().toUpper();
    addSarEdit_->clear();
    if (raw.isEmpty()) return;

    // 与 DCA 的加品种框同款补全：只输代币符号即可，默认 USDT 永续。
    // 不补全的话 "BTC" 会被原样拿去请求 K 线——币安合约没有这个交易对，
    // 于是永远拉不到数据、永远"等信号"，而界面上看不出任何异常
    if (raw.endsWith("USDT")) raw.chop(4);
    if (raw.isEmpty()) return;
    openSarDialog((raw + "USDT").toStdString());
}

void MainWindow::onSarContextMenu(const QPoint& pos) {
    if (!sarTable_ || !sar_engine_) return;
    auto* item = sarTable_->itemAt(pos);
    if (!item) return;
    const int row = item->row();
    auto bots = sar_engine_->get_bots();
    if (row < 0 || row >= (int)bots.size()) return;

    QMenu menu(this);
    auto* act = menu.addAction("策略配置…");
    if (menu.exec(sarTable_->viewport()->mapToGlobal(pos)) == act)
        openSarDialog(bots[row].cfg.symbol);
}

void MainWindow::openSarDialog(const std::string& symbol) {
    if (!sar_engine_) {
        QMessageBox::information(this, "未连接", "请先连接交易所后再配置 SAR 策略。");
        return;
    }

    // 已存在同品种则是编辑，否则是新建
    const SarBot* existing = nullptr;
    auto bots = sar_engine_->get_bots();
    for (const auto& b : bots)
        if (b.cfg.symbol == symbol) { existing = &b; break; }

    SarConfig c;
    c.symbol = symbol;
    if (existing) c = existing->cfg;

    QDialog dlg(this);
    dlg.setWindowTitle(QString("SAR 策略配置 — %1").arg(QString::fromStdString(symbol)));
    dlg.setMinimumWidth(480);
    dlg.resize(520, 700);

    // 滚动区 + 固定在底部的按钮。参数长到一屏放不下时，没有滚动区会把
    // "创建/保存"顶出屏幕外——DCA 的弹窗当初就是因为这个才改的，这里别重犯
    auto* outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget();
    auto* form = new QFormLayout(inner);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    // ⚠ 仓位算法排在最前面：它决定了下面两个框各自的含义（名义价值 vs 名义上限），
    //   放在后面的话用户会先填完再发现填错了地方
    auto* sizeMode = new QComboBox();
    sizeMode->addItem("固定名义", (int)SarConfig::SizeMode::Notional);
    sizeMode->addItem("按 ATR 等风险", (int)SarConfig::SizeMode::RiskBased);
    sizeMode->setCurrentIndex(c.size_mode == SarConfig::SizeMode::RiskBased ? 1 : 0);
    sizeMode->setToolTip(
        "固定名义：每笔都是同样的名义价值。\n"
        "等风险：名义 = 单次愿亏 / (k×ATR%)，每笔止损亏的钱固定。\n\n"
        "为什么需要等风险：同一份名义，在 4h ATR 1.6% 的 LTC 和 5.3% 的 COTI 上，"
        "单次止损亏的钱差 3.3 倍。固定名义 = 风险全压在高波动那几个品种上，"
        "而「铺开品种分散风险」就此失效。这是海龟的「单位」概念。");
    form->addRow("仓位算法", sizeMode);

    auto* budget = new QDoubleSpinBox();
    budget->setRange(10, 10'000'000);
    budget->setDecimals(2);
    budget->setValue(c.budget_usdt);
    budget->setToolTip("固定名义模式：每笔仓位的名义价值。\n"
                       "等风险模式：名义价值的【上限】（ATR 极小时兜住公式算出的天量仓位）。");
    auto* budgetLabel = new QLabel();
    form->addRow(budgetLabel, budget);

    auto* riskEdit = new QDoubleSpinBox();
    riskEdit->setRange(0, 1'000'000);
    riskEdit->setDecimals(2);
    riskEdit->setValue(c.risk_usdt);
    // 值为 0（= 最小值）时显示这行字而不是"0.00"。此前它是一个灰掉的"0"，
    // 看起来像"这个功能坏了"，而真实原因只是仓位算法还没切过去
    riskEdit->setSpecialValueText("未设置");
    riskEdit->setToolTip("单次止损愿意亏多少钱（USDT）。仅「按 ATR 等风险」模式使用。\n"
                         "账户 10000U、每次探测愿亏 1% ⇒ 填 100。");
    auto* riskLabel = new QLabel();
    form->addRow(riskLabel, riskEdit);

    // 两个框的标签和可用性都跟着算法走。写成 lambda 是因为初始化和切换时
    // 要做完全相同的事——分开写必然有一天只改一处
    auto syncSizeMode = [budget, budgetLabel, riskEdit, riskLabel](int idx) {
        const bool risk = (idx == 1);
        budgetLabel->setText(risk ? "　名义上限 (USDT)" : "　仓位名义价值 (USDT)");
        riskLabel->setText(risk ? "　单次愿亏 (USDT)"
                                : "　单次愿亏（切到「按 ATR 等风险」后可填）");
        riskLabel->setEnabled(risk);
        riskEdit->setEnabled(risk);
        if (risk && riskEdit->value() <= 0) {
            riskEdit->setFocus();
            riskEdit->selectAll();
        }
    };
    syncSizeMode(sizeMode->currentIndex());
    connect(sizeMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            riskEdit, [syncSizeMode](int i) { syncSizeMode(i); });

    auto* lev = new QSpinBox();
    lev->setRange(1, 125);
    lev->setValue(c.leverage);
    form->addRow("杠杆", lev);

    // 入场/止损算法：两套成套的组合，不是可以混搭的两个旋钮
    auto* modeBox = new QComboBox();
    modeBox->addItem("唐奇安突破 + ATR 止损", (int)sar::Mode::Donchian);
    modeBox->addItem("裸K线 + 摆动止损",       (int)sar::Mode::BarPattern);
    modeBox->setCurrentIndex(c.rule.mode == sar::Mode::BarPattern ? 1 : 0);
    modeBox->setToolTip(
        "唐奇安突破：价格创 N 根新高/新低入场，止损用 k×ATR 的棘轮线。海龟原版。\n\n"
        "裸K线：刚收盘那根是阳线就做多、阴线就做空；止损用【它之前 N 根】的\n"
        "最低价（做多）/ 最高价（做空），窗口随K线右移，棘轮只朝有利方向。\n"
        "入场只在K线收盘那一拍判定，不看盘中。");
    form->addRow("入场/止损算法", modeBox);

    auto* swingEdit = new QSpinBox();
    swingEdit->setRange(1, 50);
    swingEdit->setValue(c.rule.swing_bars);
    swingEdit->setToolTip(
        "止损用最近几根的最低/最高价。信号根【不算】在内。\n\n"
        "⚠ 这个数直接决定持仓时长和交易频率：随机游走下平均持仓约 N+1 根。\n"
        "N=3 ⇒ 4 根就被打掉一次。配短周期时交易次数会非常高。");
    form->addRow("摆动止损根数", swingEdit);

    auto* itv = new QComboBox();
    itv->addItems({"3m", "5m", "15m", "30m", "1h", "4h", "12h", "1d"});
    itv->setCurrentText(QString::fromStdString(c.interval));
    itv->setToolTip("信号K线周期。周期越短信号越多，假突破也越多。");
    form->addRow("信号周期", itv);

    auto* dcp = new QSpinBox();
    dcp->setRange(2, 200);
    dcp->setValue(c.rule.donchian_period);
    dcp->setToolTip("唐奇安通道周期（海龟原版 20）。\n"
                    "上破 N 根最高 → 做多，下破 N 根最低 → 做空。\n"
                    "通道只由已收盘K线构成，当前这根去撞它。");
    form->addRow("通道周期", dcp);

    auto* atrp = new QSpinBox();
    atrp->setRange(2, 100);
    atrp->setValue(c.rule.atr_period);
    form->addRow("ATR 周期", atrp);

    auto* k = new QDoubleSpinBox();
    k->setRange(0.5, 20.0);
    k->setSingleStep(0.5);
    k->setDecimals(1);
    k->setValue(c.rule.atr_mult);
    k->setToolTip("止损距离 = k × ATR（Chandelier Exit）。\n"
                  "棘轮，只朝有利方向移动，绝不回退。\n"
                  "k 越小假突破越多，越大回吐越多。经典值 2.5~3.5。\n"
                  "表格里的 ATR 列悬停可看当前 k 对应的实际止损距离%。");
    form->addRow("ATR 倍数 k", k);

    auto* rev = new QCheckBox("亏损止损后反向入场");
    rev->setChecked(c.rule.allow_reverse);
    rev->setToolTip("盈利出场【永不】反手——力竭不等于反转。这一项只管亏损出场。");
    form->addRow(rev);

    auto* revSig = new QCheckBox("反手需要反向信号成立（强烈建议保持勾选）");
    revSig->setChecked(c.rule.reverse_needs_signal);
    revSig->setToolTip(
        "不勾 = 无条件反手，在震荡市是绞肉机：\n"
        "亏损止损本就在震荡市最频繁，而无条件反手恰好在那时最激进。\n"
        "开多→跌 k×ATR 止损→反手开空→涨回来 k×ATR 止损→反手开多…\n"
        "ATR 常态 2% 时单次绞杀约 6% 名义，3 倍杠杆即保证金的 18%。");
    form->addRow(revSig);

    auto* maxRev = new QSpinBox();
    maxRev->setRange(0, 20);
    maxRev->setValue(c.rule.max_consecutive_reverses);
    maxRev->setToolTip("连续反手上限，超过即强制冷却。\n"
                       "真趋势不需要连续反手——连续反手本身就是「现在是震荡市」的信号。");
    form->addRow("连续反手上限", maxRev);

    auto* cd = new QSpinBox();
    cd->setRange(0, 100);
    cd->setValue(c.rule.cooldown_bars);
    cd->setToolTip("触顶后冷却多少根【K线】（不是 tick）。");
    form->addRow("冷却K线数", cd);

    auto* pyrMax = new QSpinBox();
    pyrMax->setRange(0, 10);
    pyrMax->setValue(c.rule.pyramid_max_adds);
    pyrMax->setToolTip(
        "金字塔加仓档数（0=关）。探测仓开出来后，每朝有利方向再走若干个 ATR "
        "就加一档。\n\n"
        "它回答的是「怎么低成本试出单边大行情」：错了只亏第一档，对了越骑越重。\n"
        "与 DCA 的补仓方向【相反】——DCA 是跌了加（摊薄），这里是涨了加（顺势）。\n\n"
        "加仓不额外挪止损线：ATR 棘轮本来就跟着新高走，加仓时线已经在更高位置了。");
    form->addRow("顺势加仓档数", pyrMax);

    auto* pyrStep = new QDoubleSpinBox();
    pyrStep->setRange(0.1, 10.0);
    pyrStep->setSingleStep(0.1);
    pyrStep->setDecimals(2);
    pyrStep->setValue(c.rule.pyramid_step_atr);
    pyrStep->setToolTip("每走多少个 ATR 加一档（海龟原版 0.5）。\n"
                        "间距从【上一档的成交价】量起，不是首档——否则越加越密。");
    form->addRow("加仓间距 (×ATR)", pyrStep);
    for (QWidget* w : {(QWidget*)pyrStep}) {
        w->setEnabled(pyrMax->value() > 0);
        connect(pyrMax, QOverload<int>::of(&QSpinBox::valueChanged),
                w, [w](int v) { w->setEnabled(v > 0); });
    }

    auto* note = new QLabel(
        "没有固定止盈，这是设计而非遗漏：趋势跟随胜率天然只有 30~40%，\n"
        "收益全来自少数几笔跑得很远的单子。固定止盈会砍断它们，\n"
        "而亏损笔大小不变——等于单方面砍掉盈利分布的右尾。");
    note->setStyleSheet("color:#8b949e;font-size:11px;");
    note->setWordWrap(true);
    form->addRow(note);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(existing ? "保存修改" : "创建");
    bb->setContentsMargins(9, 6, 9, 9);
    outer->addWidget(bb);   // 在滚动区【外面】，永远可见
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    c.budget_usdt = budget->value();
    c.leverage    = lev->value();
    c.interval    = itv->currentText().toStdString();
    c.rule.donchian_period = dcp->value();
    c.rule.atr_period      = atrp->value();
    c.rule.atr_mult        = k->value();
    c.rule.allow_reverse   = rev->isChecked();
    c.rule.reverse_needs_signal = revSig->isChecked();
    c.rule.max_consecutive_reverses = maxRev->value();
    c.rule.cooldown_bars   = cd->value();
    c.rule.mode       = (sar::Mode)modeBox->currentData().toInt();
    c.rule.swing_bars = swingEdit->value();
    c.size_mode = (SarConfig::SizeMode)sizeMode->currentData().toInt();
    c.risk_usdt = riskEdit->value();
    c.rule.pyramid_max_adds = pyrMax->value();
    c.rule.pyramid_step_atr = pyrStep->value();

    if (c.size_mode == SarConfig::SizeMode::RiskBased && c.risk_usdt <= 0) {
        QMessageBox::warning(this, "缺少参数",
            "选择了「按 ATR 等风险」，但没有填「单次愿亏」。\n\n"
            "这个模式用 单次愿亏 ÷ (k×ATR) 反推仓位，没有它算不出任何数量。");
        return;
    }

    if (existing) {
        // 编辑已有：先删再建会丢掉持仓跟踪，所以有持仓时不允许改。
        // 提示要说清楚为什么，不能只是"不能改"
        if (existing->st.pos != sar::Pos::Flat) {
            QMessageBox::warning(this, "有持仓，无法修改",
                QString::fromStdString(symbol) +
                " 当前有持仓。改参数会重建止损线基准，"
                "可能让线瞬间跳到现价另一侧而立刻触发平仓。\n\n"
                "请先平仓，再修改参数。");
            return;
        }
        sar_engine_->remove_bot(existing->bot_id);
    }

    // 同品种已被 DCA 接管则拒绝：两个引擎会在同一个交易所仓位上互相平掉对方的单
    if (engine_) {
        for (const auto& b : engine_->get_bots())
            if (b.cfg.symbol == symbol && b.state != CcgBot::State::Stopped) {
                QMessageBox::warning(this, "品种冲突",
                    QString::fromStdString(symbol) +
                    " 已经由网格 DCA 策略接管。\n\n"
                    "两套策略会在同一个交易所仓位上互相平掉对方的单——"
                    "SAR 的 reduceOnly 平仓会平掉 DCA 的层，反之亦然。\n"
                    "必须二选一。");
                return;
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
            return;
        }
    }

    const auto id = sar_engine_->add_bot(c);
    if (id.empty()) {
        QMessageBox::warning(this, "添加失败", "该品种已存在一个未停止的 SAR bot。");
        return;
    }
    if (ticker_) ticker_->subscribe(c.symbol);
    sarSigForce_.store(true);   // 下一个 tick 立刻拉信号，不等满 20 拍
    log(QString("SAR %1 已配置：通道%2 / ATR%3 / k=%4 / %5")
            .arg(QString::fromStdString(c.symbol))
            .arg(c.rule.donchian_period).arg(c.rule.atr_period)
            .arg(c.rule.atr_mult, 0, 'f', 1)
            .arg(c.rule.reverse_needs_signal ? "反手需信号" : "无条件反手"), "OK");
    refreshSarTable();
    save_sar_bots();
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
    refreshSarTable();
}

} // namespace ccg
