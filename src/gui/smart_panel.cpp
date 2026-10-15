// 智能模式的界面部分：设置弹窗、候选名单、顶部徽标、调度。
// 规则与执行都在 core/smart_mode + app/smart_runner（GUI 与 headless 共用），
// 这里只负责"什么时候调"和"怎么显示"。
#include "gui/main_window.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <tuple>

namespace ccg {

std::string MainWindow::smart_cfg_path() const {
    return (portable_data_dir() + "/ccg_smart.json").toStdString();
}
std::string MainWindow::smart_state_path() const {
    return (portable_data_dir() + "/ccg_smart_state.json").toStdString();
}

void MainWindow::save_smart_cfg() const {
    QSaveFile f(QString::fromStdString(smart_cfg_path()));
    if (!f.open(QIODevice::WriteOnly)) return;
    const auto s = smart::config_to_json(smartCfg_);
    f.write(s.data(), (qint64)s.size());
    f.commit();
}

void MainWindow::load_smart_cfg() {
    QFile f(QString::fromStdString(smart_cfg_path()));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = f.readAll();
    std::string err;
    smart::Config c;
    if (smart::config_from_json(std::string_view(raw.constData(), (size_t)raw.size()), c, &err)) {
        smartCfg_ = c;
    } else {
        // 坏文件不能静默当成"关"——那等于用户以为在跑、其实没跑
        log(QString("⚠ 智能模式配置读取失败（%1），已按关闭处理，请重新设置")
                .arg(QString::fromStdString(err)), "WARN");
        smartCfg_.enabled = false;
    }
}

void MainWindow::startSmartRunner() {
    if (!trend_engine_ || !client_) return;
    // 断线重连：旧运行器持有旧引擎，它若有一轮扫描在跑，作废掉——不落地、不写文件
    if (smart_) smart_->cancel();
    smart_ = std::make_shared<SmartRunner>(
        client_, trend_engine_, smart_state_path(),
        [this](const std::string& m, bool warn) {
            QMetaObject::invokeMethod(this, [this, m, warn]() {
                log(QString::fromStdString(m), warn ? "WARN" : "INFO");
                if (warn) sendAlert(QString::fromStdString(m));
            }, Qt::QueuedConnection);
        });
    // 增删品种的回调可能来自扫描线程：订阅/退订一律回到 GUI 线程做
    smart_->on_symbol_added = [this](const std::string& s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            if (ticker_) ticker_->subscribe(s);
            trendSigForce_.store(true);   // 新 bot 立刻拉信号，不干等一个周期
        }, Qt::QueuedConnection);
    };
    smart_->on_symbol_removed = [this](const std::string& s) {
        QMetaObject::invokeMethod(this, [this, s]() { unsubscribeIfUnused(s); },
                                  Qt::QueuedConnection);
    };
    // 智能裸K 的埋伏池进 / 出：只订阅价格，不建 bot（价格是全市场一次拉，多订不多花请求）
    smart_->on_watch = [this](const std::string& s, bool on) {
        QMetaObject::invokeMethod(this, [this, s, on]() {
            if (on) { if (ticker_) ticker_->subscribe(s); }
            else    unsubscribeIfUnused(s);
        }, Qt::QueuedConnection);
    };
    smart_->set_journal(journal_);
    smart_->load();
    // 状态文件里没有今天的数（升级后第一次运行）：用成交明细补，亏损限额不从 0 算
    smart_->seed_day_from_history(trades_);
    applySmartConfig(smartCfg_);
    if (smartCfg_.enabled) log("智能模式已启用，稍后开始扫描全市场", "OK");
}

void MainWindow::applySmartConfig(const smart::Config& c) {
    smartCfg_ = c;
    save_smart_cfg();
    if (smart_) smart_->set_config(c);
    // 同时持仓上限：开着时由智能模式设，关掉时 GUI 本身没有这项设置 ⇒ 不限
    if (trend_engine_ && !c.enabled) trend_engine_->set_max_open_positions(0);
    refreshSmartBadge();
}

void MainWindow::smartTick() {
    if (!smart_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (smart_->due(now) && !smartBusy_.exchange(true)) {
        auto sr = smart_;
        run_async([this, sr, now]() {
            try { sr->run_once(now); } catch (...) {}
            smartBusy_.store(false);
            QMetaObject::invokeMethod(this, [this]() {
                save_trend_bots();
                refreshBotTable();
                refreshSmartBadge();
            }, Qt::QueuedConnection);
        });
        return;
    }
    // 智能裸K 的点火检测：每拍一次（新的 5m 收盘了就判定池里的币，点火就建 bot 开仓）
    if (smartCfg_.enabled && smartCfg_.strategy == smart::Strategy::BareK &&
        !bareBusy_.exchange(true)) {
        auto sr = smart_;
        auto tk = ticker_;   // 副本：后台线程不碰成员（重连会换掉它）
        run_async([this, sr, tk, now]() {
            try {
                sr->bare_tick(now, [tk](const std::string& s) {
                    return tk ? tk->mark_price(s) : 0.0;
                });
            } catch (...) {}
            bareBusy_.store(false);
        });
    }
    // 轻量维护每分钟一次：关闭后清空仓自动 bot、当日盈亏落盘
    if (slowTickCount_ % 20 == 7 && !smartBusy_.load()) {
        smart_->housekeep(now);
        save_trend_bots();
        refreshSmartBadge();
    }
}

void MainWindow::refreshSmartBadge() {
    if (!smartBtn_) return;
    int autos = 0, holding = 0;
    if (trend_engine_)
        for (const auto& b : trend_engine_->get_bots()) {
            if (!b.is_auto) continue;
            ++autos;
            if (b.qty > 0) ++holding;
        }
    QString text, color = "#8b949e", tip;
    if (!smartCfg_.enabled) {
        text = autos > 0 ? QString("智能：关（%1 个待走完）").arg(autos) : QString("智能：关");
    } else if (smartCfg_.strategy == smart::Strategy::BareK) {
        int seeds = 0;
        if (smart_) {
            const auto rep = smart_->last_report();
            if (!rep.groups.empty()) seeds = (int)rep.groups.front().rows.size();
        }
        text = QString("智能裸K 埋伏 %1 · 持仓 %2").arg(seeds).arg(holding);
        color = "#58a6ff";
        if (smart_ && smartBusy_.load()) text += " · 扫描中";
    } else {
        text = QString("智能 监控 %1 · 持仓 %2").arg(autos).arg(holding);
        color = "#58a6ff";
        if (smart_ && smartBusy_.load()) text += " · 扫描中";
    }
    if (smartCfg_.pause_new) { text += " · 暂停开新仓"; color = "#d29922"; }
    if (trend_engine_ && smartCfg_.enabled && smartCfg_.daily_loss_limit > 0) {
        const double r = trend_engine_->day_pnl().realized;
        tip = QString("今日（UTC）已实现 %1U（含估算手续费）/ 限额 %2U")
                  .arg(r, 0, 'f', 2).arg(smartCfg_.daily_loss_limit, 0, 'f', 0);
        if (r <= -smartCfg_.daily_loss_limit) { text += " · 已达亏损限额"; color = "#f85149"; }
    }
    if (smart_) {
        const auto rep = smart_->last_report();
        if (rep.at_ms > 0)
            tip += QString("%1上次扫描 %2：%3")
                       .arg(tip.isEmpty() ? "" : "\n")
                       .arg(QDateTime::fromMSecsSinceEpoch(rep.at_ms).toString("HH:mm"))
                       .arg(QString::fromStdString(rep.summary));
    }
    smartBtn_->setText(text);
    smartBtn_->setToolTip(tip.isEmpty() ? QString("点击打开智能模式设置") : tip);
    smartBtn_->setStyleSheet(QString("QPushButton{background:#21262d;color:%1;font-size:11px;"
                                     "padding:0 8px;}").arg(color));
}

// 智能模式弹窗。两套策略（海龟 1h / 裸K 埋伏点火）二选一，选哪套就只显示哪套的设置页与名单页，
// 另一套的项不出现。两套共用的设置（硬门槛、仓位、亏损限额）两页各放一份控件，
// 切换时把当前页的值抄到另一页，保存时取正在显示的那一页——所以改一边另一边跟着变。
// 顶部的切换只是换显示，按「保存」才真正切策略
void MainWindow::openSmartDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("智能模式");
    dlg.resize(820, 720);
    auto* root = new QVBoxLayout(&dlg);
    const smart::Config c0 = smartCfg_;

    // ── 顶部：启用 + 策略切换 + 暂停开新仓（这三项两套共用，只放一份）──────────
    auto* top = new QHBoxLayout();
    auto* cbEnable = new QCheckBox("启用");
    cbEnable->setChecked(c0.enabled);
    cbEnable->setToolTip("扫描全市场、自动建 bot。两套策略共用这一个开关，同一时刻只跑一套。");
    top->addWidget(cbEnable);
    top->addSpacing(16);
    top->addWidget(new QLabel("策略："));
    auto* btnTurtle = new QPushButton("海龟 1h");
    auto* btnBare   = new QPushButton("裸K 埋伏点火");
    btnTurtle->setToolTip("1h 打分排名 → 候选池 → 常驻海龟 bot 等突破");
    btnBare->setToolTip("只做多：1h 结构建埋伏池（每天 08:00）→ 5m 收盘判定点火 → 点火K线低点止损");
    auto* stratGroup = new QButtonGroup(&dlg);
    stratGroup->setExclusive(true);
    stratGroup->addButton(btnTurtle, 0);
    stratGroup->addButton(btnBare, 1);
    for (auto* b : {btnTurtle, btnBare}) {
        b->setCheckable(true);
        b->setStyleSheet("QPushButton{background:#21262d;color:#8b949e;padding:4px 16px;"
                         "border:1px solid #30363d;border-radius:4px;}"
                         "QPushButton:checked{background:#1f6feb;color:#ffffff;border-color:#1f6feb;}");
        top->addWidget(b);
    }
    (c0.strategy == smart::Strategy::BareK ? btnBare : btnTurtle)->setChecked(true);
    // 只跑一套：当前这套还有持仓时，另一套的按钮置灰。走完（或在主表手动平仓）才能切
    auto holdings_now = [this](smart::Strategy s) {
        return trend_engine_ ? smart_holdings(trend_engine_->get_bots(), s) : 0;
    };
    auto* lockNote = new QLabel();
    lockNote->setStyleSheet("color:#d29922;font-size:11px;");
    lockNote->setVisible(false);
    {
        const bool cur_bare = (c0.strategy == smart::Strategy::BareK);
        const int  hold     = holdings_now(c0.strategy);
        if (hold > 0) {
            QPushButton* other = cur_bare ? btnTurtle : btnBare;
            const QString msg = QString("还有 %1 笔%2持仓，走完才能切换（也可以在主表手动平仓）")
                                    .arg(hold).arg(cur_bare ? "裸K" : "海龟");
            other->setEnabled(false);
            other->setToolTip(msg);
            lockNote->setText(msg);
            lockNote->setVisible(true);
        }
    }
    top->addSpacing(8);
    top->addWidget(lockNote);
    top->addStretch();
    auto* cbPause = new QCheckBox("暂停开新仓");
    cbPause->setChecked(c0.pause_new);
    cbPause->setToolTip("应急：全部 bot（含手动）暂停开新仓；不删 bot、不动已有仓位。");
    top->addWidget(cbPause);
    root->addLayout(top);

    auto* status = new QLabel();
    status->setWordWrap(true);
    status->setStyleSheet("color:#8b949e;font-size:11px;");
    root->addWidget(status);

    auto* tabs = new QTabWidget();
    root->addWidget(tabs, 1);

    // ── 控件工具 ──────────────────────────────────────────────────────────
    auto group = [](QVBoxLayout* pv, const QString& title) {
        auto* g = new QGroupBox(title);
        g->setStyleSheet("QGroupBox{color:#58a6ff;font-size:11px;font-weight:bold;"
                         "border:1px solid #21262d;border-radius:4px;margin-top:8px;"
                         "padding:10px 10px 6px 10px;}"
                         "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;}");
        auto* f = new QFormLayout(g);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pv->addWidget(g);
        return f;
    };
    auto dspin = [](double lo, double hi, double step, int dec, double val,
                    const QString& suffix = {}, const QString& special = {}) {
        auto* s = new QDoubleSpinBox();
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(dec);
        if (!suffix.isEmpty())  s->setSuffix(suffix);
        if (!special.isEmpty()) s->setSpecialValueText(special);
        s->setValue(val);
        return s;
    };
    auto ispin = [](int lo, int hi, int val, const QString& suffix = {}, const QString& special = {}) {
        auto* s = new QSpinBox();
        s->setRange(lo, hi);
        if (!suffix.isEmpty())  s->setSuffix(suffix);
        if (!special.isEmpty()) s->setSpecialValueText(special);
        s->setValue(val);
        return s;
    };
    auto hint = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet("color:#8b949e;font-size:11px;");
        return l;
    };
    // 收益率追踪止盈那一行（海龟模板、裸K 出场各一份）
    auto roiRowOf = [&dlg](bool on, double start, double back) {
        auto* cb = new QCheckBox("收益率追踪止盈");
        cb->setChecked(on);
        cb->setToolTip("收益率 = 浮动盈亏 ÷ 保证金，和主表「收益率」那一列同一个数（带杠杆）。\n"
                       "最高收益率到过激活线后，从最高点回撤指定的百分点就平仓\n"
                       "（例：激活 10%、回撤 5：最高到过 +15% 后回到 +10% 就平）。\n"
                       "和止损线同时生效，谁先触发听谁的。");
        auto* s1 = new QDoubleSpinBox();
        s1->setRange(0.5, 1000); s1->setDecimals(1); s1->setSuffix(" %"); s1->setValue(start);
        auto* s2 = new QDoubleSpinBox();
        s2->setRange(0.1, 1000); s2->setDecimals(1); s2->setSuffix(" 个点"); s2->setValue(back);
        auto* row = new QHBoxLayout();
        row->addWidget(cb);
        row->addWidget(new QLabel("收益率到"));
        row->addWidget(s1);
        row->addWidget(new QLabel("后，回撤"));
        row->addWidget(s2);
        row->addWidget(new QLabel("就平"));
        row->addStretch();
        auto sync = [s1, s2](bool en) { s1->setEnabled(en); s2->setEnabled(en); };
        sync(on);
        connect(cb, &QCheckBox::toggled, &dlg, sync);
        return std::make_tuple(cb, s1, s2, row);
    };
    auto makePage = [](QVBoxLayout*& pv) {
        auto* page = new QWidget();
        pv = new QVBoxLayout(page);
        auto* scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        return scroll;
    };

    // ── 每页一份的门槛 / 仓位 / 风控控件 ───────────────────────────────────
    // vol / age / chg（成交额、上线天数、24h 涨跌）两套各有各的值（裸K 的门槛更严）；
    // 其余是两套共用的同一个值，切换页面时抄过去
    struct Shared {
        QDoubleSpinBox *vol = nullptr, *chg = nullptr, *fund = nullptr;
        QSpinBox*       age = nullptr;
        QLineEdit*      black = nullptr;
        QComboBox*      size = nullptr;
        QDoubleSpinBox *bud = nullptr, *risk = nullptr, *buf = nullptr;
        QSpinBox*       lev = nullptr;
        QDoubleSpinBox *day = nullptr, *flt = nullptr;
    };
    const auto& t0 = c0.tmpl;
    auto mkShared = [&](double vol, int age, double chg) {
        Shared s;
        s.vol = dspin(0, 100000, 0.5, 1, vol / 1e6, " M U");
        s.vol->setToolTip("24h 成交额下限。");
        s.age = ispin(0, 3650, age, " 天");
        s.age->setToolTip("太新的币K线不够，ATR 与箱体都算不准。");
        s.chg = dspin(1, 1000, 5, 0, chg, " %");
        s.chg->setToolTip("24h 涨跌幅绝对值超过这个不新加：已经走完一大段的不追，在跌的不埋伏。");
        s.fund = dspin(0.001, 5, 0.01, 3, c0.max_abs_funding, " %");
        s.fund->setToolTip("资金费率绝对值（每 8 小时）超过这个不新加：太拥挤，或被操纵。");
        QStringList bl;
        for (const auto& x : c0.blacklist) bl << QString::fromStdString(x);
        s.black = new QLineEdit(bl.join(", "));
        s.black->setPlaceholderText("例如 XXXUSDT, YYYUSDT");
        s.black->setToolTip("永远不进名单 / 埋伏池。已有的自动 bot 空仓时会被移除，持仓中的等走完。\n"
                            "BTC、ETH、稳定币已内置排除。");
        s.size = new QComboBox();
        s.size->addItem("固定名义", (int)TrendConfig::SizeMode::Notional);
        s.size->addItem("固定单笔风险", (int)TrendConfig::SizeMode::RiskBased);
        s.size->setCurrentIndex(t0.size_mode == TrendConfig::SizeMode::RiskBased ? 1 : 0);
        s.bud = dspin(1, 1e7, 10, 0, t0.budget_usdt, " U");
        s.bud->setToolTip("固定名义：每笔名义金额。固定单笔风险：名义上限。");
        s.risk = dspin(0, 1e6, 1, 2, t0.risk_usdt, " U");
        s.risk->setToolTip("固定单笔风险时：一笔止损愿意亏多少。数量 = 它 ÷ |开仓价 − 止损线|。");
        s.lev = ispin(1, 125, t0.leverage, " x");
        s.buf = dspin(0.01, 20, 0.05, 2, t0.disaster_stop_buffer_pct, " %");
        s.buf->setToolTip("交易所侧硬止损挂在止损线外这么远（占价格 %）。程序崩溃时它是最后一道保护。");
        s.day = dspin(0, 1e7, 10, 0, c0.daily_loss_limit, " U", "关");
        s.day->setToolTip("全部 bot（手动 + 自动）当日已实现盈亏，减去按名义 × 0.05% × 2 估算的手续费。\n"
                          "≤ −限额 就停开新仓，已有仓位照常管理。按 UTC 日计，北京时间早 8 点恢复。");
        s.flt = dspin(0, 1e7, 10, 0, c0.float_loss_limit, " U", "关");
        s.flt->setToolTip("全部持仓的浮动盈亏合计 ≤ −限额 就停开新仓，回落后自动恢复。");
        return s;
    };
    auto copyShared = [](const Shared& a, const Shared& b) {
        b.fund->setValue(a.fund->value());
        b.black->setText(a.black->text());
        b.size->setCurrentIndex(a.size->currentIndex());
        b.bud->setValue(a.bud->value());
        b.risk->setValue(a.risk->value());
        b.lev->setValue(a.lev->value());
        b.buf->setValue(a.buf->value());
        b.day->setValue(a.day->value());
        b.flt->setValue(a.flt->value());
    };
    auto addGateRows = [](QFormLayout* f, const Shared& s) {
        f->addRow("24h 成交额 ≥", s.vol);
        f->addRow("上线天数 ≥", s.age);
        f->addRow("24h 涨跌 <", s.chg);
        f->addRow("资金费率 <", s.fund);
        f->addRow("拉黑", s.black);
    };
    auto addSizeRows = [](QFormLayout* f, const Shared& s) {
        f->addRow("仓位模式", s.size);
        f->addRow("名义金额", s.bud);
        f->addRow("单笔风险", s.risk);
        f->addRow("杠杆", s.lev);
        f->addRow("硬止损外扩", s.buf);
    };
    const Shared sT = mkShared(c0.min_quote_vol, c0.min_age_days, c0.max_abs_chg24);
    const Shared sB = mkShared(c0.bare.min_quote_vol, c0.bare.min_age_days, c0.bare.max_abs_chg24);

    // ════════════════════════════ 海龟 1h 设置页 ════════════════════════════
    QVBoxLayout* tv = nullptr;
    auto* turtlePage = makePage(tv);

    auto* fTGate = group(tv, "硬门槛（不满足直接排除）");
    addGateRows(fTGate, sT);
    auto* sAtrLo = dspin(0, 50, 0.1, 2, c0.atr_pct_min, " %");
    auto* sAtrHi = dspin(0.1, 100, 0.1, 2, c0.atr_pct_max, " %");
    auto* atrRow = new QHBoxLayout();
    atrRow->addWidget(sAtrLo);
    atrRow->addWidget(new QLabel("~"));
    atrRow->addWidget(sAtrHi);
    atrRow->addStretch();
    sAtrHi->setToolTip("建议 2%：回测里 1h ATR% > 2% 的期望≈0。");
    sAtrLo->setToolTip("建议 0.5%：再低的话 3×ATR 止损太近，手续费占一个 R 的比例变大。");
    fTGate->addRow("1h ATR% 区间", atrRow);

    auto* fList = group(tv, "方向与名单");
    auto* cbLong  = new QCheckBox("做多（扫强势 + 靠近上沿）");
    auto* cbShort = new QCheckBox("做空（扫弱势 + 靠近下沿）");
    cbLong->setChecked(c0.dir_long);
    cbShort->setChecked(c0.dir_short);
    cbLong->setToolTip("两个都勾：bot 是多空——向上突破做多、向下突破做空，不看它从哪份名单进来。\n"
                       "只勾一个：bot 只做那个方向。");
    cbShort->setToolTip(cbLong->toolTip());
    auto* dirRow = new QHBoxLayout();
    dirRow->addWidget(cbLong);
    dirRow->addWidget(cbShort);
    dirRow->addStretch();
    fList->addRow("方向", dirRow);
    auto* cbMode = new QComboBox();
    cbMode->addItem("混合：强势仅多 / 弱势仅空 / 中间多空 / 过热仅空", (int)smart::DirMode::Mixed);
    cbMode->addItem("突破定方向：全部多空，谁突破做谁", (int)smart::DirMode::Breakout);
    cbMode->setCurrentIndex(c0.dir_mode == smart::DirMode::Mixed ? 0 : 1);
    cbMode->setToolTip(
        "混合：按 7 天相对强弱分段，每段固定方向——跟着趋势走，强势币不逆势做空、弱势币不逆势做多；\n"
        "  中间段（名额最多）是横盘蓄势、方向未出的币，多空都做，谁突破做谁；\n"
        "  7 天内单日暴涨 ≥ 过热阈值的币单独一段，只做空（等它派发回落）。\n"
        "突破定方向：不分段，所有自动 bot 都是多空。");
    fList->addRow("方向模式", cbMode);

    // 混合模式的名额与门槛
    auto* sSn_ = ispin(0, 200, c0.strong_n, " 个");
    auto* sWn  = ispin(0, 200, c0.weak_n, " 个");
    auto* sMn  = ispin(0, 200, c0.mid_n, " 个");
    auto* sHn  = ispin(0, 200, c0.hot_n, " 个");
    auto* quotaRow = new QHBoxLayout();
    quotaRow->addWidget(new QLabel("强"));  quotaRow->addWidget(sSn_);
    quotaRow->addWidget(new QLabel("弱"));  quotaRow->addWidget(sWn);
    quotaRow->addWidget(new QLabel("中"));  quotaRow->addWidget(sMn);
    quotaRow->addWidget(new QLabel("过热")); quotaRow->addWidget(sHn);
    quotaRow->addStretch();
    fList->addRow("各段名额", quotaRow);
    auto* sEnter = dspin(1, 49, 5, 0, c0.seg_enter_pct, " %");
    auto* sExit  = dspin(1, 49, 5, 0, c0.seg_exit_pct, " %");
    sEnter->setToolTip("强势：相对强弱排名前这么多、且跑赢 BTC 才进；弱势对称（后这么多、跑输 BTC）。");
    sExit->setToolTip("掉出这个范围、或强弱翻了符号，连续 2 轮才离开——排名在边界附近浮动时方向不来回变。");
    auto* segRow = new QHBoxLayout();
    segRow->addWidget(new QLabel("进"));  segRow->addWidget(sEnter);
    segRow->addWidget(new QLabel("出"));  segRow->addWidget(sExit);
    segRow->addStretch();
    fList->addRow("强弱段门槛", segRow);
    auto* sTurn  = dspin(0, 1, 0.05, 2, c0.turn_er);
    sTurn->setToolTip("强势段里 24 根效率比低于 −这个值（在掉头）⇒ 这一轮降为多空；弱势段对称。");
    fList->addRow("掉头 效率比", sTurn);
    auto* sHot   = dspin(1, 1000, 5, 0, c0.hot_pct, " %");
    auto* sCrash = dspin(1, 100, 5, 0, c0.crash_pct, " %");
    sHot->setToolTip("7 天内任意连续 24 小时涨幅 ≥ 这个 ⇒ 过热段，只做空。\n"
                     "相对强弱是这一下堆出来的，后面多半是派发、阴跌回原位。");
    sCrash->setToolTip("7 天内任意连续 24 小时跌幅 ≥ 这个 ⇒ 放到中间段（不进弱势段，做空易被轧）。");
    auto* hotRow = new QHBoxLayout();
    hotRow->addWidget(new QLabel("过热 ≥")); hotRow->addWidget(sHot);
    hotRow->addWidget(new QLabel("暴跌 ≥")); hotRow->addWidget(sCrash);
    hotRow->addStretch();
    fList->addRow("单日涨跌", hotRow);
    auto* sWeakVol = dspin(0, 20, 0.1, 1, c0.weak_vol_ratio, " 倍", "不要求");
    sWeakVol->setToolTip(
        "弱势段（仅空）跌破时要求的量比，覆盖下面的「突破量比」。\n"
        "下跌不需要有人砸，阴跌缩量是常态——1.5 倍会挡掉一部分真实下跌。\n"
        "1.0 = 不低于均量：挡明显缩量的假跌破、放行正常阴跌。0 = 不要求。\n"
        "过热段固定不要求；中间段沿用模板（横盘币缩量跌破多半是洗盘）。\n"
        "建议先用 1.5，日志里弱势段常见「突破待放量」再往下调。");
    fList->addRow("弱势段量比≥", sWeakVol);

    // 突破模式的名额
    auto* sLn = ispin(0, 200, c0.long_n, " 个");
    auto* sSn = ispin(0, 200, c0.short_n, " 个");
    auto* lsRow = new QHBoxLayout();
    lsRow->addWidget(new QLabel("做多")); lsRow->addWidget(sLn);
    lsRow->addWidget(new QLabel("做空")); lsRow->addWidget(sSn);
    lsRow->addStretch();
    fList->addRow("名单名额", lsRow);
    // 只启用当前方向模式用得上的那些
    {
        auto sync = [=]() {
            const bool mixed = (smart::DirMode)cbMode->currentData().toInt() == smart::DirMode::Mixed;
            for (QWidget* w : std::initializer_list<QWidget*>{sSn_, sWn, sMn, sHn, sEnter, sExit,
                                                               sTurn, sHot, sCrash, sWeakVol})
                w->setEnabled(mixed);
            sLn->setEnabled(!mixed);
            sSn->setEnabled(!mixed);
        };
        sync();
        connect(cbMode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg,
                [sync](int) { sync(); });
    }
    auto* sMin = dspin(0, 100, 5, 0, c0.min_score, " 分");
    sMin->setToolTip("排名是相对的，行情清淡时前几名也可能很平庸——低于这个分不进名单。");
    fList->addRow("最低综合分", sMin);
    fList->addRow(hint("淘汰：空仓时掉出前 1.5N 且连续 2 轮、加入满 24 小时才移除；"
                       "硬门槛不合格立即移除；持仓中绝不移除，等这笔走完。"
                       "一笔走完后 bot 保留，继续等下一次突破。"
                       "混合模式下币换了段，空仓时 bot 跟着换方向，持仓中的等走完。"));

    auto* fTpl = group(tv, "海龟参数（不反手、硬止损强制开）");
    auto* cbIv = new QComboBox();
    cbIv->addItems({"1h", "2h", "4h"});
    cbIv->setCurrentText(QString::fromStdString(t0.interval));
    fTpl->addRow("信号周期", cbIv);
    auto* sDc = ispin(2, 200, t0.rule.donchian_period, " 根");
    fTpl->addRow("通道周期", sDc);
    auto* sAtrP = ispin(2, 100, t0.rule.atr_period, " 根");
    fTpl->addRow("ATR 周期", sAtrP);
    auto* sK = dspin(0.5, 20, 0.5, 1, t0.rule.atr_mult);
    fTpl->addRow("ATR 倍数 k", sK);
    auto* sBox = dspin(0, 20, 0.5, 1, t0.rule.squeeze_box_atr, " ATR", "关");
    fTpl->addRow("横盘 箱体≤", sBox);
    auto* sEr = dspin(0, 1, 0.05, 2, t0.rule.trend_er_min, {}, "关");
    fTpl->addRow("趋势 效率比≥", sEr);
    auto* sVr = dspin(0, 20, 0.1, 1, t0.rule.breakout_vol_ratio, " 倍", "关");
    fTpl->addRow("突破量比≥", sVr);
    auto* cbLock  = new QCheckBox("锁定 ATR");
    auto* cbAccel = new QCheckBox("盈利后加速收紧");
    cbLock->setChecked(t0.rule.lock_atr);
    cbAccel->setChecked(t0.rule.accel_exit);
    auto* exRow = new QHBoxLayout();
    exRow->addWidget(cbLock);
    exRow->addWidget(cbAccel);
    exRow->addStretch();
    fTpl->addRow("出场优化", exRow);
    auto* cbFail = new QCheckBox("多单");
    auto* cbFailShort = new QCheckBox("空单");
    cbFail->setChecked(t0.rule.fail_exit);
    cbFailShort->setChecked(t0.rule.fail_exit_short);
    const QString failTip = "开仓后有一根 1h 收盘跌回开仓时的通道边沿以内，不等 k×ATR 止损直接平。\n"
                            "假突破最常见的样子就是很快跌回箱体，等止损要亏满 1R。\n"
                            "多空分开勾：空单反弹回通道后多半又接着跌，判了反而砍掉能赚的单，默认只勾多单。";
    cbFail->setToolTip(failTip);
    cbFailShort->setToolTip(failTip);
    auto* failRow = new QHBoxLayout();
    failRow->addWidget(new QLabel("收盘跌回通道内就走（假突破提前出场）："));
    failRow->addWidget(cbFail);
    failRow->addWidget(cbFailShort);
    failRow->addStretch();
    fTpl->addRow("", failRow);
    auto* cbPaced = new QCheckBox("突破量比按已走时间折算");
    cbPaced->setChecked(t0.rule.vol_paced);
    cbPaced->setToolTip("不折算的话，1h 开盘 5 分钟就突破时累计量只有整根的 1/12，\n"
                        "多数突破被拦到后半小时才开，那时价格已远离通道边沿。\n"
                        "折算有下限：已走时间按至少 1/4 根算，开盘头几分钟的一点量不会被放大十几倍。");
    fTpl->addRow("", cbPaced);
    // 不用结构化绑定：下面的 collect lambda 要捕获它们，而捕获结构化绑定是 C++20 才放开的，
    // 老一些的 Apple clang 不认
    QCheckBox* cbTRoi = nullptr;
    QDoubleSpinBox *sTRoiStart = nullptr, *sTRoiBack = nullptr;
    QHBoxLayout* tRoiRow = nullptr;
    std::tie(cbTRoi, sTRoiStart, sTRoiBack, tRoiRow) =
        roiRowOf(t0.rule.roi_trail, t0.rule.roi_trail_start, t0.rule.roi_trail_back);
    fTpl->addRow("止盈", tRoiRow);
    fTpl->addRow(hint("改参数后，空仓的自动 bot 在下一轮扫描时换上新参数；持仓中的等走完再换。"));

    auto* fTSize = group(tv, "仓位（与裸K 共用）");
    addSizeRows(fTSize, sT);

    auto* fTRisk = group(tv, "风控（全部 bot 生效）");
    auto* sMaxPos = ispin(0, 500, c0.max_positions, " 个", "不限");
    fTRisk->addRow("同时持仓上限", sMaxPos);
    fTRisk->addRow("当日亏损限额", sT.day);
    fTRisk->addRow("持仓浮亏限额", sT.flt);
    auto* sRes = ispin(0, 50, c0.prio_reserve, " 个", "关");
    auto* sPri = dspin(0, 100, 5, 0, c0.prio_min_score, " 分");
    auto* priRow = new QHBoxLayout();
    priRow->addWidget(new QLabel("名额剩"));
    priRow->addWidget(sRes);
    priRow->addWidget(new QLabel("以内时，只接受 ≥"));
    priRow->addWidget(sPri);
    priRow->addStretch();
    fTRisk->addRow("名额紧张", priRow);
    tv->addStretch();

    // ════════════════════════ 裸K（埋伏点火）设置页 ════════════════════════
    QVBoxLayout* bv = nullptr;
    auto* barePage = makePage(bv);
    const auto& b0 = c0.bare;
    bv->addWidget(hint("只做多。每天北京 08:00（币安日K 切换）用 1h 结构建埋伏池，程序任何时候启动先建一次；"
                       "每根 1h 收盘后刷新池内；每根 5m 收盘后 3 秒内判定池里的币是否点火，点火就开仓。"));

    auto* fPool = group(bv, "埋伏池（12 根 1h 的结构）");
    auto* sPool = ispin(1, 200, b0.pool_n, " 个");
    sPool->setToolTip("按结构分取前这么多个。分数：κ 40、箱体内位置 25、1h ATR% 20、威科夫 10、ATR 压缩比 5。");
    fPool->addRow("池子大小", sPool);
    auto* sBAtr = dspin(0.1, 20, 0.1, 2, b0.atr_pct_max, " %");
    sBAtr->setToolTip("1h ATR% 超过这个不埋伏（回测 >2% 期望≈0，这条最重要）。≤1% 打分最高。");
    fPool->addRow("1h ATR% ≤", sBAtr);
    auto* sPosMin = dspin(0, 0.99, 0.05, 2, b0.pos_min);
    sPosMin->setToolTip("收盘在 12 根 1h 箱体里的位置（0 = 下沿，1 = 上沿）。只埋伏贴近上沿的。");
    fPool->addRow("箱体内位置 ≥", sPosMin);
    auto* sRiseOut = dspin(1, 500, 5, 0, b0.rise_out_pct, " %");
    sRiseOut->setToolTip("进池后涨超这么多就移出（已经走了，不再埋伏）。");
    fPool->addRow("进池后涨超移出", sRiseOut);
    fPool->addRow(hint("κ = (箱体高度% ÷ 1h ATR%) ÷ √12：0.8~1.2 最优；< 0.5 是假突破重灾区，> 1.5 已在走趋势。"
                       "每小时刷新时跌破箱体下沿、ATR% 超限、进池后涨太多的移出，不补人，等第二天重建。"));

    auto* fBGate = group(bv, "硬门槛");
    addGateRows(fBGate, sB);
    fBGate->addRow(hint("成交额、上线天数、24h 涨跌是裸K 自己的；资金费率、拉黑与海龟共用。"
                        "稳定币、黄金代币、股票等非币标的不埋伏。"));

    auto* fIgn = group(bv, "点火（刚收盘的那根 5m，全部满足）");
    auto* sBVol = dspin(0, 100, 0.5, 1, b0.vol_ratio, " 倍");
    sBVol->setToolTip("本根成交额 ÷ 前 20 根成交额的中位数。");
    fIgn->addRow("量比 ≥", sBVol);
    auto* sBody = dspin(0, 20, 0.1, 2, b0.body_pct_min, " %");
    fIgn->addRow("实体涨幅 ≥", sBody);
    auto* sClosePos = dspin(0, 1, 0.05, 2, b0.close_pos_min);
    sClosePos->setToolTip("收盘在这根K线高低之间的位置，越靠上越好。");
    fIgn->addRow("收盘位置 ≥", sClosePos);
    auto* sWick = dspin(0, 1, 0.05, 2, b0.upper_wick_max);
    sWick->setToolTip("上影 ÷ 振幅。上影长 = 冲上去被砸回来。");
    fIgn->addRow("上影 ≤", sWick);
    auto* sAmp = dspin(0, 20, 0.1, 2, b0.amp_atr_min, " ATR");
    sAmp->setToolTip("振幅 ÷ 点火前的 5m ATR。");
    fIgn->addRow("振幅 ≥", sAmp);
    auto* sEdge = dspin(0, 10, 0.1, 2, b0.edge_tol_pct, " %");
    sEdge->setToolTip("收盘不低于 1h 箱体上沿 −这么多 %（要贴着上沿或已站上）。");
    fIgn->addRow("离箱体上沿 ≥ −", sEdge);

    auto* fVeto = group(bv, "否决（任一成立就不做）");
    auto* sRed = ispin(0, 20, b0.veto_red_bars, " 根", "关");
    sRed->setToolTip("点火前这么多根全是阴线 = 下跌中继的反抽。");
    fVeto->addRow("点火前连阴", sRed);
    auto* sVetoPos = dspin(0, 1, 0.05, 2, b0.veto_pos_max);
    sVetoPos->setToolTip("点火前收盘在箱体里的位置低于这个 = 箱内乱窜，不是贴顶突破。");
    fVeto->addRow("点火前箱体位置 <", sVetoPos);
    auto* sRiseAtr = dspin(0, 20, 0.1, 2, b0.rise_atr_min, " ATR");
    sRiseAtr->setToolTip("涨幅 ÷ 5m ATR 低于这个 = 相对自身波动太弱，是噪音。");
    fVeto->addRow("涨幅 <", sRiseAtr);

    auto* fExit = group(bv, "出场");
    fExit->addRow(hint("初始止损 = 点火K线最低点（点火时 5m ATR 会瞬间膨胀好几倍，按点火前 ATR 算的止损不可用）。"
                       "之后按 k × 1h ATR 追踪，只上移。"));
    auto* sBK = dspin(0.5, 20, 0.5, 1, b0.atr_mult, " × 1h ATR");
    fExit->addRow("追踪止损", sBK);
    auto* cbBLock  = new QCheckBox("锁定 ATR");
    auto* cbBAccel = new QCheckBox("暴涨收紧（≥6 ATR 时 k=2，≥10 ATR 时 k=1.5）");
    cbBLock->setChecked(b0.lock_atr);
    cbBAccel->setChecked(b0.accel_exit);
    auto* bExRow = new QHBoxLayout();
    bExRow->addWidget(cbBLock);
    bExRow->addWidget(cbBAccel);
    bExRow->addStretch();
    fExit->addRow("出场优化", bExRow);
    QCheckBox* cbBRoi = nullptr;
    QDoubleSpinBox *sBRoiStart = nullptr, *sBRoiBack = nullptr;
    QHBoxLayout* bRoiRow = nullptr;
    std::tie(cbBRoi, sBRoiStart, sBRoiBack, bRoiRow) = roiRowOf(b0.roi_trail, b0.roi_start, b0.roi_back);
    fExit->addRow("止盈", bRoiRow);

    auto* fBSize = group(bv, "仓位（与海龟共用）");
    addSizeRows(fBSize, sB);

    auto* fBRisk = group(bv, "风控（全部 bot 生效）");
    auto* sBPos = ispin(0, 500, b0.max_positions, " 个", "不限");
    sBPos->setToolTip("全部 bot（含手动）同时持仓的上限。点火时已满就不开，记进 journal。");
    fBRisk->addRow("同时持仓上限", sBPos);
    fBRisk->addRow("当日亏损限额", sB.day);
    fBRisk->addRow("持仓浮亏限额", sB.flt);
    bv->addStretch();

    auto* stack = new QStackedWidget();
    stack->addWidget(turtlePage);
    stack->addWidget(barePage);
    stack->setCurrentIndex(c0.strategy == smart::Strategy::BareK ? 1 : 0);
    tabs->addTab(stack, "设置");

    // ── 名单页（跟着正在显示的策略换）──────────────────────────────────────
    const auto rep = smart_ ? smart_->last_report() : SmartRunner::Report{};
    auto turtleTable = [](const std::vector<SmartRunner::Row>& rows) {
        auto* t = new QTableWidget((int)rows.size(), 9);
        t->setHorizontalHeaderLabels({"#", "品种", "分数", "强弱", "7天最大单日涨",
                                      "ATR%", "箱体/ATR", "量能抬升", "已开 bot"});
        t->verticalHeader()->setVisible(false);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        for (int i = 0; i < (int)rows.size(); ++i) {
            const auto& r = rows[i];
            auto set = [&](int col, const QString& s) {
                auto* it = new QTableWidgetItem(s);
                it->setTextAlignment(Qt::AlignCenter);
                t->setItem(i, col, it);
            };
            set(0, QString::number(r.rank));
            set(1, QString::fromStdString(r.symbol));
            set(2, QString::number(r.score, 'f', 0));
            set(3, QString("%1%2").arg(r.rs >= 0 ? "+" : "").arg(r.rs, 0, 'f', 2));
            set(4, QString::number(r.max_up24, 'f', 0) + "%");
            set(5, QString::number(r.atr_pct, 'f', 2));
            set(6, QString::number(r.box12_atr, 'f', 1));
            set(7, QString::number(r.vol_lift, 'f', 2) + "x");
            set(8, r.has_bot ? "✓" : "");
        }
        return t;
    };
    auto bareTable = [this]() {
        // 状态按此刻的价格与 bot 现算
        const auto rows = smart_ ? smart_->bare_rows(QDateTime::currentMSecsSinceEpoch(),
                                                     [this](const std::string& s) {
                                                         return ticker_ ? ticker_->mark_price(s) : 0.0;
                                                     })
                                 : std::vector<SmartRunner::Row>{};
        auto* t = new QTableWidget((int)rows.size(), 9);
        t->setHorizontalHeaderLabels({"#", "品种", "状态", "分数", "κ", "箱体内位置",
                                      "1h ATR%", "离箱体上沿", "上涨量占比"});
        t->verticalHeader()->setVisible(false);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        auto pct = [](double v) {
            return QString("%1%2%").arg(v >= 0 ? "" : "−").arg(std::abs(v), 0, 'f', 2);
        };
        for (int i = 0; i < (int)rows.size(); ++i) {
            const auto& r = rows[i];
            auto set = [&](int col, const QString& s, const char* color = nullptr) {
                auto* it = new QTableWidgetItem(s);
                it->setTextAlignment(Qt::AlignCenter);
                if (color) it->setForeground(QColor(color));
                t->setItem(i, col, it);
            };
            const QString st = QString::fromStdString(r.status);
            set(0, QString::number(r.rank));
            set(1, QString::fromStdString(r.symbol));
            set(2, st, st == "持仓中" ? "#3fb950" : st == "今日已点火" ? "#d29922" : nullptr);
            set(3, QString::number(r.score, 'f', 0));
            // κ 在最优区间 0.8~1.2 的标绿
            set(4, QString::number(r.kappa, 'f', 2),
                (r.kappa >= 0.8 && r.kappa <= 1.2) ? "#3fb950" : nullptr);
            set(5, QString::number(r.box_pos, 'f', 2));
            set(6, QString::number(r.atr_pct, 'f', 2));
            set(7, pct(r.dist_up_pct));
            set(8, QString::number(r.up_vol_share * 100.0, 'f', 0) + "%");
        }
        t->setToolTip("离箱体上沿：价格离 12 根 1h 箱体上沿还有多少（负数 = 已站上）。\n"
                      "κ 0.8~1.2 标绿（回测最优区间）。今日已点火 = 今天开过仓、现在空仓，还能再点火。");
        return std::make_pair(t, (int)rows.size());
    };
    auto rebuildLists = [&, tabs](bool bare) {
        while (tabs->count() > 1) {
            QWidget* w = tabs->widget(1);
            tabs->removeTab(1);
            delete w;
        }
        if (bare) {
            auto [t, n] = bareTable();
            tabs->addTab(t, QString("埋伏池 %1/%2").arg(n).arg(sPool->value()));
            return;
        }
        // 上一次扫描是裸K（埋伏池）的话，没有海龟名单可看
        for (const auto& g : rep.groups) {
            if (g.code == smart::kSegBare) continue;
            tabs->addTab(turtleTable(g.rows), QString("%1 %2/%3")
                                                  .arg(QString::fromStdString(g.title))
                                                  .arg(g.rows.size()).arg(g.quota));
        }
    };

    // ── 状态行 ────────────────────────────────────────────────────────────
    auto updateStatus = [&, status](bool bare) {
        QString s = !c0.enabled ? QString("状态：关闭。")
                  : c0.strategy == smart::Strategy::BareK
                      ? QString("状态：已启用（裸K 埋伏点火），每天 08:00 建埋伏池、每根 1h 收盘后刷新，"
                                "每根 5m 收盘后 3 秒内判定点火。")
                      : QString("状态：已启用（海龟 1h），每小时整点后 1 分钟扫描一次。");
        if (bare != (c0.strategy == smart::Strategy::BareK))
            s += QString("\n正在查看%1的设置：按「保存」后才切换过去。").arg(bare ? "裸K" : "海龟");
        if (rep.at_ms > 0)
            s += QString("\n上次扫描 %1：%2")
                     .arg(QDateTime::fromMSecsSinceEpoch(rep.at_ms).toString("MM-dd HH:mm"))
                     .arg(QString::fromStdString(rep.summary));
        else if (!smart_)
            s += "\n未连接交易所：设置可以先保存，连接后生效。";
        if (trend_engine_) {
            const auto d = trend_engine_->day_pnl();
            s += QString("\n今日（UTC）已实现 %1U（含估算手续费）").arg(d.realized, 0, 'f', 2);
            if (c0.daily_loss_limit > 0)
                s += QString(" / 限额 %1U").arg(c0.daily_loss_limit, 0, 'f', 0);
        }
        status->setText(s);
    };

    rebuildLists(stack->currentIndex() == 1);
    updateStatus(stack->currentIndex() == 1);
    connect(stratGroup, &QButtonGroup::idClicked, &dlg, [&, stack](int id) {
        if (id == stack->currentIndex()) return;
        // 共用的设置跟着带过去：在这一页改过的，切过去看到的是同一个值
        copyShared(id == 1 ? sT : sB, id == 1 ? sB : sT);
        stack->setCurrentIndex(id);
        rebuildLists(id == 1);
        updateStatus(id == 1);
    });

    // ── 回读 ──────────────────────────────────────────────────────────────
    // 两套的专属参数都回读（没在看的那套保持原样）；共用的取正在显示的那一页
    auto collect = [&]() {
        smart::Config c = c0;
        const bool bare = (stack->currentIndex() == 1);
        const Shared& s = bare ? sB : sT;
        c.enabled   = cbEnable->isChecked();
        c.strategy  = bare ? smart::Strategy::BareK : smart::Strategy::Turtle;
        c.pause_new = cbPause->isChecked();

        // 海龟的成交额 / 上线天数 / 24h 涨跌永远取海龟页那一份（裸K 的在下面）
        c.min_quote_vol   = sT.vol->value() * 1e6;
        c.min_age_days    = sT.age->value();
        c.max_abs_chg24   = sT.chg->value();
        c.max_abs_funding = s.fund->value();
        c.blacklist.clear();
        for (auto x : s.black->text().split(QRegularExpression("[,，\\s]+"), Qt::SkipEmptyParts)) {
            x = x.trimmed().toUpper();
            if (!x.isEmpty()) c.blacklist.insert(x.toStdString());
        }
        c.daily_loss_limit = s.day->value();
        c.float_loss_limit = s.flt->value();
        auto& t = c.tmpl;
        t.size_mode   = (TrendConfig::SizeMode)s.size->currentData().toInt();
        t.budget_usdt = s.bud->value();
        t.risk_usdt   = s.risk->value();
        t.leverage    = s.lev->value();
        t.disaster_stop_buffer_pct = s.buf->value();

        // 海龟专属
        c.atr_pct_min = sAtrLo->value();
        c.atr_pct_max = sAtrHi->value();
        c.dir_long  = cbLong->isChecked();
        c.dir_short = cbShort->isChecked();
        c.dir_mode  = (smart::DirMode)cbMode->currentData().toInt();
        c.strong_n  = sSn_->value();
        c.weak_n    = sWn->value();
        c.mid_n     = sMn->value();
        c.hot_n     = sHn->value();
        c.seg_enter_pct = sEnter->value();
        c.seg_exit_pct  = sExit->value();
        c.turn_er   = sTurn->value();
        c.hot_pct   = sHot->value();
        c.crash_pct = sCrash->value();
        c.weak_vol_ratio = sWeakVol->value();
        c.long_n    = sLn->value();
        c.short_n   = sSn->value();
        c.min_score = sMin->value();
        c.max_positions  = sMaxPos->value();
        c.prio_reserve   = sRes->value();
        c.prio_min_score = sPri->value();
        t.interval = cbIv->currentText().toStdString();
        t.rule.donchian_period    = sDc->value();
        t.rule.atr_period         = sAtrP->value();
        t.rule.atr_mult           = sK->value();
        t.rule.squeeze_box_atr    = sBox->value();
        t.rule.trend_er_min       = sEr->value();
        t.rule.breakout_vol_ratio = sVr->value();
        t.rule.lock_atr           = cbLock->isChecked();
        t.rule.accel_exit         = cbAccel->isChecked();
        t.rule.fail_exit          = cbFail->isChecked();
        t.rule.fail_exit_short    = cbFailShort->isChecked();
        t.rule.vol_paced          = cbPaced->isChecked();
        t.rule.roi_trail          = cbTRoi->isChecked();
        t.rule.roi_trail_start    = sTRoiStart->value();
        t.rule.roi_trail_back     = sTRoiBack->value();

        // 裸K 专属（成交额 / 上线天数 / 24h 涨跌各套各的，取裸K 页那一份）
        auto& bc = c.bare;
        bc.min_quote_vol  = sB.vol->value() * 1e6;
        bc.min_age_days   = sB.age->value();
        bc.max_abs_chg24  = sB.chg->value();
        bc.pool_n         = sPool->value();
        bc.atr_pct_max    = sBAtr->value();
        bc.pos_min        = sPosMin->value();
        bc.rise_out_pct   = sRiseOut->value();
        bc.vol_ratio      = sBVol->value();
        bc.body_pct_min   = sBody->value();
        bc.close_pos_min  = sClosePos->value();
        bc.upper_wick_max = sWick->value();
        bc.amp_atr_min    = sAmp->value();
        bc.edge_tol_pct   = sEdge->value();
        bc.veto_red_bars  = sRed->value();
        bc.veto_pos_max   = sVetoPos->value();
        bc.rise_atr_min   = sRiseAtr->value();
        bc.atr_mult       = sBK->value();
        bc.lock_atr       = cbBLock->isChecked();
        bc.accel_exit     = cbBAccel->isChecked();
        bc.roi_trail      = cbBRoi->isChecked();
        bc.roi_start      = sBRoiStart->value();
        bc.roi_back       = sBRoiBack->value();
        bc.max_positions  = sBPos->value();
        return c;
    };
    // 校验走和 headless 同一套（config_from_json），保证两边认同一份规则
    auto validate = [&](const smart::Config& c, smart::Config& out) {
        std::string err;
        out = c;
        if (!smart::config_from_json(smart::config_to_json(c), out, &err)) {
            QMessageBox::warning(&dlg, "设置不合法", QString::fromStdString(err));
            return false;
        }
        if (out.enabled && out.strategy == smart::Strategy::Turtle &&
            !out.dir_long && !out.dir_short) {
            QMessageBox::warning(&dlg, "设置不合法", "启用智能模式至少要勾一个方向。");
            return false;
        }
        // 打开弹窗之后可能又开了仓：保存时再查一次
        if (out.strategy != c0.strategy) {
            const auto old = (out.strategy == smart::Strategy::BareK) ? smart::Strategy::Turtle
                                                                       : smart::Strategy::BareK;
            if (const int hold = holdings_now(old); hold > 0) {
                QMessageBox::warning(&dlg, "不能切换策略",
                    QString("还有 %1 笔%2持仓，走完才能切换（也可以在主表手动平仓）。")
                        .arg(hold).arg(old == smart::Strategy::BareK ? "裸K" : "海龟"));
                return false;
            }
        }
        return true;
    };
    auto confirm_enable = [&](const smart::Config& c) {
        // 第一次启用、或换了策略（裸K 与海龟开仓的节奏完全不同）都要确认一次
        if (!c.enabled || (c0.enabled && c0.strategy == c.strategy)) return true;
        const bool bare = (c.strategy == smart::Strategy::BareK);
        return confirmDanger(
            bare ? "启用智能裸K（埋伏点火）" : "启用智能模式（海龟 1h）",
            QString("智能模式会扫描全市场，在你【没有逐个看过】的币上自动建 bot 并开仓。%5\n\n"
                    "当前设置：同时持仓上限 %1 个，每笔名义 %2U、%3x 杠杆，当日亏损限额 %4。\n"
                    "确认这些数字在你能承受的范围内。")
                .arg(bare ? c.bare.max_positions : c.max_positions)
                .arg(c.tmpl.budget_usdt, 0, 'f', 0)
                .arg(c.tmpl.leverage)
                .arg(c.daily_loss_limit > 0 ? QString::number(c.daily_loss_limit, 'f', 0) + "U"
                                            : QString("未设"))
                .arg(bare ? QString("\n裸K 只做多，点火当下市价开仓；止损在点火K线最低点，"
                                    "单笔止损距离可能有好几个百分点。")
                          : QString()),
            "启用");
    };

    auto* btnRow = new QHBoxLayout();
    auto* btnScan = new QPushButton("保存并立即扫描");
    btnScan->setToolTip("保存设置，并马上扫描一次（不等下一轮）。");
    btnRow->addWidget(btnScan);
    btnRow->addStretch();
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Save)->setText("保存");
    bb->button(QDialogButtonBox::Cancel)->setText("关闭");
    btnRow->addWidget(bb);
    root->addLayout(btnRow);

    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, &dlg, [&]() {
        smart::Config c;
        if (!validate(collect(), c) || !confirm_enable(c)) return;
        applySmartConfig(c);
        log(QString("智能模式设置已保存（%1）")
                .arg(!c.enabled ? "关闭"
                     : c.strategy == smart::Strategy::BareK ? "启用 · 裸K 埋伏点火" : "启用 · 海龟 1h"),
            "OK");
        dlg.accept();
    });
    connect(btnScan, &QPushButton::clicked, &dlg, [&]() {
        smart::Config c;
        if (!validate(collect(), c) || !confirm_enable(c)) return;
        if (!c.enabled) {
            QMessageBox::information(&dlg, "智能模式", "智能模式没有启用，不扫描。");
            return;
        }
        applySmartConfig(c);
        if (!smart_) {
            QMessageBox::information(&dlg, "智能模式", "还没连接交易所：设置已保存，连接后自动扫描。");
            dlg.accept();
            return;
        }
        smart_->force();
        smartTick();
        log("智能模式：开始扫描全市场", "OK");
        dlg.accept();
    });

    dlg.exec();
}

} // namespace ccg
