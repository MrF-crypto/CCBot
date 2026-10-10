// 智能模式的界面部分：设置弹窗、候选名单、顶部徽标、调度。
// 规则与执行都在 core/smart_mode + app/smart_runner（GUI 与 headless 共用），
// 这里只负责"什么时候调"和"怎么显示"。
#include "gui/main_window.h"

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
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

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
    smart_->load();
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

void MainWindow::openSmartDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("智能模式");
    dlg.resize(820, 720);
    auto* root = new QVBoxLayout(&dlg);

    auto* status = new QLabel();
    status->setWordWrap(true);
    status->setStyleSheet("color:#8b949e;font-size:11px;");
    root->addWidget(status);

    auto* tabs = new QTabWidget();
    root->addWidget(tabs, 1);

    // ── 设置页 ────────────────────────────────────────────────────────────
    const smart::Config c0 = smartCfg_;
    auto* page = new QWidget();
    auto* pv = new QVBoxLayout(page);
    auto group = [pv](const QString& title) {
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

    auto* fMain = group("开关与方向");
    auto* cbEnable = new QCheckBox("启用智能模式（扫描全市场、自动建海龟 bot 监控）");
    cbEnable->setChecked(c0.enabled);
    fMain->addRow(cbEnable);
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
    fMain->addRow("方向", dirRow);
    auto* cbPause = new QCheckBox("暂停开新仓（应急：全部 bot，含手动；不删 bot、不动已有仓位）");
    cbPause->setChecked(c0.pause_new);
    fMain->addRow(cbPause);

    auto* fGate = group("硬门槛（不满足直接排除）");
    auto* sVol  = dspin(0, 100000, 1, 1, c0.min_quote_vol / 1e6, " M U");
    sVol->setToolTip("24h 成交额下限。已在池里的币按 80% 判，在门槛边上不会来回进出。");
    fGate->addRow("24h 成交额 ≥", sVol);
    auto* sAge = ispin(0, 3650, c0.min_age_days, " 天");
    sAge->setToolTip("太新的币K线不够，ATR 与箱体都算不准。");
    fGate->addRow("上线天数 ≥", sAge);
    auto* sAtrLo = dspin(0, 50, 0.1, 2, c0.atr_pct_min, " %");
    auto* sAtrHi = dspin(0.1, 100, 0.1, 2, c0.atr_pct_max, " %");
    auto* atrRow = new QHBoxLayout();
    atrRow->addWidget(sAtrLo);
    atrRow->addWidget(new QLabel("~"));
    atrRow->addWidget(sAtrHi);
    atrRow->addStretch();
    fGate->addRow("1h ATR% 区间", atrRow);
    auto* sChg = dspin(1, 1000, 5, 0, c0.max_abs_chg24, " %");
    sChg->setToolTip("24h 涨跌幅绝对值超过这个不新加：已经走完一大段的，突破大概率是最后一冲。");
    fGate->addRow("24h 涨跌 <", sChg);
    auto* sFund = dspin(0.001, 5, 0.01, 3, c0.max_abs_funding, " %");
    sFund->setToolTip("资金费率绝对值（每 8 小时）超过这个不新加：太拥挤，或被操纵。");
    fGate->addRow("资金费率 <", sFund);
    QStringList bl;
    for (const auto& s : c0.blacklist) bl << QString::fromStdString(s);
    auto* eBlack = new QLineEdit(bl.join(", "));
    eBlack->setPlaceholderText("例如 XXXUSDT, YYYUSDT");
    eBlack->setToolTip("永远不进名单。已有的自动 bot 空仓时会被移除，持仓中的等走完。\n"
                       "BTC、ETH、稳定币已内置排除。");
    fGate->addRow("拉黑", eBlack);

    auto* fList = group("名单");
    auto* sLn = ispin(0, 200, c0.long_n, " 个");
    auto* sSn = ispin(0, 200, c0.short_n, " 个");
    fList->addRow("做多名单取前", sLn);
    fList->addRow("做空名单取前", sSn);
    auto* sMin = dspin(0, 100, 5, 0, c0.min_score, " 分");
    sMin->setToolTip("排名是相对的，行情清淡时前几名也可能很平庸——低于这个分不进名单。");
    fList->addRow("最低综合分", sMin);
    auto* note = new QLabel("淘汰：空仓时掉出前 1.5N 且连续 2 轮、加入满 24 小时才移除；"
                            "硬门槛不合格立即移除；持仓中绝不移除，等这笔走完。"
                            "一笔走完后 bot 保留，继续等下一次突破。");
    note->setWordWrap(true);
    note->setStyleSheet("color:#8b949e;font-size:11px;");
    fList->addRow(note);

    auto* fRisk = group("风控（全部 bot 生效）");
    auto* sMaxPos = ispin(0, 500, c0.max_positions, " 个", "不限");
    fRisk->addRow("同时持仓上限", sMaxPos);
    auto* sDay = dspin(0, 1e7, 10, 0, c0.daily_loss_limit, " U", "关");
    sDay->setToolTip("全部 bot（手动 + 自动）当日已实现盈亏，减去按名义 × 0.05% × 2 估算的手续费。\n"
                     "≤ −限额 就停开新仓，已有仓位照常管理。按 UTC 日计，北京时间早 8 点恢复。");
    fRisk->addRow("当日亏损限额", sDay);
    auto* sFloat = dspin(0, 1e7, 10, 0, c0.float_loss_limit, " U", "关");
    sFloat->setToolTip("全部持仓的浮动盈亏合计 ≤ −限额 就停开新仓，回落后自动恢复。");
    fRisk->addRow("持仓浮亏限额", sFloat);
    auto* sRes = ispin(0, 50, c0.prio_reserve, " 个", "关");
    auto* sPri = dspin(0, 100, 5, 0, c0.prio_min_score, " 分");
    auto* priRow = new QHBoxLayout();
    priRow->addWidget(new QLabel("名额剩"));
    priRow->addWidget(sRes);
    priRow->addWidget(new QLabel("以内时，只接受 ≥"));
    priRow->addWidget(sPri);
    priRow->addStretch();
    fRisk->addRow("名额紧张", priRow);

    const auto& t0 = c0.tmpl;
    auto* fTpl = group("自动 bot 模板（海龟；不反手、硬止损强制开）");
    auto* cbIv = new QComboBox();
    cbIv->addItems({"1h", "2h", "4h"});
    cbIv->setCurrentText(QString::fromStdString(t0.interval));
    fTpl->addRow("信号周期", cbIv);
    auto* cbSize = new QComboBox();
    cbSize->addItem("固定名义", (int)TrendConfig::SizeMode::Notional);
    cbSize->addItem("固定单笔风险", (int)TrendConfig::SizeMode::RiskBased);
    cbSize->setCurrentIndex(t0.size_mode == TrendConfig::SizeMode::RiskBased ? 1 : 0);
    fTpl->addRow("仓位模式", cbSize);
    auto* sBud  = dspin(1, 1e7, 10, 0, t0.budget_usdt, " U");
    sBud->setToolTip("固定名义：每笔名义金额。固定单笔风险：名义上限。");
    fTpl->addRow("名义金额", sBud);
    auto* sRisk = dspin(0, 1e6, 1, 2, t0.risk_usdt, " U");
    sRisk->setToolTip("固定单笔风险时：一笔止损愿意亏多少。数量 = 它 ÷ |开仓价 − 止损线|。");
    fTpl->addRow("单笔风险", sRisk);
    auto* sLev = ispin(1, 125, t0.leverage, " x");
    fTpl->addRow("杠杆", sLev);
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
    auto* sBuf = dspin(0.01, 20, 0.05, 2, t0.disaster_stop_buffer_pct, " %");
    fTpl->addRow("硬止损外扩", sBuf);
    auto* tplNote = new QLabel("改模板后，空仓的自动 bot 在下一轮扫描时换上新参数；持仓中的等走完再换。");
    tplNote->setWordWrap(true);
    tplNote->setStyleSheet("color:#8b949e;font-size:11px;");
    fTpl->addRow(tplNote);
    pv->addStretch();

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    tabs->addTab(scroll, "设置");

    // ── 名单页 ────────────────────────────────────────────────────────────
    const auto rep = smart_ ? smart_->last_report() : SmartRunner::Report{};
    auto mkTable = [](const std::vector<SmartRunner::Row>& rows) {
        auto* t = new QTableWidget((int)rows.size(), 7);
        t->setHorizontalHeaderLabels({"#", "品种", "综合分", "ATR%", "箱体/ATR", "量能抬升", "已开 bot"});
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
            set(3, QString::number(r.atr_pct, 'f', 2));
            set(4, QString::number(r.box12_atr, 'f', 1));
            set(5, QString::number(r.vol_lift, 'f', 2) + "x");
            set(6, r.has_bot ? "✓" : "");
        }
        return t;
    };
    tabs->addTab(mkTable(rep.longs),  QString("做多名单（%1）").arg(rep.longs.size()));
    tabs->addTab(mkTable(rep.shorts), QString("做空名单（%1）").arg(rep.shorts.size()));

    // ── 状态行 ────────────────────────────────────────────────────────────
    {
        QString s = c0.enabled ? "状态：已启用，每小时整点后 1 分钟扫描一次。"
                               : "状态：关闭。";
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
    }

    // ── 回读 ──────────────────────────────────────────────────────────────
    auto collect = [&]() {
        smart::Config c = c0;
        c.enabled   = cbEnable->isChecked();
        c.dir_long  = cbLong->isChecked();
        c.dir_short = cbShort->isChecked();
        c.pause_new = cbPause->isChecked();
        c.min_quote_vol   = sVol->value() * 1e6;
        c.min_age_days    = sAge->value();
        c.atr_pct_min     = sAtrLo->value();
        c.atr_pct_max     = sAtrHi->value();
        c.max_abs_chg24   = sChg->value();
        c.max_abs_funding = sFund->value();
        c.blacklist.clear();
        for (auto s : eBlack->text().split(QRegularExpression("[,，\\s]+"), Qt::SkipEmptyParts)) {
            s = s.trimmed().toUpper();
            if (!s.isEmpty()) c.blacklist.insert(s.toStdString());
        }
        c.long_n    = sLn->value();
        c.short_n   = sSn->value();
        c.min_score = sMin->value();
        c.max_positions    = sMaxPos->value();
        c.daily_loss_limit = sDay->value();
        c.float_loss_limit = sFloat->value();
        c.prio_reserve     = sRes->value();
        c.prio_min_score   = sPri->value();
        auto& t = c.tmpl;
        t.interval    = cbIv->currentText().toStdString();
        t.size_mode   = (TrendConfig::SizeMode)cbSize->currentData().toInt();
        t.budget_usdt = sBud->value();
        t.risk_usdt   = sRisk->value();
        t.leverage    = sLev->value();
        t.rule.donchian_period    = sDc->value();
        t.rule.atr_period         = sAtrP->value();
        t.rule.atr_mult           = sK->value();
        t.rule.squeeze_box_atr    = sBox->value();
        t.rule.trend_er_min       = sEr->value();
        t.rule.breakout_vol_ratio = sVr->value();
        t.rule.lock_atr           = cbLock->isChecked();
        t.rule.accel_exit         = cbAccel->isChecked();
        t.disaster_stop_buffer_pct = sBuf->value();
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
        if (out.enabled && !out.dir_long && !out.dir_short) {
            QMessageBox::warning(&dlg, "设置不合法", "启用智能模式至少要勾一个方向。");
            return false;
        }
        return true;
    };
    auto confirm_enable = [&](const smart::Config& c) {
        if (!c.enabled || c0.enabled) return true;
        return confirmDanger(
            "启用智能模式",
            QString("智能模式会扫描全市场，在你【没有逐个看过】的币上自动建 bot 并开仓。\n\n"
                    "当前设置：同时持仓上限 %1 个，每笔名义 %2U、%3x 杠杆，当日亏损限额 %4。\n"
                    "确认这些数字在你能承受的范围内。")
                .arg(c.max_positions)
                .arg(c.tmpl.budget_usdt, 0, 'f', 0)
                .arg(c.tmpl.leverage)
                .arg(c.daily_loss_limit > 0 ? QString::number(c.daily_loss_limit, 'f', 0) + "U"
                                            : QString("未设")),
            "启用");
    };

    auto* btnRow = new QHBoxLayout();
    auto* btnScan = new QPushButton("保存并立即扫描");
    btnScan->setToolTip("保存设置，并马上扫描一次（不等下个整点）。");
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
        log(QString("智能模式设置已保存（%1）").arg(c.enabled ? "启用" : "关闭"), "OK");
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
        log("智能模式：开始扫描全市场（约几十秒）", "OK");
        dlg.accept();
    });

    dlg.exec();
}

} // namespace ccg
