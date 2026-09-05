# v1.0.0 刷写说明

仅用于 OSPTEK ESP32-S3-Touch-LCD-4 / ESP32-TPCB4，ESP32-S3-WROOM-1-N16R8，480×480 ST7701S RGB/FT5x06。

解压到单独目录，先用 `SHA256SUMS.txt` 核对三个 bin。Windows 可执行 `Get-FileHash *.bin -Algorithm SHA256`。安装 CH340 串口驱动（如系统未自动识别），在设备管理器确认实际串口，关闭其他串口监视器。

在已初始化 ESP-IDF 5.5.3 的终端，切换到解压目录。本命令使用该环境的 esptool 4.12.0；将 PORT 换成实际端口：

```powershell
python -m esptool --chip esp32s3 --port PORT --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 esp32s3-395rgb-480x480-bringup.bin
```

下载失败可降低到 115200，并检查数据线、端口和设备下载模式。

| 地址 | 文件 |
| --- | --- |
| 0x0 | bootloader.bin |
| 0x8000 | partition-table.bin |
| 0x10000 | esp32s3-395rgb-480x480-bringup.bin |

不执行 erase_flash，不写整片合并镜像；以上文件不覆盖 0x9000 的 NVS 配置区。本固件包不包含任何设备 NVS。已有其他分区布局的设备不保证兼容，应先确认型号和数据备份。

首次使用连接 `ESP32-Clock-Setup`，通用密码 `12345678`，浏览器访问 `http://192.168.4.1` 配置自己的 Wi-Fi。IP 定位失败可在配网页填经纬度。运行设置会保存在设备内。

此为当前开发版本存档，未承诺长期稳定性或其他板卡兼容性。20 km 地图加载/比例、地名缺字和网络失败恢复仍需实机回归，详见仓库 README。仅刷写构建文件，不能替代源码构建环境备份。
