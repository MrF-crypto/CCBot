# ccbot_headless：无图形界面版本

配置文件驱动，不依赖 Qt、不需要显示器，可以在 Linux 服务器上完全后台运行。核心策略引擎
（趋势 SAR、行情链路自愈、账户级闸门、webhook 提醒）跟图形界面版共用同一份代码，
行为完全一致，只是把"填表单"换成了"写配置文件"。

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
| `alert_webhook` | 企业微信/飞书/Telegram webhook，行情链路持续异常、对账不一致、账户接口失败、资金费成本跨阈值时推送 |
| `max_open_positions` | 同时持仓的品种数上限，`0`=不限。与 `max_total_margin` 是两道不同的闸：前者管"总共投多少钱"，这个管"同时压在几个品种上" |
| `state_path` | 仓位运行时状态落盘路径，重启续跑用，默认 `ccbot_state.json` |
| `log_path` | 日志文件路径，留空则只输出到 stdout（配合 `journalctl`/`docker logs` 更方便） |
| `sar_bots` | 策略数组，见下一节 |

> ⚠ **v5.0.0：旧的 `bots` 数组（网格 DCA）不再运行。** 它不会被静默忽略——启动时会明确报出
> "配置里有 N 个 bots 条目，但网格DCA 已整体移除，这些条目【完全不会运行】"。
>
> 静默忽略才是危险的：升级后配置文件原样放着、进程正常启动、日志一切正常，而那些品种其实
> 一个都没在跑，要等到某天去交易所对账才发现。趋势策略请配在 `sar_bots` 里。
> DCA 的实测结论见 [NEGATIVE_RESULTS.md](NEGATIVE_RESULTS.md)——代码删了，花钱买来的结论不删。

## 趋势 SAR 策略（`sar_bots`）

本版起是唯一的策略。状态落盘在 `<state_path>.sar`（后缀保留，老部署升级上来时这个文件已经
存在，改名等于把在跑的仓位状态丢掉）。重启会恢复持仓、开仓价、止损线、连续反手计数与统计，
并在启动时与交易所对账一次，运行中每分钟再对一次。

这套策略的风险形状：**胜率低（30~40%）、每笔都止损、盈利来自少数几笔跑很远的单子、
致命场景是震荡市连续磨损**。所以判据是 `胜率 × 盈亏比`，不是胜率。

```json
"sar_bots": [
  { "symbol": "BTCUSDT", "budget_usdt": 500, "leverage": 3, "interval": "4h",
    "donchian_period": 20, "atr_period": 14, "atr_mult": 3.0 }
]
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `symbol` | — | 必填 |
| `budget_usdt` | `1000` | 每笔仓位的**名义价值**。SAR 是单仓位，没有分层，这就是全部 |
| `leverage` | `3` | |
| `interval` | `4h` | 信号K线周期 |
| `mode` | `donchian` | 入场/止损算法。`donchian`=突破入场 + k×ATR 棘轮止损（海龟）；`bar`=**裸K线**：刚收盘那根是阳线就做多、阴线就做空，止损用它**之前** N 根的最低价（做多）/ 最高价（做空）。两套是成套的，不能混搭 |
| `swing_bars` | `3` | `mode=bar` 时止损用最近几根。信号根**不算**在内。⚠ 这个数直接决定持仓时长：随机游走下平均持仓约 N+1 根 |
| `donchian_period` | `20` | 唐奇安通道周期（海龟原版 20）。上破 N 根最高→做多，下破 N 根最低→做空。通道**排除当前未收盘K线**，否则「价格≥上沿」恒成立、信号恒真 |
| `atr_period` | `14` | ATR 周期，Wilder 平滑（与 TradingView/币安同口径） |
| `atr_mult` | `3.0` | **止损距离 = k × ATR**（Chandelier Exit）。棘轮，只朝有利方向移动。k 越小假突破越多，越大回吐越多 |
| `allow_reverse` | `true` | 亏损止损后是否反向入场。**盈利出场永不反手**——力竭不等于反转 |
| `reverse_needs_signal` | `true` | **强烈建议保持 `true`。** 反手要求反向信号真的成立，而不是「我被打了所以反着来」。无条件反手在震荡市是绞肉机：亏损止损本就在震荡市最频繁，ATR 常态 2% 时单次绞杀约 6% 名义，3 倍杠杆即保证金的 18% |
| `max_consecutive_reverses` | `2` | 连续反手上限。真趋势不需要连续反手，连续反手本身就是「现在是震荡市」的信号 |
| `cooldown_bars` | `3` | 触顶后冷却多少根**K线**（不是 tick） |
| `signal_max_age_sec` | `900` | 信号快照过期时长。过期后**不开新仓**，但已持仓的止损线仍然有效（沿用最后一条有效线，绝不因数据断流撤掉保护） |
| `use_disaster_stop` | `false` | **强烈建议开。** 把棘轮止损线镜像成交易所上的 `STOP_MARKET + closePosition` 单——唯一的进程外保护。这套策略的全部保护就是那条活在本进程里的止损线，程序崩了就什么都不剩 |
| `disaster_stop_buffer_pct` | `1.0` | 挂单价 = 止损线再外扩这么多%（多头往下、空头往上）。**必须 > 0**，填 0 会被配置校验拒绝。理由：交易所用连续标记价触发而本地每 3 秒采样，挂在线上会让交易所抢先触发，于是正常止损变成「外部平仓 → 对账停 bot」，一次例行出场变成需要人工介入的事件。止损线移动超过 0.5% 才重挂（逐次撤挂会吃光限流额度） |
| `size_mode` | `notional` | 仓位算法。`notional`=固定名义；`risk`=**按 ATR 等风险**：名义 = `risk_usdt / (k×ATR%)`，每笔止损亏的钱固定。同一份名义在 ATR 1.6% 的 LTC 和 5.3% 的 COTI 上，单次止损亏的钱差 3.3 倍——固定名义等于把风险全压在高波动那几个品种上，「铺开品种分散风险」就此失效。这是海龟的「单位」概念 |
| `risk_usdt` | `0` | `size_mode=risk` 时**必填**：单次止损愿亏多少钱。账户 10000U、每次探测愿亏 1% ⇒ 填 100。此时 `budget_usdt` 变成**名义上限**（ATR 极小时兜住公式算出的天量仓位） |
| `pyramid_max_adds` | `0` | **顺势加仓档数**（0=关）。每朝有利方向再走 `pyramid_step_atr` 个 ATR 就加一档。回答的是「怎么低成本试出单边大行情」：错了只亏第一档，对了越骑越重。方向是**顺势**的：涨了才加，不是跌了摊薄 |
| `pyramid_step_atr` | `0.5` | 每走多少个 ATR 加一档（海龟原版 0.5）。间距从**上一档的成交价**量起，不是首档，否则越加越密 |

**没有固定止盈，这是设计而非遗漏**：趋势跟随的胜率天然只有 30~40%，全部收益来自少数几笔跑得很远的单子。任何固定止盈都会砍断这些单子，而亏损笔的大小不变——等于单方面砍掉盈利分布的右尾，期望必然转负。

**选 `atr_mult` 前先看实测 ATR**：GUI 版的 SAR 表有一列 ATR%，悬停即给出当前 k 对应的实际止损距离。跨品种唯一可比的口径是 `atr/price`，所以 BTC 和新上山寨能共用同一组参数。

**对账的判定规则**（刻意保守——这套策略没有「层」这种可供收敛的中间结构）：

| 情况 | 处理 |
|---|---|
| 本地有仓、交易所没有 | 清空本地并**停止** bot。分不清是人工平的还是被强平的，后者继续开仓是往坑里跳 |
| 本地空仓、交易所有仓 | **停止** bot 并告警。最危险的一种——不停的话下一个突破信号会再开一笔，净敞口翻倍且原来那笔没有止损线守着 |
| 本地数量 > 交易所 | 外部部分平仓：数量收敛到交易所值，止损线与开仓价保留 |
| 本地数量 < 交易所 | 仅告警，不动本地。那笔不是自己开的，止损线基准不成立 |
| 方向不一致 | **停止** bot。任何自动收敛都是在猜 |

在途（有订单未回执）的 bot 一律跳过对账——订单可能已在交易所生效而本地还没入账，此刻比对必然误判。

SAR 的平仓也写进 `ccbot_trades.csv`，方向列是 `sar_long` / `sar_short`，层数列恒为 0。

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
| **行情链路持续异常** | 连续约 3 分钟 `Health::healthy()` 为假（WS 半开、订阅失效、涨幅流单独死掉）。**与上面那条"行情停摆"不是一回事**——半开连接下 REST 兜底会让 per-symbol 的停摆计数一直归零、一条告警不发，而链路已经死了 |
| **进程退出** | 收到退出信号时，附带"还有几个品种有持仓" |

最后一条值得单独说：**进程一停，所有本地风控就停了**，只剩交易所侧的灾难止损单
（`use_disaster_stop`，见下）。这条消息本身就是"从现在起没人在管仓位"的信号。

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

## 唯一的进程外保护：`use_disaster_stop`

**这套策略的全部保护就是那条活在进程里的棘轮止损线。** 程序崩溃、断电、被 OOM
杀掉之后，它一点都不剩——仓位完全裸奔且没有任何底。

打开 `use_disaster_stop` 之后，引擎会在**币安服务器上**挂一张 `STOP_MARKET` +
`closePosition` 单，镜像当前的棘轮止损线。止损线每次往前推进都会自动撤旧挂新
（超过 0.5% 才重挂，否则会把限流额度吃光）；平仓后自动撤销；重启对账完成后自动重建。

```json
{ "symbol": "BTCUSDT", "use_disaster_stop": true, "disaster_stop_buffer_pct": 1.0, ... }
```

**`disaster_stop_buffer_pct` 不能是 0。** 挂单价 = 止损线再往外扩这么多%（多头往下、
空头往上）。交易所用连续的标记价触发，而本地是采样的——挂在止损线【上】的话交易所
几乎总会先触发，于是主出场路径从"本地 reduceOnly 平仓"变成"交易所平掉、本地靠对账
才发现"，而对账发现外部平仓会**停掉那个 bot**。等于把一次正常的止损出场变成一次
需要人工介入的事件。留出缓冲之后：正常情况本地先平并撤掉这张单，只有进程真的不在了，
价格才会继续走到它上面。

它不参与常规交易，只在进程不在时兜底。


