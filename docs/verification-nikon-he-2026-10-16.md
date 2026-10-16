# Nikon HE/HE* NEF 解码验证 2026-10-16

分支 `hotfix/0.7.1`，基线 `v0.7.0`（`c7f12db`）。本记录对应 fork
[realllllty/RawLab](https://github.com/realllllty/RawLab) 的 v0.7.1 macOS 发布；
没有修改任何原始 RAW。

## 原因与修改

尼康 Z9、Z8、Zf、Z6III、Z5II、Z50II 等机身可用 High Efficiency（HE）和
High Efficiency*（HE*）记录 NEF。两者的 RAW 条带都是一条 intoPIX TicoRAW 写出的
JPEG XS（ISO/IEC 21122）码流，只有逐 precinct 的量化参数不同。LibRaw 0.22.2
只能识别、不能解包，v0.4.1 起 RawLab 对这类文件明确拒绝。

- 新增仅头文件的解码器 [nikon_he_decoder.h](../lutools/third_party/nikon-he/nikon_he_decoder.h)
  （LGPL-2.1-only，C++11）：码流、packet 与熵解码移植自
  [dnglab PR #835](https://github.com/dnglab/dnglab/pull/835)；逆变换采用 ISO/IEC 21122-1
  附录 F 的 star-tetrix（Cf=0、e1=e2=0），分量位置和底边处理与
  [LibRaw PR #826](https://github.com/LibRaw/LibRaw/pull/826) 中经 Adobe 校验的实现一致；
  传输曲线按该 PR 中公布的规则读取 PIH 扩展参数。未使用 Nikon SDK。
- 解码逐 precinct 流式进行，只保留几行分量缓冲，不新增整帧缓冲；45MP 帧单线程约 1 秒
  （Linux arm64 实测）。
- [patch-libraw.cmake](../lutools/cmake/patch-libraw.cmake) 把头文件复制进 LibRaw，
  替换 `nikon_he_load_raw()` 占位实现、清除 decoder_info 中的不支持标志，并让所有
  `NIKON Z` 机型按条带签名（JPEG XS SOC + CAP）路由；其余条带继续使用无损解码器。
  补丁可重复执行，四端共用。
- 只接受已验证的码流配置（TicoRAW CAP、4 个 14-bit 分量且分量 2 不做小波、5/3 小波、
  NLy=1、均匀量化、独立符号子包、13 字节 PIH 扩展）；其他 JPEG XS 码流返回不支持，
  损坏或截断返回数据错误，不输出半解码图像。

## 回归依据

样片均来自 [raw.pixls.us](https://raw.pixls.us/)（CC0），只以 URL 和 SHA-256 记录在
[nikon-he-raw-samples.json](../lutools/tests/nikon-he-raw-samples.json)，不随仓库分发：
Z8、Z9 各 HE/HE*/无损三张同场景同参数连拍，Zf 的 HE、HE* 和无损各一张。

| 检查 | 结果 |
| --- | --- |
| 与 Adobe DNG Converter 18.2 (2482) 未压缩 DNG 的 CFA 逐像素比较 | 6 个 HE/HE* 文件共 231,819,520 个样本，差异 0；3 个无损对照与 LibRaw 无损解码同样差异 0，说明比较方法不引入偏移或缩放。[运行记录](https://github.com/realllllty/RawLab/actions/runs/38902836853) |
| 流式实现对照整帧 ISO 参考（numpy，SVT-JPEG-XS 通用访问函数语义） | Z9 HE 全帧逐位一致 |
| 经打补丁 LibRaw 解包（`run_nikon_he_samples.py`） | 9/9 通过：HE/HE* 走 `nikon_he_load_raw()`、无损仍走 `nikon_load_raw()`，尺寸、错误计数 0、解包 CFA 的 FNV-1a 指纹全部一致；无损文件与未打补丁 LibRaw 的输出逐字节相同 |
| RawLab 核心 `raw_smoke` 与 `white_balance_tests`（Linux arm64） | 9 个文件 `raw_smoke` 均退出 0；三个 HE/HE* 文件白平衡测试各 38 项通过 |
| RAW 中位数相对相机内嵌 JPEG 的曝光差（每个文件自参照，消除两次拍摄间的光线变化） | Z8 无损/HE/HE*：+0.176/+0.158/+0.174 EV；Z9：−0.940/−0.848/−0.922 EV；Zf：−0.501/−0.561/−0.553 EV |
| `nikon_he_tests` 单元检查 | 46/46 通过：真实 Z8 头部与拒绝用例、拓扑、传输曲线、5/3 往返，以及测试编码器生成的 4 组合成码流（含奇数平面宽度、多 slice、raw GCLI、垂直预测、显著性跳过、子包填充、尾组额外符号位）逐像素等于独立整帧参考 |
| 损坏输入 | 200 个随机损坏的码流（Zf HE、HE* 各 100 个：改头部、改负载、截断、改长度字段、清零区块）在 ASan/UBSan 下全部干净地报错或完成，无越界或未定义行为 |

作为对照，dnglab 自带的输出级（色度增益、限定在 quad 内的变换、拟合的平方根曲线）在
同一 Z9 HE 文件上相对无损片的比值从高光约 0.92 升到最暗处约 1.42；本实现在全部亮度区间
和四个 CFA 通道保持约 0.85，剩余差异来自两次拍摄之间的曝光变化。

## 平台结果

| 平台 | 本轮实际验证 | 结果与边界 |
| --- | --- | --- |
| Linux arm64（本地） | 打补丁 LibRaw、lutools 核心与测试；CTest；9 个样片的解包、`raw_smoke`、白平衡 | CTest 12/12 通过（含 `nikon_he`、`nikon_he_raw`）；不是发布平台 |
| macOS arm64 / x86_64（GitHub 托管 macOS 15 runner） | 源码依赖、核心与应用构建；`bundle.sh`；CTest（不含仅 Metal 套件）；9 个样片经 LibRaw、核心和应用 `--smoke` 导出 | 见下方 CI 记录 |
| Windows、Android、iOS | 共用同一 LibRaw 补丁，但本轮未构建、未测试 | 未验证 |

CI 记录：待 macOS 构建完成后补充。

## 限制

- 实际解码过的机身只有 Z8、Z9 和 Zf。dnglab 测试中的 Z6III HE* 头部与这些文件相同，
  LibRaw PR #826 的 Adobe 比较也覆盖 Z6III 和 Z5II，但本仓库没有解码过 Z6III、Z5II、
  Z50II、ZR 或任何裁切模式的文件；配置不符的码流会被拒绝。
- macOS 包在托管 runner 上以无界面方式完成构建和导出测试，没有人工 GUI 操作，也没有在
  物理 macOS 15 设备上运行。包为 ad-hoc 签名，未公证。
- 白平衡与色彩沿用与无损 NEF 相同的元数据和矩阵；本轮不涉及色彩标定。
