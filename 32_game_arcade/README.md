# HiSpark WiFi-IoT 鸿蒙套件样例开发--三合一游戏厅（Game Arcade）

![hihope_illustration](https://gitee.com/hihopeorg/hispark-hm-pegasus/raw/master/docs/figures/hihope_illustration.png)

[HiSpark WiFi-IoT鸿蒙开发套件](https://item.taobao.com/item.htm?spm=a1z10.1-c-s.w5003-23341819265.1.bf644a82Da9PZK&id=622343426064&scene=taobao_shop) 首发于HDC 2020，是首批支持HarmonyOS 2.0的开发套件，亦是鸿蒙官方推荐套件，由润和软件HiHope量身打造，已在鸿蒙社区和广大鸿蒙开发者中得到广泛应用。

![wifi_iot](https://gitee.com/hihopeorg/hispark-hm-pegasus/raw/master/docs/figures/wifi_iot.png)

> 本项目把**扫雷、贪吃蛇、弹球消方块**三款游戏，以及一段**视频+音频播放**整合到同一个固件中。上电（或按 reset 键）后先进入游戏选择菜单，选定后进入对应游戏；任意游戏长按 USER 键可返回菜单。

## 一、文件结构（模块化）

为便于后期维护，代码按职责拆分为多个文件，各游戏互不干扰：

```
32_game_arcade/
├── arcade.h        # 共享头文件：硬件抽象接口声明、游戏模块接口
├── arcade.c        # 共享层：硬件初始化、ADC按键、蜂鸣器、LED、菜单、主循环
├── minesweeper.c   # 扫雷游戏（带红绿灯反馈）
├── snake.c         # 贪吃蛇游戏
├── breakout.c      # 弹球消方块游戏
├── movie.c         # 视频播放器（解码 RLE 帧到 OLED + 流式喂入蜂鸣器旋律）
├── frame_data.c    # 自动生成：280 帧 128×64 1bpp 视频，RLE 压缩后约 18.4 KB
├── sfx_data.c      # 自动生成：170 条 Beep 音符，匹配视频时长（约 23.3 秒）
├── BUILD.gn
└── README.md
```

### 模块依赖关系

```
minesweeper.c ─┐
snake.c       ─┤
breakout.c    ─┼──► arcade.h ──► arcade.c（唯一接触 GPIO/ADC/PWM/I2C 的文件）
movie.c       ─┤      ▲
              │      └── extern 引用 frame_data.c / sfx_data.c 中的数据符号
frame_data.c ──┴─── 仅 const 数据，无逻辑
sfx_data.c   ──── 仅 const 数据，无逻辑
```

- **arcade.c** 是唯一直接操作硬件（GPIO/ADC/PWM/I2C）的文件，对外只暴露 `KeysScan`、`SoundPlay`、`SoundSlots`、`LedPulse`、`FillRect` 等语义化接口。
- 三个游戏文件只 `#include "arcade.h"` 和 `ssd1306.h`，不直接碰寄存器，便于单独阅读、修改和测试。
- 每个游戏实现统一的三件套接口：`XxxInit()` / `XxxUpdate(now, &toMenu)` / `XxxRender()`。
- `frame_data.c` / `sfx_data.c` 是**自动生成的纯数据文件**，不要手工编辑；如需更换素材，用工具链重新生成（见第六节）。

新增游戏只需：在 `arcade.h` 加三个函数声明、新建一个 `.c` 实现、在 `arcade.c` 的菜单和状态机里注册即可。

## 二、功能简介

### 菜单

上电/reset 后显示游戏列表，`R1` 循环切换，`R2` 或 `USER` 短按确认进入。共 4 项：MINESWEEPER / SNAKE / BREAKOUT / MOVIE。

### 三款游戏 + 一段视频

| 项目 | 操作 | 特色 |
| ---- | ---- | ---- |
| **扫雷 Minesweeper** | R1 短按右移/长按下移；R2 短按挖雷/长按插旗 | 安全开→绿灯亮 1 秒；踩雷→红灯亮 3 秒+三声急促蜂鸣 |
| **贪吃蛇 Snake** | R1 左转 / R2 右转 / R2 长按暂停 | 420ms 起步、每吃食物 -5ms、最低 140ms；3 格转向队列防误触 |
| **弹球消方块 Breakout** | R1 挡板左移 / R2 挡板右移 / R2 短按发球 | 8×3 砖块阵，击球角度随落点变化，清空获胜、掉底失败 |
| **视频 Movie** | 无操作（自动播放）；USER 长按中途退出 | 128×64 1bpp 动画 280 帧（12 fps，约 23.3 秒），同步蜂鸣器单音旋律 |

所有项目中 **USER 长按** 返回菜单；游戏/视频结束后短按任意键返回菜单（视频播放中 USER 长按可中途退出，播完会停在末帧，此时短按任意键返回）。

## 三、外设与主控芯片（Pegasus）引脚的对应关系

* SSD1306 OLED 显示屏（OLED 显示板）——I2C 接口通信，地址 0x3C
  - GPIO13: I2C0_SDA
  - GPIO14: I2C0_SCL
* 按键——OLED 显示屏下方的 R1(S1)、R2(S2) 与核心板 USER 键 **共用 GPIO5**，通过电阻分压网络接到同一引脚，GPIO5 复用为 **ADC2**，软件按电压区分按键：

| 按键 | 位置 | ADC 换算电压（约） | 判定区间 |
| ---- | ---- | ---- | ---- |
| R1（S1/按键1） | OLED 屏下方左键 | 0.563 ~ 0.577 V | 0.40 ~ 0.75 V |
| R2（S2/按键2） | OLED 屏下方右键 | 0.963 ~ 0.970 V | 0.80 ~ 1.20 V |
| USER（S3） | 核心板 | 0.197 ~ 0.204 V | < 0.35 V |
| 无按键 | — | 3.227 ~ 3.241 V | 其余（> 2.5 V） |

  电压换算公式：`voltage = raw * 1.8 * 4 / 4096.0`。
* 蜂鸣器（交通灯板/环境板）——**GPIO9/PWM0**，无源蜂鸣器
* 红绿指示灯（交通灯板，高电平点亮）：
  - **GPIO10：红色 LED**（扫雷踩雷报警）
  - **GPIO11：绿色 LED**（扫雷安全开提示）

## 四、如何编译

1. 将 `32_game_arcade` 目录放到 `vendor\hihope\hispark_pegasus\demo` 下（本仓库已就位）。
2. 修改 `demo\BUILD.gn`，启用本项目并注释掉其他游戏 demo（三者都用 `APP_FEATURE_INIT` 且占用同一组外设，同时只能启用一个）：

```python
    features = [
#       "29_minesweeper:minesweeper_demo",
#       "30_minesweeper_light:minesweeper_light_demo",
#       "31_snake:snake_demo",
        "32_game_arcade:game_arcade_demo",
    ]
```

3. 编译依赖：
   - `CONFIG_I2C_SUPPORT=y`（OLED）
   - `CONFIG_PWM_SUPPORT=y`（蜂鸣器音效）

   两者在本 SDK 默认开启；ADC 驱动默认编译，无需额外配置。
4. 在 openharmony 源码顶层目录执行：`python build.py wifiiot`

## 五、视频素材再生（frame_data.c / sfx_data.c 的生成）

`frame_data.c` 和 `sfx_data.c` 由脚本从外部素材自动生成，**不要手工编辑**。如需更换视频或音频，按下面流程重新生成即可。

### 依赖

- Python 3 + Pillow + numpy
- ffmpeg（可用 `pip install imageio-ffmpeg`，会自带一个可用的 ffmpeg.exe）

### 步骤

1. 把视频（mp4/avi/mov 等）和音频（mp3/wav/m4a 等）放到任意目录。
2. 用 ffmpeg 抽帧（128×64 灰度，12 fps）：

   ```
   ffmpeg -i input.mp4 -vf "scale=128:64:force_original_aspect_ratio=decrease,pad=128:64:(ow-iw)/2:(oh-ih)/2:black,format=gray" -r 12 f%04d.png
   ```

3. 用 ffmpeg 把音频转 8 kHz 单声道 16-bit wav：

   ```
   ffmpeg -i input.mp3 -ac 1 -ar 8000 -c:a pcm_s16le audio.wav
   ```

4. 运行转换脚本（仓库未附带，可参照本节说明自行编写）：
   - 逐帧读取 PNG，二值化（阈值 127）后按 SSD1306 页寻址格式打包成 1024 字节/帧
   - 字节级 RLE 压缩每帧，输出 `frame_data.c`（含 `MOVIE_FRAME_COUNT`、`MOVIE_OFFSET[]`、`MOVIE_DATA[]`）
   - 对 wav 每 80 ms 做一次 FFT，取主频量化到最近的 MIDI 音符（C2–C7 范围，匹配无源蜂鸣器），合并相邻同音，截断到视频时长，输出 `sfx_data.c`（含 `MOVIE_SFX[]`）
5. 把两个生成的 .c 文件覆盖 `32_game_arcade/` 下的同名文件，重新编译烧录。

### 数据量参考（当前素材）

- 视频：280 帧 × 1024 B 原始 = 280 KB；RLE 压缩后 `MOVIE_DATA` 仅 **18.4 KB**（原神 PV 转黑白后有大片纯黑/纯白行，RLE 效率很高，最大单帧 262 B）。
- 音频：170 条 Beep（`freq, durMs`），约占 680 B。
- Flash 总增量约 **20 KB**，对 Hi3861 余量充裕。

## 六、运行结果

烧录后按 reset：

1. OLED 显示 `GAME ARCADE` 菜单，`>` 指向当前选中游戏
2. R1 切换到目标游戏，R2/USER 确认进入
3. 游戏中 USER 长按随时返回菜单重新选择
4. 串口打印按键 ADC 电压与游戏日志，例如：

```
[Arcade] ADC raw=327, v=0.57V -> R1
[Arcade] starting game: SNAKE
[Arcade] ADC raw=553, v=0.97V -> R2
```

选中 MOVIE 项确认后会立即在 OLED 上播放视频，蜂鸣器同步播旋律；播完停在末帧，短按任意键返回菜单，播放中 USER 长按可中途退出。

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
