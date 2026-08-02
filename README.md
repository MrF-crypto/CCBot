# CCBOT合约监控

Binance USDT-M 永续合约的网格 / 马丁格尔（DCA）自动交易程序，C++20。图形界面版（Qt6）支持
Windows 和 macOS；核心引擎另有一个不依赖 Qt 的**无图形界面(headless)版**，配置文件驱动，
可以在 Linux 服务器上完全后台运行，见 [docs/HEADLESS.md](docs/HEADLESS.md)。

> ⚠️ **风险提示**：本项目直接对接 Binance 合约账户下真实订单接口，涉及杠杆合约交易，存在**本金全部亏损**的风险。代码按"原样"提供，不构成任何投资建议，作者不对使用本软件造成的任何资金损失负责。**请务必先在测试网（Testnet）验证策略行为，确认无误后再连接真实账户，并从小额资金开始。**

## 功能

- **网格 / 马丁格尔 DCA 策略**：8 种加仓曲线（平推、倍投、倍投Plus、三倍、平方、斐波那契、卢卡斯、递增），支持多/空/双向，可配置杠杆、间隔%、追踪建仓%、止盈/止损/冷却。
- **指标信号首单**（v2.0+）：首单不再是"一开监控就立刻市价开仓"，可以改成等 1h（可配置周期）K线的 BOLL 布林带 + RSI 信号满足才开仓，RSI 确认方式支持"瞬时快照"和"反转确认（先探底再回穿阈值）"两种模式。
- **趋势状态机**（v2.5+）：4h EMA200 + 中轨斜率判定高周期空头态——空头态暂停开新首仓（不接单边下跌的飞刀）、补仓间隔自动放大1.5倍，趋势恢复自动放行。聪明网格的核心：知道什么时候不干活。
- **周期统计**（v2.5+）：每笔平仓记录使用层数；GUI 交易明细弹窗内置按品种统计（周期数/周、胜率、累计盈亏、平均/最大层数）与调参提示；headless 版平仓明细落盘 CSV。
- **动态W模式**（v2.3+）：把策略锚点从"固定百分比"换成"布林带本身"——补仓锚定下轨（价格须在带外+跌够动态间隔）、止盈锚定上轨（触及上轨+盈利≥保底利润才激活追踪）、间隔/追踪参数由实时带宽 W 自动推导（间隔=W/3、追踪止盈=0.15W、追踪建仓=0.1W）。带子随趋势下移时梯子跟着走，均价贴着下轨，带内震荡即可完成多头周期；指标数据超时自动冻结补仓/止盈激活。
- **实时行情**：WebSocket bookTicker 订阅买一/卖一，最新成交价取买卖中间价，REST 兜底。
- **策略配置弹窗**：右键品种即可配置，含层级分配预览（每层名义价值/保证金/预计建仓价/预计浮亏、扛完全部层数总共需要多少资金）。
- **账户级风控**：总保证金上限（超过暂缓开新首仓，不影响已有仓位）。
- **持久化**：本地保存 API Key（Windows DPAPI / macOS 钥匙串）、Bot 运行状态（重启不丢仓位跟踪，原子写盘防崩溃损坏）、交易明细。
- **资金安全防线**（v2.4+）：启动时与交易所对账（外部手动平仓/强平后本地状态自动收敛并告警）；下单幂等（网络超时后查单确认，杜绝盲目重试造成重复下单）；平仓部分成交自动续平；单实例锁（防止双开同账户重复下单）。
- **关键事件提醒**：企业微信 / 飞书自定义机器人 / Telegram Bot webhook，触发硬止损、账户连接失败/网络异常时自动推送。
- **日志落盘**：按天写入本地文件，方便事后复盘。
- **实时权益/连接状态**：顶部常驻权益、可用余额、累计盈亏徽标、连接时长与呼吸灯状态指示。
- **无图形界面(headless)版**（v2.2+）：配置文件驱动，不依赖Qt，可在Linux服务器上后台常驻运行，核心策略引擎跟图形界面版完全共用一份代码，行为一致，详见 [docs/HEADLESS.md](docs/HEADLESS.md)。

## 构建（图形界面版，Windows）

依赖（通过 [vcpkg](https://github.com/microsoft/vcpkg) manifest 模式自动安装，见 `vcpkg.json`）：

- CMake ≥ 3.20
- Visual Studio（含 MSVC，C++20）
- Qt6 (Widgets)
- ixwebsocket / simdjson / curl / mbedtls

```powershell
git clone https://github.com/MrF-crypto/CCBot.git
cd CCBot
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg根目录>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

生成的可执行文件在 `build/Release/CCGMonitor.exe`。

## 构建（图形界面版，macOS）

GUI 同样支持 macOS（iMac / Mac mini / MacBook，Intel 和 Apple Silicon 都可以），
API 密钥存进系统钥匙串（Keychain）。需要先装 Xcode 命令行工具和 Homebrew：

```bash
xcode-select --install
brew install cmake ninja pkg-config

git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

git clone https://github.com/MrF-crypto/CCBot.git
cd CCBot
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-osx    # Apple Silicon（M系列）；Intel Mac 用 x64-osx
cmake --build build --config Release
./build/CCGMonitor
```

> 第一次配置时 vcpkg 会从源码编译 Qt6，耗时 1~2 小时（一次性）。首次保存 API
> 密钥时 macOS 可能弹出钥匙串授权框，选"始终允许"。

## 构建（无图形界面版，Linux 服务器）

见 [docs/HEADLESS.md](docs/HEADLESS.md)，含配置文件字段说明和 systemd 常驻部署示例。

## 测试

指标（布林带/RSI）数学逻辑有独立单元测试，不依赖网络：

```powershell
cmake --build build --config Release --target ccg_indicator_tests
build/Release/ccg_indicator_tests.exe
```

## License

[MIT](LICENSE)
