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

## 趋势策略（`sar_bots`）

本版有**三个趋势策略**，靠 `strategy` 字段选，一个 bot 一个策略。数组名 `sar_bots` 保留没改：
老部署的配置文件里就是这个名字，改名等于让升级上来的人配置全失效。

状态落盘在 `<state_path>.sar`（后缀同理保留）。重启会恢复持仓、开仓价、止损线、加速因子、
连续反手计数与统计，并在启动时与交易所对账一次，运行中每分钟再对一次。

三个策略共享同一套**出场语义**：只有追踪止损、**没有固定止盈**、止损线是棘轮的、
数据断流时沿用最后一条有效线。风险形状也一样：**胜率低（30~40%）、每笔都止损、
盈利来自少数几笔跑很远的单子、致命场景是震荡市连续磨损**。判据是 `胜率 × 盈亏比`，不是胜率。

```json
"sar_bots": [
  { "symbol": "BTCUSDT", "strategy": "turtle", "interval": "4h",
    "budget_usdt": 500, "leverage": 3,
    "donchian_period": 20, "atr_period": 14, "atr_mult": 3.0 }
]
```

### 选哪个

| `strategy` | 入场 | 止损 | 适合 |
|---|---|---|---|
| `turtle`（默认） | 价格创 N 根新高/新低 | `k × ATR` 的 Chandelier 棘轮线 | 4h 以上。信号最少、对参数最不敏感 |
| `psar` | 价格站上/跌破前一根极值；之后**只由翻转产生** | SAR 值本身，随趋势加速贴近价格 | 有明确单边的品种。震荡市会连续翻转 |
| `bare_k` | 盘中即时 或 收盘突破前一根高/低 | 最近 N 根的摆动极值 | 短周期。最快也最吵 |

### 共享字段

| 字段 | 默认 | 说明 |
|---|---|---|
| `symbol` | — | 必填 |
| `strategy` | `turtle` | `turtle` / `psar` / `bare_k`。填别的会**拒绝启动**，不会默默跑海龟 |
| `budget_usdt` | `1000` | 每笔仓位的**名义价值**。三个策略都是单仓位、都不加仓，这就是全部 |
| `leverage` | `3` | |
| `interval` | `4h` | 信号K线周期。可选 `1m` `3m` `5m` `15m` `30m` `1h` `2h` `4h` `6h` `8h` `12h` `1d`（币安没有 10m 这一档） |
| `reverse` | `none` | 亏损止损后怎么办。`none`=只平掉，回到正常入场流程（方向不限，同向信号先来就同向再进）；`immediate`=立即反手开反向仓，不查信号。**盈利出场永不反手**——力竭不等于反转（`psar` 例外，见下）|
| `max_consecutive_reverses` | `0` | 连续反手上限，`0`=不限。真趋势不需要连续反手，连续反手本身就是「现在是震荡市」的信号 |
| `cooldown_bars` | `0` | 触顶后冷却多少根**K线**（不是 tick） |
| `signal_max_age_sec` | `0` | 信号快照的保质期。过期后**不开新仓**，但已持仓的止损线仍然有效。`0`=**自动**取 `max(60秒, 2.5×K线周期)`——1m 给 150 秒，4h 给 10 小时。填死值只在你明确知道想要什么时才有必要 |
| `size_mode` | `notional` | 仓位算法。`notional`=固定名义；`risk`=**固定单笔风险**：名义 = `risk_usdt ÷ 入场价到止损线的距离%`。用的是**当前策略算出来的那条线**（海龟 k×ATR、SAR 首根 SAR 值、裸K 摆动极值），不是某个固定的 ATR 倍数 |
| `risk_usdt` | `0` | `size_mode=risk` 时**必填**：单次止损愿亏多少钱。账户 10000U、每次探测愿亏 1% ⇒ 填 100。此时 `budget_usdt` 变成**名义上限**（止损距离极小时兜住公式算出的天量仓位） |
| `use_disaster_stop` | `false` | **强烈建议开。** 把棘轮止损线镜像成交易所上的 `STOP_MARKET + closePosition` 单——唯一的进程外保护。这套策略的全部保护就是那条活在本进程里的止损线，程序崩了就什么都不剩。挂不上会重试 13 次（3 次快速 + 7 次退避 + 3 次最后机会），第 10 次起界面标红「此仓位无交易所侧保护」，第 13 次仍失败就**立刻平掉刚开的仓**；连续 2 次这样平仓会熔断停掉该 bot |
| `disaster_stop_buf_dist_pct` | `20.0` | 挂单价 = 止损线再外扩**止损距离的**这么多%（多头往下、空头往上）。止损距离 = \|开仓价 − 初始止损线\|，即这一仓的计划风险，所以填 20 的含义是「进程真死了，最坏亏 1.2 倍计划风险」——这个倍数与品种波动率、杠杆、止损松紧都无关。**必须 > 0**，填 0 会被配置校验拒绝：交易所用连续标记价触发而本地每 3 秒采样，挂在线上会让交易所抢先触发，于是正常止损变成「外部平仓 → 对账停 bot」，一次例行出场变成需要人工介入的事件。挂上之后**永不重挂**，直到仓位关闭才撤 |
| ~~`disaster_stop_buffer_pct`~~ | — | **v5.10.0 废弃。** 旧口径是「占**价格**的百分比」。换成占止损距离是因为占价格会让兜底单的代价与仓位风险脱钩：止损距离 0.5% 的仓位配 1%，兜底单落在 1.5% 外，进程一死就是计划亏损的 3 倍；止损距离 8% 的仓位，1% 又紧到容易被交易所抢先。仍然会被识别，但只用来提示你改键名——本次按新默认值运行。**两种口径无法换算**（旧口径要折算得知道该 bot 当时的 ATR，配置文件里没有），请自行确认一次；默认参数下 `20` 与旧的 `1` 大致等价 |

为什么 `size_mode=risk` 值得开：同一份名义，在 4h 波动 1.6% 的 LTC 和 5.3% 的 COTI 上，
单次止损亏的钱差 3.3 倍。固定名义 = 风险全压在高波动那几个品种上，「铺开品种分散风险」就此失效。
这是海龟的「单位」概念。

### ① `turtle` 专属

| 字段 | 默认 | 说明 |
|---|---|---|
| `donchian_period` | `20` | 唐奇安通道周期（海龟原版 20）。上破 N 根最高→做多，下破 N 根最低→做空。通道**排除当前未收盘K线**，否则「价格≥上沿」恒成立、信号恒真 |
| `atr_period` | `14` | ATR 周期，Wilder 平滑（与 TradingView/币安同口径）。**只有这个策略用 ATR** |
| `atr_mult` | `3.0` | **止损距离 = k × ATR**（Chandelier Exit）。棘轮，只朝有利方向移动。k 越小假突破越多，越大回吐越多。经典值 2.5~3.5 |

**选 `atr_mult` 前先看实测 ATR**：GUI 版的表有一列 ATR%，悬停即给出当前 k 对应的实际止损距离。
跨品种唯一可比的口径是 `atr/price`，所以 BTC 和新上山寨能共用同一组参数。

### ② `psar` 专属

`SAR(下一根) = SAR + AF × (EP − SAR)`，其中 EP 是持仓期极值（多=最高、空=最低）。
**AF 只在刷新 EP 的那一根递增**，封顶 `af_max`——这是「趋势走得越强 SAR 收得越快」的全部来源，
每根都加的话 AF 三四根就封顶、SAR 立刻贴上价格、一点正常回调就出场。
Wilder 的夹逼规则也实现了：SAR 不得落进**前两根**的价格范围内。

| 字段 | 默认 | 说明 |
|---|---|---|
| `af_start` | `0.02` | 加速因子初值（Wilder 原版）。调大 = 一开仓就贴得紧 = 更容易被正常回调扫掉 |
| `af_step` | `0.02` | 每次刷新 EP 时 AF 加多少 |
| `af_max` | `0.20` | AF 上限。封顶后 SAR 每根固定吃掉「当前距 EP」的 20% |

⚠ **`reverse` 对这个策略是写死的 `immediate`，配了也会被覆盖。** PSAR 的定义就是
stop-and-reverse：触及 SAR 就平仓并立即反向，**盈利触及也翻**。它的入场信号**只有**翻转
这一个来源，不翻就永远空仓——所以「保守版 PSAR」不是更保守，是根本不交易。

⚠ 代价是它在震荡市里会**一直翻下去没有刹车**，这是 PSAR 最著名的弱点而不是实现缺陷。
不设 `max_consecutive_reverses` 时启动会明确告警一次，建议设 2~3。

### ③ `bare_k` 专属

| 字段 | 默认 | 说明 |
|---|---|---|
| `bare_entry` | `break_prev` | `immediate`=**盘中即时**：实时价 > 本根开盘价 ⇒ 做多，< ⇒ 做空，不等收盘；`break_prev`=**等收盘突破**：本根收盘价 > 上一根最高价 ⇒ 做多，< 上一根最低价 ⇒ 做空 |
| `once_per_bar` | `false` | 每根K线最多开一次：同一根里被止损出场后不再重新入场，等下一根 |
| `swing_bars` | `3` | 止损用最近几根的最低/最高价。信号根**不算**在内。⚠ 这个数直接决定持仓时长：随机游走下平均持仓约 N+1 根，N=3 配短周期时交易次数会非常高 |

`break_prev` 比「刚收盘那根是阳线就做多」严格得多——下跌趋势里的一根阳线只是噪音，
而突破前一根的高点是真正的动能信号。旧的阳线规则已在 v5.1 删除。

⚠ `immediate` **天然抖**：新K线刚开盘时价格 ≈ 开盘价，微小波动会让多空条件反复翻转。
配上 `reverse: "immediate"` 就是一根K线内来回开平好几次，每次两笔手续费。
这个组合在启动时会告警并建议开 `once_per_bar`——它是这个模式唯一的抖动护栏。

### 从 v5.0.x 升级

旧键仍然认，但**每一处行为变化都会在启动日志里明确报出来**，不会静默生效：

| 旧写法 | 迁移结果 | 告警 |
|---|---|---|
| `mode: "donchian"` | `strategy: "turtle"` | 有（行为不变） |
| `mode: "bar"` | `strategy: "bare_k"` + `bare_entry: "break_prev"` | 有，**入场规则变了**：旧的阳线入场已删除，新规则严格得多，信号会明显变少 |
| `allow_reverse: true` + `reverse_needs_signal: false` | `reverse: "immediate"` | 有（行为不变） |
| `allow_reverse: true` + `reverse_needs_signal: true` | `reverse: "none"` | 有，**行为有差别**：新枚举里没有「反手但等反向信号」这一档，`none` 是保护性更强的那边，差别是现在同向信号先来也会再进一次 |
| `pyramid_max_adds` / `pyramid_step_atr` | 忽略 | 有（未知键告警）。金字塔加仓已整体移除，三个策略都不加仓 |

新键在场时旧键**完全不起作用**——两套键同时被尊重是最难查的一类 bug。

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
`closePosition` 单。它在**开仓成交那一刻挂一次，此后不动**，直到仓位关闭才撤；
重启对账完成后自动重建。

> v5.0.1 之前它镜像移动止损（线每推进超 0.5% 就撤旧挂新）。改成固定的理由：撤挂
> 之间有一个「旧单已撤、新单未挂」的空窗，而这张单的全部职责就是「进程死了兜住」，
> 空窗期正是最不该有它不在的时候。代价是趋势走远后它仍停在开仓时的位置——这是取舍，
> 它的定位是**最大风险兜底**，不是第二条移动止损。

```json
{ "symbol": "BTCUSDT", "use_disaster_stop": true, "disaster_stop_buf_dist_pct": 20.0, ... }
```

**`disaster_stop_buf_dist_pct` 不能是 0。** 挂单价 = 止损线再往外扩**止损距离的**
这么多%（多头往下、空头往上）。交易所用连续的标记价触发，而本地是采样的——挂在止损线
【上】的话交易所几乎总会先触发，于是主出场路径从"本地 reduceOnly 平仓"变成"交易所平掉、
本地靠对账才发现"，而对账发现外部平仓会**停掉那个 bot**。等于把一次正常的止损出场变成
一次需要人工介入的事件。留出缓冲之后：正常情况本地先平并撤掉这张单，只有进程真的不在了，
价格才会继续走到它上面。

缓冲占的是**止损距离**而不是价格（v5.10.0 改的口径）。举例，两个都填 20：

| | 开仓价 | 止损线 | 止损距离 | 缓冲 | 兜底单 | 进程死了最坏亏 |
|---|---|---|---|---|---|---|
| 止损紧 | 110 | 107 | 3 | 0.6 | 106.4 | 1.2× 计划风险 |
| 止损松 | 110 | 98 | 12 | 2.4 | 95.6 | 1.2× 计划风险 |

倍数恒定，与品种波动率、杠杆、止损松紧都无关。占价格的老口径下这两行会拿到
**一样**的缓冲，而它们的计划风险差 4 倍。

它不参与常规交易，只在进程不在时兜底。


