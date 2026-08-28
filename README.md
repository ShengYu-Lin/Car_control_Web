# WebSocket Server 與 PWA 部署說明

## 1. 架構

```
PWA
 │ ws://RPi_IP:8080/ws
 ▼
WebSocket Server
 ├── /dev/motor → 馬達 Driver
 └── /dev/mq135 → MQ135 SerDev Driver
```

WebSocket Server 負責：

- 接收 PWA 馬達控制 JSON。
- 將馬達指令寫入 `/dev/motor`。
- 讀取 `/dev/mq135`。
- 將空氣品質傳送給所有 WebSocket Client。

## 2. 專案結構

```
/smart_car/car_control/
├── WebSocket_server.c
├── WebSocket_server.Makefile
├── WebSocket_server
├── WebSocket.service
├── smartcar-web.service
├── index.html
├── app.js
├── style.css
├── manifest.json
├── service-worker.js
└── icons/
```

systemd 實際讀取：

```
/etc/systemd/system/WebSocket.service
/etc/systemd/system/smartcar-web.service
```

## 3. 通訊格式

PWA → Server：

```json
{"type":"motor","left":50,"right":50}
```

Server → PWA：

```json
{"type":"mq135","quality":"GOOD"}
```

品質值：

| Server 值 | PWA 顯示 |
|---|---|
| GOOD | 優良 |
| NORMAL | 普通 |
| BAD | 不佳 |
| UNKNOWN | 等待資料 |

## 4. 編譯

`WebSocket_server.Makefile` 的執行檔名稱必須與 service 一致：

```make

```

編譯：

```bash
cd /smart_car/car_control
make -f WebSocket_server.Makefile clean
make -f WebSocket_server.Makefile
ls -l WebSocket_server
```

## 5. WebSocket.service

```ini
[Unit]
Description=SmartCar motor WebSocket server
Wants=network-online.target
After=network-online.target systemd-modules-load.service

[Service]
Type=simple
User=pi
Group=pi
SupplementaryGroups=motor
WorkingDirectory=/smart_car/car_control
ExecStart=/smart_car/car_control/WebSocket_server
Restart=on-failure
RestartSec=2
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

安裝與啟用：

```bash
sudo cp WebSocket.service /etc/systemd/system/WebSocket.service
sudo systemctl daemon-reload
sudo systemctl enable WebSocket.service
sudo systemctl restart WebSocket.service
```

## 6. smartcar-web.service

將網頁服務放到：

```bash
sudo cp smartcar-web.service /etc/systemd/system/smartcar-web.service
sudo systemctl daemon-reload
sudo systemctl enable smartcar-web.service
sudo systemctl restart smartcar-web.service
```

確認兩個服務：

```bash
systemctl status WebSocket.service --no-pager
systemctl status smartcar-web.service --no-pager
```

## 7. PWA WebSocket URL

`app.js`：

```javascript
function motorWebSocketUrl() {
  const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
  return `${scheme}://${window.location.hostname}:8080/ws`;
}
```

若網頁使用一般 HTTP，實際網址為：

```
ws://RPi_IP:8080/ws
```

目前 Server 沒有設定 TLS，因此 HTTPS 頁面使用 `wss://` 可能無法連線。

## 8. 多個 PWA Client

Server 必須使用每個 Client 個別的傳送版本：

```c
struct mq135_client_session {
    unsigned long last_sent_generation;
};
```

protocol 必須設定：

```c
.per_session_data_size =
    sizeof(struct mq135_client_session),
```

不能在第一個 Client 傳送後直接執行：

```c
mq135_tx_length = 0;
```

否則後加入的 PWA 會收不到同一筆資料。

## 9. 更新後標準流程

只修改 Server：

```bash
cd /smart_car/car_control
make -f WebSocket_server.Makefile clean
make -f WebSocket_server.Makefile
sudo systemctl restart WebSocket.service
```

修改 service：

```bash
sudo cp WebSocket.service /etc/systemd/system/WebSocket.service
sudo systemctl daemon-reload
sudo systemctl restart WebSocket.service
```

修改前端：

```bash
sudo systemctl restart smartcar-web.service
```

若手機仍使用舊版，清除 PWA 網站資料，或更新 `service-worker.js` 的 cache 版本。

## 10. 偵錯指令

查看 Server：

```bash
systemctl status WebSocket.service --no-pager
journalctl -u WebSocket.service -f
```

確認 8080：

```bash
ss -ltnp | grep 8080
```

正常應看到：

```
0.0.0.0:8080 LISTEN
```

確認 PWA 是否連線，Log 應出現：

```
WebSocket client connected
```

查看裝置權限：

```bash
ls -l /dev/motor /dev/mq135
id pi
```

## 11. 開機檢查清單

```bash
systemctl is-enabled WebSocket.service
systemctl is-enabled smartcar-web.service
systemctl is-active WebSocket.service
systemctl is-active smartcar-web.service
lsmod | grep mq135_serdev
ls -l /dev/mq135
ss -ltnp | grep 8080
```

兩個服務應該都是：

```
enabled
active
```

## 12. 部署順序

1. 確認 MQ135、Pico W 與 RPi5 共地。
2. 啟用 Device Tree overlay。
3. 安裝 `mq135_serdev.ko`。
4. 確認 `/dev/mq135`。
5. 編譯 `WebSocket_server`。
6. 安裝兩個 systemd service。
7. 執行 `systemctl daemon-reload`。
8. 啟用兩個 service。
9. 重新開機測試。

