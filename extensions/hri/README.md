# HRI独立实验组件

不由默认SDK入口加载，不构成完整论文复现。

- models.py：监督校准DBN、朝向边沿门控和合成奖励的Q-learning软件实验；没有ICRA作者CPT/CACAS原模拟器。
- listener.py：应答机会分数到有界点头请求的门控；阈值/冷却/动作是工程适配，实物点头尚未通过。
- vap_worker.py：作者MaAI中文单路组件；通过TANGYUAN_VAP_ASSETS指定资产目录。目录须包含upstream/maai/src/maai和models目录中的公开校验权重；本包不附代码检出、权重或安装环境。
- 主VAP/CPC文件核对assets/models/manifest.json；Backchannel核对sources/20261024/bc-checkpoint.json，并按worker中路径配置。

作者项目：https://github.com/MaAI-Kyoto/MaAI 。请分别核对作者代码及权重许可。没有重新发布真人训练图片、现场音视频、VAP或参与模型权重。整体交互收益需真实对照，不能凭模型已加载声称改进。
