# CCBOT合约监控

Binance USDT-M 永续合约的网格 / 马丁格尔（DCA）自动交易程序，C++20。图形界面版（Qt6）仅支持
Windows；核心引擎另有一个不依赖 Qt 的**无图形界面(headless)版**，配置文件驱动，可以在 Linux
服务器上完全后台运行，见 [docs/HEADLESS.md](docs/HEADLESS.md)。

> ⚠️ **风险提示**：本项目直接对接 Binance 合约账户下真实订单接口，涉及杠杆合约交易，存在**本金全部亏损**的风险。代码按"原样"提供，不构成任何投资建议，作者不对使用本软件造成的任何资金损失负责。**请务必先在测试网（Testnet）验证策略行为，确认无误后再连接真实账户，并从小额资金开始。**

## 功能

- **网格 / 马丁格尔 DCA 策略**：8 种加仓曲线（平推、倍投、倍投Plus、三倍、平方、斐波那契、卢卡斯、递增），支持多/空/双向，可配置杠杆、间隔%、追踪建仓%、止盈/止损/冷却。
- **指标信号首单**（v2.0+）：首单不再是"一开监控就立刻市价开仓"，可以改成等 1h（可配置周期）K线的 BOLL 布林带 + RSI 信号满足才开仓，RSI 确认方式支持"瞬时快照"和"反转确认（先探底再回穿阈值）"两种模式。
- **实时行情**：WebSocket bookTicker 订阅买一/卖一，最新成交价取买卖中间价，REST 兜底。
- **策略配置弹窗**：右键品种即可配置，含层级分配预览（每层名义价值/保证金/预计建仓价/预计浮亏、扛完全部层数总共需要多少资金）。
- **账户级风控**：总保证金上限（超过暂缓开新首仓，不影响已有仓位）。
- **持久化**：本地保存 API Key（Windows DPAPI 加密）、Bot 运行状态（重启不丢仓位跟踪）、交易明细。
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
