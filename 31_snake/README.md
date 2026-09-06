# HiSpark WiFi-IoT 鸿蒙套件样例开发--贪吃蛇游戏（Snake）

![hihope_illustration](https://gitee.com/hihopeorg/hispark-hm-pegasus/raw/master/docs/figures/hihope_illustration.png)

[HiSpark WiFi-IoT鸿蒙开发套件](https://item.taobao.com/item.htm?spm=a1z10.1-c-s.w5003-23341819265.1.bf644a82Da9PZK&id=622343426064&scene=taobao_shop) 首发于HDC 2020，是首批支持HarmonyOS 2.0的开发套件，亦是鸿蒙官方推荐套件，由润和软件HiHope量身打造，已在鸿蒙社区和广大鸿蒙开发者中得到广泛应用。

![wifi_iot](https://gitee.com/hihopeorg/hispark-hm-pegasus/raw/master/docs/figures/wifi_iot.png)

## 一、功能简介

在 SSD1306 OLED 屏（128x64）上实现经典贪吃蛇游戏：

- 蛇身沿当前方向自动前进，吃到食物后身体变长、速度加快
- 撞墙或撞到自己身体则游戏结束
- 顶部状态栏显示**分数** `S:xx`、**蛇长** `L:xx`、**当前移动间隔** `xxxms`
- 暂停时状态栏显示 `PAUSE`，游戏结束显示 `OVER`
- 游戏结束后按任意键（短按）重新开始一局
- **蜂鸣器音效**：开场琶音、吃食双音提示、撞墙低沉轰鸣、暂停短音
- 游戏过程同步通过串口打印日志（含 ADC 原始值与换算电压，便于校准按键阈值）

## 二、外设与主控芯片（Pegasus）引脚的对应关系

* SSD1306 OLED 显示屏（OLED 显示板）——I2C 接口通信，地址 0x3C
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
* 蜂鸣器（交通灯板/环境板）——**GPIO9/PWM0**，无源蜂鸣器，PWM 输出不同频率提示音

> 按键在 OLED 显示板上，蜂鸣器在交通灯板/环境板上；玩本游戏时底板需同时插着两块板才能完整体验（只插 OLED 板也能玩，只是没有声音）。

## 三、游戏规则与难度

* **棋盘**：32 列 × 12 行（每格 4 像素），蛇初始长度为 3，位于棋盘中央，方向向右
* **食物**：随机生成在空格上，以 2×2 中心点显示，与 3×3 的蛇身块视觉上可区分
* **得分**：每吃一个食物 `S`（分数）+1，蛇长 `L` +1
* **加速**：初始每 420ms 移动一步，每吃一个食物间隔减 5ms，最快 140ms/步（加速更平缓，避免后期过快）
* **死亡**：蛇头撞墙或撞到自身则游戏结束；若填满整个棋盘（384 格）则胜利
* **防自杀**：不能直接 180° 反向（例如向右时按左键不会立即向左撞自己）；转向输入进入长度为 3 的队列，每移动一步生效一次，快速连按不会丢失

## 四、按键操作说明

| 按键 | 操作 | 功能 |
| ---- | ---- | ---- |
| R1（S1） | 短按 | 左转 90°（逆时针） |
| R2（S2） | 短按 | 右转 90°（顺时针） |
| R2（S2） | 长按（>=0.5s） | 暂停 / 继续 |
| R1/R2/USER | 游戏结束后短按 | 重新开始新一局 |

> 若实测按键无响应或串键，可看串口打印的 `ADC raw=xxx, v=x.xxV`，对照表二微调 `snake.c` 中的 `KEY_VLT_*` 阈值。

## 五、音效说明

| 游戏事件 | 蜂鸣器声音 |
| ---- | ---- |
| 开场 / 重新开局 | 上行琶音（E5 → A5 → C6） |
| 吃到食物 | 双音（E6 → G6） |
| 暂停 / 继续 | 单短音（A5） |
| 撞墙/撞身（游戏结束） | 低沉轰鸣（A3 → E3） |

> 音效采用非阻塞队列实现（主循环 20ms 轮询推进），播放期间不影响蛇的移动与画面刷新。

## 六、如何编译

1. 将 `31_snake` 目录复制到 openharmony 源码的 `applications\sample\wifi-iot\app\iothardware` 目录下（或直接在本仓库 `vendor\hihope\hispark_pegasus\demo` 目录中使用）。
2. 修改 demo 目录的 `BUILD.gn`（或 openharmony 源码的 `applications\sample\wifi-iot\app\BUILD.gn`），启用本版并注释掉其他游戏 demo（三个游戏都使用 `APP_FEATURE_INIT` 且占用同一组外设，同一时间只能启用一个）：

```python
    features = [
#       "29_minesweeper:minesweeper_demo",
#       "30_minesweeper_light:minesweeper_light_demo",
        "31_snake:snake_demo",
    ]
```

3. 本样例复用 `12_ssd1306` 的 OLED 驱动，需要 I2C 支持；蜂鸣器需要 PWM 支持。
   若编译报错 `undefined reference to hi_i2c_xxx` / `hi_pwm_xxx`，修改
   `device/hisilicon/hispark_pegasus/sdk_liteos/build/config/usr_config.mk`：
   - `CONFIG_I2C_SUPPORT=y`（OLED）
   - `CONFIG_PWM_SUPPORT=y`（蜂鸣器音效）

   ADC 按键读取使用 `hi_adc_read(HI_ADC_CHANNEL_2, ...)`，本 SDK 中 ADC 驱动默认编译，无需额外配置。
4. 在 openharmony 源码顶层目录执行：`python build.py wifiiot`

## 七、运行结果

烧录文件后，按下 reset 按键，程序开始运行：

* OLED 屏先显示欢迎界面（SNAKE 标题及按键说明），蜂鸣器播放开场琶音，2 秒后进入游戏界面
* 蛇从棋盘中央向右自动移动，短按 R1/R2 控制转向（每步只接受一次转向，防止快速双击反向自杀）
* 吃到食物：蛇身变长、速度加快、蜂鸣器双音提示
* 长按 R2：暂停（状态栏显示 `PAUSE`、蜂鸣短音），再次长按继续
* 撞墙或撞身：状态栏显示 `OVER`，蜂鸣器低沉轰鸣
* 游戏结束后短按任意键重新开局
* 串口同步打印操作日志、按键电压与得分，例如：

```
[Snake] New game: 32x12 grid.
[Snake] ADC raw=327, v=0.57V -> R1
[Snake] Eat! score=1, len=4, speed=292ms
[Snake] ADC raw=553, v=0.97V -> R2
[Snake] Game over! score=5, len=8
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
