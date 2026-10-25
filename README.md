# 汤圆：桌面陪伴与机器人交互实验

汤圆结合机器人自身的摄像头与动作能力、电脑工具协作和 HRI（人机交互）研究。本仓库提供 **可由官方 SDK Runtime 启动的视觉基础应用**，并附 **MimiClaw 与 HRI 的独立实验扩展源码**。作者署名：Tangyuan Lab。版本：0.1.0。

## 真机演示视频

[观看／下载真机演示](https://github.com/yi7513268-coder/tangyuan-companion/releases/tag/demo-2026-10-25)（约2分59.56秒，原始MP4约20.5MB）。进入页面后，点击 Assets 中的 `tangyuan-real-robot-demo.mp4`。这是用户实机录制；视频不代表所有扩展或新设备安装已完成验收。

## 功能与运行位置

| 部分 | 提供内容 | 运行条件与验证范围 |
| --- | --- | --- |
| SDK 视觉基础应用 | 机器人摄像头取图、MediaPipe 人脸与手势识别、稳定目标选择、头部跟随、暂停恢复、状态预览及资源释放 | 识别运行在 Windows 电脑；运动由机器人执行。公开目录已通过静态检查和纯逻辑测试，全新设备安装待验收 |
| MimiClaw 实验扩展 | 机器人端 Agent 循环、工具与硬件适配、电脑文件和应用工具组件 | 使用自定义实验固件、电脑辅助及云端模型。原型已实物验证创建文件、读回核对和打开记事本；源码需单独组装配置 |
| HRI 实验扩展 | 作者 VAP 组件适配、参与状态估计、交互机会与朝向门控、策略模拟 | 需要另外获取作者代码、权重和依赖；默认基础入口不加载，整体交互收益尚待系统对照测试 |

默认 SDK 应用不启动语音助手、微信监听、主人识别、声纹或私人记忆。完整实验语音环境和可直接烧写的固件镜像不在本仓库中。

## 快速开始：SDK 视觉基础应用

### 1. 准备环境

- Windows，建议 Python 3.11；本包 SDK 范围见 `app.json`，本地检查使用 SDK 0.1.11。
- 支持官方 SDK 连接、`camera.capture` 与 `motion` 的机器人固件，并有稳定供电。
- 按[官方 SDK 说明](https://github.com/orulink-ai/WatcheRobot_python_sdk)完成设备连接。配对时电脑和机器人应在同一 Wi-Fi 网络。
- 自定义 MimiClaw 固件与下面的官方 SDK 路线不同，请先确认当前运行路线。本说明不要求或自动执行刷机。

解压本仓库到一个新目录，在包含 `app.json` 的目录打开 PowerShell。已配置好隔离 SDK 环境的用户可以复用自己的兼容环境；新环境可按以下方式准备：

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

依赖固定版本与 `app.json` 一致。不要在 `app.py` 中填写密钥，也不需要安装 Node.js。

### 2. 配对、检查并运行

机器人已连 Wi-Fi 时，打开其官方 Python SDK 应用，使用屏幕上当前显示的六位配对码。将下面的 `123456` 换成实际配对码：

```powershell
.\.venv\Scripts\watcherobot.exe robot pair 123456
.\.venv\Scripts\watcherobot.exe robot status
.\.venv\Scripts\watcherobot.exe app check .
.\.venv\Scripts\watcherobot.exe app run
```

使用已有兼容环境时，也可直接执行 `watcherobot app check .` 与 `watcherobot app run`。必须经 SDK Runtime 启动，不使用 `python app.py`，以便 Runtime 注入 `ApplicationContext` 和设备通道。

### 3. 查看和操作

打开 **http://127.0.0.1:18810/**。这是运行者电脑上的本地控制页，不是应用广场作品链接。

- 首次跟随和手势动作默认暂停；确认供电、摆放和画面后，再通过页面开启。
- 查看目标框、手势识别状态及动作记录。识别框或指令成功不等于实物动作成功，仍需观察机器人实际表现。
- 若方向与本机装配不符，先暂停，再按界面与 `config.json` 调整方向和安全角度限位。
- 正常退出时由应用停止自己持有的运动和视觉会话。不要同时启动另一套程序争用摄像头或电机。

首次启动会下载两项公开 Google 视觉模型，核对固定文件大小和 SHA256。模型来源见 `models/manifest.json`，也可预先执行 `python download_models.py`。下载失败会明确停止；仓库不分发这些模型权重。

## 独立实验扩展

### MimiClaw 与工具任务

入口说明见 [extensions/mimiclaw/README.md](extensions/mimiclaw/README.md)。包含板端和电脑组件参考源码，不提供开箱即用的实验镜像或完整语音环境。

实机原型曾完成：机器人接收任务，调用电脑工具创建和保存文件、读回核对，再打开结果或记事本。电脑工具必须在电脑运行；机器人端 Agent 与云端大语言模型是不同部分。不能把云端 API 调用说成大语言模型在机器人芯片上运行。

### HRI：交互时机与参与状态

入口说明见 [extensions/hri/README.md](extensions/hri/README.md)。

- VAP 使用作者中文单路组件及适配入口，未联合复现完整 TurnGPT 系统，也未完成原论文的用户实验。
- 参与状态 S1/RF 子组件在公开留出数据上的准确率为 46.5%，多数类基线为 56.7%，尚不足以直接控制是否打扰用户。
- 主动建议原型使用 ProactiveAgent 提示词并替换为所配置的云端模型，未复现作者微调和奖励模型。
- **未完成原版 CACAS 复现。** 本仓库的合成奖励策略模拟不是用户体验结果，固定展示脚本也不是模型有效性的实验证据。
- 倾听点头尚未通过实物验收；不把软件测试或动作回执当作已看到机器人点头。

## 验证记录与限制

已完成的本地检查及未验证事项见 [VALIDATION.md](VALIDATION.md)。主要边界：

- 这是研究原型，尚未完全脱离电脑；网络、识别、合成和调度可能造成延迟。
- 公开目录通过不代表新电脑、新机器人安装即通过；实验扩展经公开目录搬移后还需重新构建和验收。
- 第三方服务和研究模型须由使用者自行配置和获取；是否可用及相应许可，以原作者与服务提供者要求为准。

## 仓库结构

```text
app.py / app.json       官方 Runtime 托管入口与依赖声明
service.py             视觉会话、运动协调和本地页面服务
control.py             跟随控制逻辑
perception.py          人脸与手势识别
download_models.py     公开模型下载与固定校验
config.json            公开基础配置与安全动作限位
models/manifest.json   模型来源与摘要
tests/                 控制逻辑和模型校验测试
extensions/mimiclaw/   板端 Agent 和电脑工具实验参考
extensions/hri/        HRI 研究组件适配与实验参考
licenses/              第三方许可与来源
```

## 公开数据范围

源码按白名单整理，已排除 API/Wi-Fi 凭据、插件签名私钥、设备令牌、脸库、声纹、私人记忆、聊天和消息正文、现场日志、图像、录音、原音色参考、整片固件备份及研究权重。使用者自行配置自己的服务与数据。

## 来源与许可

- [WatcheRobot Python SDK](https://github.com/orulink-ai/WatcheRobot_python_sdk) 与[公开固件](https://github.com/orulink-ai/WatcheRobot-public)。本作品为用户实验项目。
- [MimiClaw](https://github.com/memovai/mimiclaw)：上游 MIT 许可与快照来源保留在 `licenses/` 和扩展目录；本仓库固件相关派生适配按根目录 GPL-3.0 提供。第三方代码与模型保留各自许可。
- [MediaPipe 手势方案](https://ai.google.dev/edge/mediapipe/solutions/vision/gesture_recognizer)：本仓库提供下载校验清单，不重新分发权重。
- 研究参考：[机器人对话轮次预测](https://arxiv.org/abs/2501.08946)、[NAACL 2025 倾听反馈](https://aclanthology.org/2025.naacl-long.367/)、[参与状态研究](https://doi.org/10.1007/s00521-025-11317-z)、[ProactiveAgent](https://github.com/thunlp/ProactiveAgent)。使用或改编的具体范围见上文，引用不代表完成整篇论文复现。
