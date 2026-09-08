# TradingBot

<img src="assets/banner.jpg" alt="TradingBot" width="360">

Binance USDT-M 永续合约的**网格 / 马丁格尔（DCA）自动交易程序**，C++20 编写。

图形界面版（Qt6）支持 Windows 与 macOS；核心引擎另有一个不依赖 Qt 的 **headless 版**，配置文件驱动，可在 Linux 服务器上后台常驻。**两者共用同一份策略引擎代码**，行为完全一致。

> ⚠️ **风险提示**
>
> 本项目直接对接 Binance 合约账户的**真实订单接口**，涉及杠杆交易，存在**本金全部亏损**的风险。代码按「原样」提供，不构成任何投资建议，作者不对使用本软件造成的任何资金损失负责。
>
> **请务必先在测试网（Testnet）验证策略行为，确认无误后再连接真实账户，并从小额资金开始。**

---

## 它和「随便一个网格机器人」的区别

**① 名义仓位 ≤ 权益 ⇒ 数学上不可强平**

```
P_liq = (名义成本 − 钱包余额) / (数量 × (1 − MMR))
```

名义成本不超过权益时，分子非正 ⇒ 强平价 ≤ 0。策略配置弹窗里有**层级分配预览**：每层的名义价值、保证金、预计建仓价、预计浮亏，以及**扛完全部层数总共需要多少钱**——开仓之前就能看清最坏情况。

**② 失败必须是响的**

这个程序设计成连续挂几个月无人值守，所以最危险的不是报错，是**静默降级**：

- WebSocket 半开（连接还在、数据早停了）→ 缓存里躺着冻结价，引擎拿着僵尸价格继续决策
- 某条行情流没订阅上 → 界面一片空白，日志一个字都没有
- 兜底路径生效了 → 价格新鲜度从亚秒掉到十秒，界面看不出任何异常

这些都在实盘里真实发生过。现在每一条都会主动报告：连接状态、首包到达、行情来源切换、静默超时、批次耗时、品种精度异常。

**③ 进程外保护**

交易所侧灾难止损单（`STOP_MARKET + closePosition`）挂在币安服务器上——**程序崩了、断电了、窗口被误关了，它依然生效**。这是唯一不依赖本进程存活的保护。默认关闭，因为它会把浮亏变成实亏，是否启用属于策略取向。

**④ 只有一条真相**

回测已移出为独立项目（v4.0.13）、支撑/阻力结构层已整体移除（v4.0.16）。本仓库不再存在需要同步维护的第二份策略逻辑。

---

## 功能

### 策略

| | |
|---|---|
| **加仓曲线** | 8 种：平推 / 倍投 / 倍投Plus / 三倍 / 平方 / 斐波那契 / 卢卡斯 / **递增（默认）** |
| **动态W模式** | 把锚点从「固定百分比」换成布林带本身：补仓锚定下轨（价格须在带外且跌够间隔）、止盈锚定上轨（触上轨 **且** 盈利≥保底利润才激活追踪）。带子随趋势下移时梯子跟着走 |
| **固定补仓间隔** | 覆盖 W/3 自适应。walk-forward 实测固定 6% 全面更优 |
| **指标信号首单** | 等 1h K线的 BOLL + RSI 满足才开首仓。RSI 支持「瞬时快照」与「反转确认（先探底再回穿）」两种模式 |
| **多周期梯子** | 1h / 4h / 12h / 1d 四档带值分配层数，越深的层要求越极端的证据 |
| **快进快出** | 不等上轨、够本就跑：`tp_floor_only` / `tp_fixed_profit` / `fixed_trail_tp` |

### 开仓闸门（层层递进，任一不满足就不开新首仓）

```
① 微观扳机   1h BOLL + RSI 信号 + 追踪建仓站稳
② 趋势过滤   4h EMA200 + 中轨斜率，空头态暂停开仓、补仓间隔×1.5
③ 宏观许可   日线%B / 24h涨幅 / 近7日涨幅，三条平级独立
④ 出场价记忆 止盈后要求价格回撤够了才准重开（唯一不随时间衰减的判据）
⑤ 账户级     总保证金上限 / 并发持仓数上限 / 周期熊市总开关
```

**③ 里的三条口径正交**：%B 问「价格在波动区间的什么位置」，涨幅问「最近涨得多急」。窄幅横盘时 %B 可以贴着上轨而涨幅极小；急涨突破时涨幅巨大而 %B 未必越界。

**④ 解决的是**：币爆拉一波、止盈出场、然后横在高位——上面三条都是**无记忆的相对指标**，会随时间衰减到失效（24h涨幅 1 天、7日涨幅 7 天、%B 约 18 天），最终全部放行，机器人在山顶重新开首仓。

### 资金安全

- **对账**：启动时 + 运行中周期性与交易所核对。外部手动平仓/强平后本地状态自动收敛并告警；能从交易所均价**反推层数**认领孤儿仓位
- **下单幂等**：网络超时后按 clientOrderId 查单确认，杜绝盲目重试造成重复下单
- **部分成交续平**：平仓只成交一部分时下个 tick 继续，不留残仓
- **单实例锁**：PID 锁文件，防止双开同账户重复下单
- **原子落盘**：凭证、策略配置、成交明细、运行状态全部 tmp+rename 或 QSaveFile，崩在写文件中途也不会损坏
- **退出保护**：等在途下单任务排空再落盘；超时则跳过清理直接结束进程，绝不在已释放的对象上执行代码

### 可观测性

- **实时表**：层进度（按满层健康度着色）、均价、标记价、24h涨跌、延迟、浮动P&L、保证金、收益率、**强平价 + 距强平%**、已实现
- **顶部常驻**：权益、可用、uniMMR、累计资金费、累计盈亏、连接时长与呼吸灯
- **日志落盘**：按天分文件，重启/崩溃后可复盘
- **webhook 告警**：企业微信 / 飞书 / Telegram Bot，硬止损、连接失败、网络异常、对账不一致时推送
- **资金费账本**：永续每 8 小时结算的真实现金流出，单独记账（它和浮亏性质完全不同）

---

## 快速开始

### 1. 下载

[Releases](https://github.com/MrF-crypto/CCBot/releases) 页面下载对应平台的包：

| 平台 | 文件 |
|---|---|
| Windows x64 | `tradingbot-vX.Y.Z-win64.zip` |
| macOS Apple Silicon | `tradingbot-vX.Y.Z-macos-arm64.zip` |
| macOS Intel | `tradingbot-vX.Y.Z-macos-x64.zip` |
| Linux x64（headless） | `tradingbot-vX.Y.Z-linux-x64.zip` |

macOS 产物未做 Apple 签名/公证，解压后需要：

```bash
chmod +x TradingBot ccbot_headless ladder_depth
xattr -dr com.apple.quarantine .
```

### 2. 准备 API 密钥

币安 → API 管理 → 创建 API：

- ✅ **启用合约**
- ❌ **不要**开启提现权限
- ✅ 建议绑定 IP 白名单

### 3. 先跑测试网

程序内「设置」填入密钥，**勾选测试网**。测试网密钥单独申请：<https://testnet.binancefuture.com>

确认开仓、补仓、止盈整条链路都符合预期，再切主网。

### 4. 配置第一个策略

主界面输入品种（如 `BTCUSDT`）→ 添加 → **右键该行 →「配置策略...」**。

配置弹窗里最该看的是底部的**层级分配预览**：

```
第1层  名义 $600   保证金 $300   预计建仓价 $95000   预计浮亏 $0
第2层  名义 $1200  保证金 $600   预计建仓价 $89300   预计浮亏 -$36
...
满层合计需要 $XXXX，届时均价 $XXXXX，可扛到 $XXXXX
```

**如果「满层合计」超过你愿意投入的钱，就调小预算或层数**——这一步是开仓前唯一能看清最坏情况的地方。

---

## 参数指南

### 推荐起步配置

以下组合来自 8 品种 × 8 个滚动窗口 = **64 个纯样本外测试段**（预热 4 月 / 训练 12 月 / 测试 6 月）：

| 项 | 值 | 说明 |
|---|---|---|
| 动态 W | ✅ 勾选 | 关掉会让止盈退化成固定百分比，保底利润完全不生效 |
| 固定补仓间隔 | **6** | 6% 优于 5%/4%；3% 是净负的 |
| 保底利润 | **2.0%** | 与层数强耦合，不要单独改 |
| 曲线 / 层数 | 递增 / **10** | 10 层优于 8 层（+75192 vs +70620，61/64 vs 56/64） |
| 追踪建仓 | 0.4% | 1.0% 在深熊里灾难性（周期数归零） |
| 首仓 | 指标信号（跌破 1h 下轨） | 「立即开仓」跳过微观扳机，实测收益掉 14% |
| 趋势过滤 | ✅ 开 | 深熊段值 5643U |
| 杠杆 | **2** | 名义 ≤ 权益 ⇒ 数学上不可强平 |
| 日线%B 拦截 | 0.60 | 0.80 太松≈没拦 |
| 24h / 7日涨幅拦截 | 0（关） | 无实证依据，按需手判 |
| 止盈后重开需回撤% | 0（关） | 埋伏型策略建议填 10~15 |

> ⚠ 所有回测数字都是历史统计。同一批 walk-forward 测出**样本外留存约 34%** —— 实盘预期应按此折价。

### 两条反直觉的实测结论

**调参是有害的。** 在训练段选最优参数再用到测试段（65827），**不如**全程用固定 5%（67289）。参数搜索搜到的是噪音。

**满层率是那个枢纽变量。** 60 品种实测：满层时间占比 <10% 的品种 **22/22 盈利**；>75% 的 **0/6 盈利**（净利中位 −16854）。满层 = 弹药耗尽、失去摊薄能力。界面「层进度」列按这个着色。

### 关键参数怎么想

| 参数 | 它在回答什么 |
|---|---|
| **预算 / 层数 / 间隔** | 「我准备扛多深」——三者共同决定满层时的均价和总资金 |
| **杠杆** | 名义 ≤ 权益（即杠杆 ≤ 1 相对于投入）才数学上不可强平 |
| **保底利润** | 「至少赚多少才肯走」——太低会在中间层频繁小赚，太高会拿不住 |
| **追踪止盈回调** | 触发止盈后从最高点回落多少才平仓。实测全区间差异仅 2.1%，不敏感 |
| **冷却时间** | 止盈后多久允许重开。埋伏型策略建议配合「出场价记忆」而不是靠冷却 |

---

## 构建

### Windows（GUI）

依赖通过 [vcpkg](https://github.com/microsoft/vcpkg) manifest 模式自动安装（见 `vcpkg.json`）：CMake ≥ 3.20、Visual Studio（MSVC，C++20）、Qt6 Widgets、ixwebsocket / simdjson / curl / mbedtls。

```powershell
git clone https://github.com/MrF-crypto/CCBot.git
cd CCBot
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg根目录>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

产物：`build/Release/TradingBot.exe`

### macOS（GUI）

Intel 与 Apple Silicon 都支持，API 密钥存进系统钥匙串。

```bash
xcode-select --install
brew install cmake ninja pkg-config

git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

git clone https://github.com/MrF-crypto/CCBot.git
cd CCBot
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-osx     # Intel Mac 用 x64-osx
cmake --build build --config Release
./build/TradingBot
```

> 首次配置时 vcpkg 会从源码编译 Qt6，耗时 1~2 小时（一次性）。首次保存 API 密钥时 macOS 会弹钥匙串授权框，选「始终允许」。

### Linux（headless）

见 [docs/HEADLESS.md](docs/HEADLESS.md)，含配置文件字段说明与 systemd 常驻部署示例。

---

## 数据与备份

**便携模式**：所有数据存在**程序目录的 `data/` 子目录**——整个文件夹拷走就是完整备份。

```
data/
├── ccg_creds.dat      API 凭证（Windows DPAPI / macOS 钥匙串加密，换机需重新输入）
├── ccg_bots.json      策略配置 + 持仓跟踪状态
├── ccg_trades.json    成交明细
├── ccg_settings.json  全局设置
├── ccg_funding.json   资金费账本
└── logs/              按天分文件的运行日志
```

**升级时只替换可执行文件与 dll，保留 `data/` 目录。**

程序目录不可写时自动回退到系统 AppData；老版本的 AppData 数据首次启动会自动迁移。

---

## 测试

12 套单元测试，覆盖引擎、网络、持久化、并发：

```powershell
cmake --build build --config Release
for %t in (ccg_indicator_tests ccg_key_store_tests ccg_risk_tests ccg_funding_tests ^
           ccg_rate_tests ccg_order_tests ccg_stress_tests ccg_persist_tests ^
           ccg_config_tests ccg_price_tests ccg_timesync_tests ccg_pool_tests) do ^
    build\Release\%t.exe
```

重点覆盖的是**会静默出错**的部分：

| 测试 | 守住什么 |
|---|---|
| `ccg_price_tests` | WebSocket 半开时冻结价不得流入引擎；两条流互不覆盖；REST 兜底要写回缓存 |
| `ccg_order_tests` | 超时恢复 / 部分成交 / 零成交 / -1111 重试 |
| `ccg_risk_tests` | 保证金上限挡补仓；灾难止损单生命周期；出场价记忆；宏观闸门归因 |
| `ccg_stress_tests` | 并发撕裂、极端行情、300 轮止盈重开无状态泄漏 |
| `ccg_timesync_tests` | 往返过长的测量必须丢弃；时间戳锚定单调钟 |

三平台（Windows / Linux / macOS）CI 全量跑，发布包另有**泄漏检查硬门禁**（白名单校验 + 内嵌版本号核对）。

---

## 文档

| | |
|---|---|
| [docs/GUIDE.md](docs/GUIDE.md) | 界面与参数详解 |
| [docs/HEADLESS.md](docs/HEADLESS.md) | 无界面版配置字段与 systemd 部署 |
| [docs/DYNAMIC_W.md](docs/DYNAMIC_W.md) | 动态W模式的推导与实测 |
| [docs/BACKTEST.md](docs/BACKTEST.md) | 历史回测结论存档（工具本身已移出仓库） |

---

## License

[MIT](LICENSE)
