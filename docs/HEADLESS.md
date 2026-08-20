# ccbot_headless：无图形界面版本

配置文件驱动，不依赖 Qt、不需要显示器，可以在 Linux 服务器上完全后台运行。核心策略引擎
（网格/马丁DCA、指标信号首单、账户级保证金上限、webhook提醒）跟 Windows 图形界面版共用同一份
代码，行为完全一致，只是把"填表单"换成了"写配置文件"。

## 构建（Linux）

```bash
git clone https://github.com/MrF-crypto/CCBot.git
cd CCBot
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg根目录>/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=x64-linux
cmake --build build --config Release --target ccbot_headless
```

> `vcpkg.json` 里 Qt 依赖标了 `"platform": "windows"`，Linux 上不会尝试装 Qt，只会装
> `ccbot_headless` 需要的 ixwebsocket / simdjson / curl / mbedtls，构建比图形界面版快很多。

生成的可执行文件在 `build/ccbot_headless`。

## 配置文件

复制 `config.example.json` 改成自己的，默认路径是运行目录下的 `config.json`，也可以指定别的路径：

```bash
./ccbot_headless /path/to/my_config.json
```

顶层字段：

| 字段 | 说明 |
|---|---|
| `api_key` / `api_secret` | Binance API 凭证，**明文存在配置文件里**，务必 `chmod 600` 并且不要提交到git（`.gitignore` 已经排除了 `config.json`，只有 `config.example.json` 模板会被提交） |
| `testnet` | `true`=连测试网，`false`=连真实账户，强烈建议先用 `true` 跑通 |
| `account_mode` | `"futures"`（默认）=普通合约账户，走 `fapi.binance.com`；`"portfolio_margin"`=统一账户，走 `papi.binance.com`。**统一账户没有测试网**，填了 `testnet: true` 会被忽略并告警。账户在币安开通统一账户后，普通合约的 API 端点就失效了，两者不能混用同一个账户的 Key |
| `max_total_margin` | 账户总保证金上限（USDT），`0`=不限，同图形界面版设置里的那个 |
| `alert_webhook` | 企业微信/飞书/Telegram webhook，触发硬止损、启动连接失败时推送 |
| `state_path` | 仓位运行时状态落盘路径，重启续跑用，默认 `ccbot_state.json` |
| `log_path` | 日志文件路径，留空则只输出到 stdout（配合 `journalctl`/`docker logs` 更方便） |
| `bots` | 策略数组，见下表 |

`bots` 数组每一项对应图形界面版"策略配置"弹窗里的一套参数：

| 字段 | 对应GUI | 可选值/说明 |
|---|---|---|
| `symbol` | 品种 | 如 `BTCUSDT` |
| `direction` | 方向 | `long` / `short` / `both` |
| `strat_type` | 策略 | `flat` `martingale` `mart_plus` `triple` `square` `fibonacci` `lucas` `linear` |
| `budget_usdt` `leverage` `max_entries` `interval_pct` `trail_entry` `tp_pct` `trail_tp` `auto_restart` `cooldown_secs` `stop_loss_pct` | 基础网格参数 | 同名，数值/布尔 |
| `entry_mode` | 首单模式 | `immediate`（默认，立即开首仓）/ `indicator`（等BOLL+RSI信号） |
| `kline_interval` `boll_period` `boll_mult` `use_rsi_filter` `rsi_period` `rsi_threshold` | 指标信号参数 | 同GUI |
| `rsi_confirm_mode` | RSI确认方式 | `snapshot`（瞬时快照）/ `cross`（反转确认：先探底再回穿阈值） |
| `rsi_oversold_th` | 探底阈值 | 仅 `cross` 模式用 |
| `dynamic_band_mode` | 动态W模式 (v2.3+) | `true` 时补仓锚定下轨、止盈锚定上轨，间隔/追踪参数由实时布林带宽 W 自动推导（间隔=W/3、追踪止盈=0.15W、追踪建仓=0.1W，带上下限夹逼），配置里的 `interval_pct`/`tp_pct`/`trail_*` 固定值不再生效 |
| `min_profit_floor` | 保底利润% | 仅动态W模式用：止盈激活除了触及上轨，还要求盈利≥此值（默认0.3，覆盖手续费+微利，防止上轨低于均价时亏着平仓） |
| `use_trend_filter` | 趋势过滤 (v2.5+) | `true` 时用高周期趋势判定空头态（价格在EMA之下且中轨明显下拐）：空头态暂停开新首仓、补仓间隔自动×1.5。趋势数据缺失时过滤自动失效不卡交易 |
| `trend_interval` / `trend_ema_period` | 趋势参数 | 默认 `4h` / `200`，一般不用改 |
| `sr_radar` | SR雷达 (v2.6+，影子模式) | `true` 时自动检测该品种的支撑/阻力区域（摆动点聚类+攻防转换+FVG缺口，约15分钟刷新），价格触区打日志+webhook告警（同区域2小时去重）。**不参与下单决策**——先验证检测准确率，执行接线是后续阶段 |
| `sr_interval` | SR雷达K线周期 | 默认 `4h` |

平仓明细会追加写入运行目录的 `ccbot_trades.csv`（时间/品种/方向/原因/开平价/数量/盈亏/层数），
方便做周期统计分析。

程序启动后，配置文件里的 bot **立即开始监控**（没有GUI版"先停止等手动开启"那一步）；如果
`state_path` 里有上次落盘的仓位状态（品种+方向能对上配置文件），会先恢复仓位再继续，不会
重新从零开首仓。

v2.4 起的安全行为：

- **单实例锁**：启动时在 `<state_path>.lock` 写入 PID，已有活着的实例时拒绝启动
  （双开同账户会重复下单）。程序崩溃残留的锁会被自动接管，无需手动清理
- **启动对账**：恢复仓位后会拉取交易所实际持仓核对——外部手动平过仓的 bot 本地状态
  自动清空并停止（webhook 告警），数量对不上的自动收敛到交易所值
- **状态原子写盘**：先写 `.tmp` 再改名，崩溃在写文件中途不会损坏状态文件

## 运行

```bash
./ccbot_headless config.json
```

日志会打印到 stdout（配了 `log_path` 的话同时落盘），`Ctrl+C`（或 `kill` 发 SIGTERM）会保存
当前仓位状态后干净退出。

## 用 systemd 常驻后台

```ini
# /etc/systemd/system/ccbot.service
[Unit]
Description=ccbot headless trading bot
After=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/ccbot
ExecStart=/opt/ccbot/ccbot_headless /opt/ccbot/config.json
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ccbot
journalctl -u ccbot -f
```

`config.json` 建议 `chmod 600`，`WorkingDirectory` 目录也建议只给运行用户权限，避免API Key被其他用户读到。

## 看门狗：发现"进程还在但不干活了"

交易系统最阴的故障不是崩溃——崩溃至少进程没了，`Restart=on-failure` 会拉起来。
真正危险的是**进程活着但循环停摆**：喂价全部返回 0、心跳一直失败、卡在某个网络调用上。
这种状态下仓位无人管理，而 `systemctl status` 显示一切正常。

程序每个 tick（3秒）会把当前时间戳写进 `<state_path>.alive`（默认
`ccbot_state.json.alive`）。**这个文件的年龄就是循环的心跳。** 正常退出时会删掉它。

### 方式一：systemd 定时器检查文件年龄

```ini
# /etc/systemd/system/ccbot-watchdog.service
[Unit]
Description=ccbot liveness check

[Service]
Type=oneshot
ExecStart=/opt/ccbot/watchdog.sh
```

```ini
# /etc/systemd/system/ccbot-watchdog.timer
[Unit]
Description=run ccbot liveness check every minute

[Timer]
OnBootSec=2min
OnUnitActiveSec=1min

[Install]
WantedBy=timers.target
```

```bash
#!/bin/bash
# /opt/ccbot/watchdog.sh —— 心跳文件超过 60 秒没更新就重启服务并告警
ALIVE=/opt/ccbot/ccbot_state.json.alive
HOOK='https://api.telegram.org/bot<TOKEN>/sendMessage?chat_id=<ID>'

# 文件不存在 = 进程正常退出或从未启动，交给 systemd 处理，这里不插手
[ -f "$ALIVE" ] || exit 0

AGE=$(( $(date +%s) - $(stat -c %Y "$ALIVE") ))
if [ "$AGE" -gt 60 ]; then
    curl -s -X POST "$HOOK" -H 'Content-Type: application/json'          -d "{\"text\":\"[watchdog] ccbot 心跳停止 ${AGE}s，正在重启\"}"
    systemctl restart ccbot
fi
```

```bash
sudo chmod +x /opt/ccbot/watchdog.sh
sudo systemctl enable --now ccbot-watchdog.timer
```

重启是安全的：启动时会从落盘状态恢复仓位、跟交易所对账、并重建交易所侧灾难止损单。

### 方式二：只告警不重启

把 `systemctl restart ccbot` 去掉即可。行情剧烈时自动重启会错过几十秒的判定窗口，
如果你更怕"自动操作在不该动的时候动手"，就只发告警、人工决定。

## 程序自己会叫的几种情况

配了 `alert_webhook` 之后，这些事件会主动推送——不用盯日志：

| 事件 | 触发条件 |
|---|---|
| 启动连接失败 | 启动时拉不到账户 |
| 启动对账不一致 | 落盘仓位与交易所实际持仓对不上 |
| **行情停摆** | 某品种连续 1 分钟取不到价格，**且该品种有持仓** |
| **账户接口连续失败** | 心跳连续 3 次失败（约 3 分钟）；恢复后也会通知 |
| **uniMMR 接近强平** | 统一账户 uniMMR < 1.3（币安 1.05 起强制减仓） |
| 触发硬止损 | 本地硬止损平仓 |
| **进程退出** | 收到退出信号时，附带"还有几个品种有持仓" |

最后一条值得单独说：**进程一停，所有本地风控就停了**，只剩交易所侧的灾难止损单
（`disaster_stop_pct`，见下）。这条消息本身就是"从现在起没人在管仓位"的信号。

## 资金费账本：一笔看不见的真实成本

永续合约每 8 小时结算一次资金费。**这是真实划走的现金，不是浮亏**——价格涨回来
也拿不回来。对"套住就长期持有"的用法，它是持有成本的全部定价。

0.01%/8h ≈ **年化 11%**。一个被套一年的多单，光资金费就吃掉 11%。

程序会自动记账，无需配置：

- **费率**约 5 分钟刷一次（公开接口，不占签名限流）
- **历史流水**约 1 小时同步一次（`/fapi/v1/income`，统一账户走 `/papi/v1/um/income`）
- 首次运行按最早那笔持仓的建仓时间往回补，7 天一窗分页，最多 30 窗（约 7 个月）
- 账本落在 `<state_path>.funding`，与 bot 生命周期无关（按**品种**记账，不按 bot——
  交易所是按"账户×品种"收费的，同品种多 bot 无法真实归属）

日志里每分钟一行：

```
资金费 | BTCUSDT 本轮已付 12.480000 USDT | 年化 -11.0% | 回本价 60000.00 → 60832.00
```

**回本价**那一项是关键：已付的资金费摊到每一份持仓上，就是均价之外还要多涨的部分。
它把"利息"换算成了你真正关心的单位。

年化持有成本超过 30% 会推一条告警（回落到 20% 以下解除）。**这条告警只告知成本变化，
不建议平仓**——持有决策是你的事，账本只负责让你看得见。

它不并进 `realized_pnl`：那个数是按平仓周期结算的，而资金费属于持有期。两个数分开
显示，口径才不会混。

## 唯一的进程外保护：`disaster_stop_pct`

本地的追踪止盈、硬止损、结构止损**全都活在进程里**。程序崩溃、断电、被 OOM
杀掉之后，它们一个都不剩。

`disaster_stop_pct`（每个 bot 单独配，0=关，默认关）会在**币安服务器上**挂一张
`STOP_MARKET` + `closePosition` 单，位置在均价下方该比例处。每次补仓拉低均价后
自动撤旧挂新；平仓后自动撤销；重启对账完成后自动重建。

```json
{ "symbol": "BTCUSDT", "use_disaster_stop": true, "disaster_stop_pct": 32, ... }
```

`use_disaster_stop` 是独立开关，**默认 false**。只写 `disaster_stop_pct` 不写开关的话
功能不会生效——启动时会明确告警，不让它静默地什么都不做。

**如果你的策略是「套住就长线持有、只要标的不归零就等」，这个功能与你的取向冲突**：
它会把浮亏变成实亏。保持关闭即可，那正是默认值。

**取值要远离正常止盈区间。** 网格策略天然要吃深度回撤，设太紧会在正常的补仓过程中
被打掉，把浮亏变成实亏。参考算法：按你的层数和间隔算出满层时的理论跌幅，再留一段
余量（满层跌 20% 的配置设 30~35）。

它不参与常规交易，只防瀑布。


---

## 全市场超卖扫描（v3.9.7+）

另一种用法：不预先指定品种，而是**扫描全市场永续合约，找出超卖的开仓，快进快出**。
配置模板见 `config.scanner.example.json`，把 `scanner.enabled` 设成 `true` 即可启用。

它和常规用法可以共存——`bots` 里放你的常驻品种，扫描器额外动态开仓，两者共享同一套
账户级闸门。

### 为什么是两阶段扫描

币安**没有批量 K 线接口**。全市场约 500 个 USDT 永续，逐个拉 K 线要串行两分半、
权重 500——每一轮都这个代价，跑不起来。

而 `/fapi/v1/ticker/24hr` 不带参数时一个请求返回全部品种（权重 40），足以按成交额和
跌幅把 500 个筛到几十个：

```
粗筛   1 个请求   权重 40    500 → 40 个
精算  40 个请求   权重 40    40  → 8 个
合计             权重 80    约 12 秒
```

差一个数量级。所以配置里 `coarse_top_n` 是**成本旋钮**，不是选股参数——它决定
"把有限的 K 线请求花在谁身上"。

### 三个必须理解的参数

**`min_quote_vol_24h`（成交额下限）** —— 最重要的一道过滤。快进快出赚的是 1~2% 的
差价，而小币的市价单双边滑点轻松吃掉 0.5%。**成交额不够的品种，这个策略在数学上
就不成立**，不是概率问题。

**`max_pct_b`（布林 %B 上限）** —— 主判据。%B 是**波动率归一化**的：同样跌 5%，在
低波动品种上是极端事件，在高波动品种上很平常，而 %B 把这个差异消掉了。全市场扫描
的本质就是跨品种排序，所以判据必须可跨品种比较——"跌了多少%"不行，%B 行。
RSI 只做次级确认。

**`max_open_positions`（并发持仓上限）** —— 开扫描器时**必填**，不填直接拒绝启动。
它和 `max_total_margin` 是两道不同的闸：后者管"总共投出去多少钱"，撞到了才停；
前者管"同时压在几个品种上"。大跌那天可能几十个品种同时满足信号，只有保证金上限的话，
钱会被最先触发的那几个吃光，分散度完全失控。

### 仓位是一次性的

扫描器建的 bot `auto_restart` 会被**强制为 false**（并给出告警）：止盈后 bot 变成
Stopped，主循环回收它、释放并发额度和 WS 订阅，把位置让给新候选。

否则跑一天就攒下几百个僵尸 bot，状态文件和订阅数一起膨胀，而且它们会一直占着并发额度。

配置文件里 `bots` 数组指定的常驻品种**不受影响**，不会被回收。

### 风险

- **全市场超卖 = 反向幸存者偏差**。跌得最狠的常常是要归零的——快进快出救不了归零。
- **和"套住长持"的用法是相反的**。建议用独立账户或子账户跑，别和长线仓位共用保证金池。
- 这套配置**没有回测支持**。`min_profit_floor`、`tp_fixed_profit` 这些值是推理出来的
  起点，不是实证最优。
