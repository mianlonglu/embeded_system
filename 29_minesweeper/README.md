# HiSpark WiFi-IoT 鸿蒙套件样例开发--扫雷游戏（Minesweeper）

![hihope_illustration](https://gitee.com/hihopeorg/hispark-hm-pegasus/raw/master/docs/figures/hihope_illustration.png)

[HiSpark WiFi-IoT鸿蒙开发套件](https://item.taobao.com/item.htm?spm=a1z10.1-c-s.w5003-23341819265.1.bf644a82Da9PZK&id=622343426064&scene=taobao_shop) 首发于HDC 2020，是首批支持HarmonyOS 2.0的开发套件，亦是鸿蒙官方推荐套件，由润和软件HiHope量身打造，已在鸿蒙社区和广大鸿蒙开发者中得到广泛应用。

![wifi_iot](https://gitee.com/hihopeorg/hispark-hm-pegasus/raw/master/docs/figures/wifi_iot.png)

## 一、功能简介

在 SSD1306 OLED 屏（128x64）上实现经典扫雷游戏：

- 雷区 **16 列 x 6 行，共 15 颗雷**
- 白块为未翻开的格子，翻开后显示周围地雷数（1~8）
- 踩到雷游戏失败（BOOM），所有非雷格子全部翻开则胜利（WIN!）
- 首次挖雷保证安全（第一次点击及其周围 8 格不会有雷，并自动展开一片空白区域）
- 顶部状态栏显示剩余雷数 `M:xx`、已插旗数 `F:xx`、光标坐标 `RxxCxx`
- 游戏结束后按任意键（短按）重新开始一局
- **蜂鸣器音效**：移动光标、翻开格子、插旗、踩雷（低沉轰鸣）、胜利（上行琶音）各有不同提示音
- 游戏过程同步通过串口打印日志（含 ADC 原始值与换算电压，便于校准按键阈值），结束时串口输出 ASCII 雷区图

## 二、外设与主控芯片（Pegasus）引脚的对应关系

* SSD1306 OLED 显示屏——I2C 接口通信，地址 0x3C
  - GPIO13: I2C0_SDA
  - GPIO14: I2C0_SCL
* 按键——OLED 显示屏下方的 R1(S1)、R2(S2) 与核心板 USER 键 **共用 GPIO5**，通过电阻分压网络接到同一引脚，GPIO5 复用为 **ADC2**，软件按电压区分按键（不能用普通数字 GPIO 读取，否则三个键会被识别成同一个信号）：

| 按键 | 位置 | ADC 换算电压（约） | 判定区间 |
| ---- | ---- | ---- | ---- |
| R1（S1/按键1） | OLED 屏下方左键 | 0.563 ~ 0.577 V | 0.40 ~ 0.75 V |
| R2（S2/按键2） | OLED 屏下方右键 | 0.963 ~ 0.970 V | 0.80 ~ 1.20 V |
| USER（S3） | 核心板 | 0.197 ~ 0.204 V | < 0.35 V |
| 无按键 | — | 3.227 ~ 3.241 V | 其余（> 2.5 V） |

  电压换算公式：`voltage = raw * 1.8 * 4 / 4096.0`（raw 为 `hi_adc_read` 的原始值）。
* 蜂鸣器——**GPIO9/PWM0**（无源蜂鸣器，交通灯板/环境板上的蜂鸣器），PWM 输出不同频率提示音

## 三、按键操作说明

| 按键 | 操作 | 功能 |
| ---- | ---- | ---- |
| R1（S1） | 短按 | 光标右移一格（行末自动换行） |
| R1（S1） | 长按（>=0.5s） | 光标下移一格，按住可连续移动 |
| R2（S2） | 短按 | 翻开光标所在格子 |
| R2（S2） | 长按（>=0.5s） | 在光标所在格子插旗 / 取消旗 |
| R1/R2/USER | 游戏结束后短按 | 重新开始新一局 |

> 光标为闪烁的方框：在未翻开的白块上显示为黑色方框，在已翻开的格子上显示为白色方框。
> 若实测按键无响应或串键，可看串口打印的 `ADC raw=xxx, v=x.xxV`，对照上表微调 `minesweeper.c` 中的 `KEY_VLT_*` 阈值。

## 四、如何编译

1. 将 `29_minesweeper` 目录复制到 openharmony 源码的 `applications\sample\wifi-iot\app\iothardware` 目录下（或直接在本仓库 `vendor\hihope\hispark_pegasus\demo` 目录中使用）。
2. 修改 demo 目录的 `BUILD.gn`（或 openharmony 源码的 `applications\sample\wifi-iot\app\BUILD.gn`），将 `features` 改为：

```python
    features = [
        "29_minesweeper:minesweeper_demo",
    ]
```

3. 本样例复用 `12_ssd1306` 的 OLED 驱动，需要 I2C 支持；蜂鸣器需要 PWM 支持。
   若编译报错 `undefined reference to hi_i2c_xxx` / `hi_pwm_xxx`，修改
   `device/hisilicon/hispark_pegasus/sdk_liteos/build/config/usr_config.mk`：
   - `CONFIG_I2C_SUPPORT=y`（OLED）
   - `CONFIG_PWM_SUPPORT=y`（蜂鸣器音效）
   ADC 按键读取使用 `hi_adc_read(HI_ADC_CHANNEL_2, ...)`，本 SDK 中 ADC 驱动默认编译，无需额外配置。
4. 在 openharmony 源码顶层目录执行：`python build.py wifiiot`

## 五、运行结果

烧录文件后，按下 reset 按键，程序开始运行：

* OLED 屏先显示欢迎界面（MINESWEEPER 及按键说明），蜂鸣器播放开场提示音，2 秒后进入雷区界面
* 短按/长按 R1 移动闪烁光标（伴随短促提示音），短按 R2 挖雷（中音提示），长按 R2 插旗（高低双音）
* 踩雷时状态栏显示 `BOOM`、蜂鸣器播放低沉轰鸣，并标出所有地雷（白块黑叉为踩中的雷，黑底白雷为其余雷，白块黑叉也表示错误插旗）
* 翻开全部安全格子时状态栏显示 `WIN!`，蜂鸣器播放上行琶音
* 串口同步打印操作日志与按键电压，例如：

```
[Minesweeper] New game: 16 cols x 6 rows, 15 mines.
[Minesweeper] ADC raw=327, v=0.57V -> R1
[Minesweeper] (3,2) flagged, flags: 1.
[Minesweeper] ADC raw=553, v=0.97V -> R2
[Minesweeper] BOOM! Game over at (7,3).
```

### 【套件支持】

##### 1. 套件介绍  http://www.hihope.org/pro/pro1.aspx?mtt=8

##### 2. 套件购买  https://item.taobao.com/item.htm?id=622343426064&scene=taobao_shop

##### 3. 技术资料

- Gitee码云网站（OpenHarmony Sample Code等) **https://gitee.com/hihopeorg**

- HiHope官网-资源中心（SDK包、技术文档下载）[**www.hihope.org**](http://www.hihope.org/)

##### 4. 互动交流

- 润和HiHope鸿蒙技术交流-微信群（加群管理员微信13605188699，发送文字#申请加入润和官方鸿蒙群#，予以邀请入群）
- HiHope开发者社区-论坛 **https://bbs.elecfans.com/group_1429**
- 润和HiHope鸿蒙售后服务群（QQ：980599547）
- 售后服务电话（025-52668590）
