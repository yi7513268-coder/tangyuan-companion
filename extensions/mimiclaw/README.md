# MimiClaw实验扩展源码

本目录不由默认SDK app.py启动。需要自定义ESP32-S3固件、兼容Himax/PTL和底座接口，使用自有认证HTTP协议；与官方SDK/Daemon路线分开。Agent循环在机器人，云端模型提供推理，电脑执行已登记工具。

- firmware/main、port：任务、工具、屏幕、音频、运动、静音和投递适配。
- firmware/upstream/mimiclaw-main：Agent循环、消息总线与所需头文件；MIT许可及快照哈希保留。没有取得确切Git提交号，未伪造版本。
- pc：公开HTTPS读取、隔离工作区、应用登记、固定CSV统计等组件。没有任意shell或消息发送工具；没有任何用户的令牌/凭据文件。

不附完整语音环境、声纹/记忆、参考音色、模型或可直接烧写镜像。cached_audio.c已替换为空实现，合法TTS需由使用者配置。

原型使用ESP-IDF 6.0.2及公开硬件SDK。构建者必须显式指定兼容公开固件目录WATCHE_PUBLIC_ROOT；本发布包调整的相对路径尚未重新完成固件构建，不提供默认flash命令，不自动切换或擦写设备。先自行核对分区、备份和恢复条件。许可证见仓库LICENSE及licenses/MimiClaw-MIT.txt。
