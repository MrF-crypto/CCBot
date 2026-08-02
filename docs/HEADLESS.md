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
