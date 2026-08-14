# LUT アセット

タイル型3D LUTアトラス(GPUImageレイアウト): 幅 = tiles³。512×512 = 8×8タイル×64レベル。
`lut` フィルターの `source:` にそのまま与える。

| ファイル | 内容 |
|---|---|
| `whiten_base_64.png` | gpupixel美白チェーン全段を強度0で焼いたもの(レベル補正+カーブ+origin/skin LUT) |
| `whiten_full_64.png` | 同チェーンを強度1で焼いたもの(light LUTまで) |

本家の美白は `mix(base(c), full(c), w)` で完全再現できる(検証誤差 最大0.014)。
1枚だけ使うなら `full` を `amount` でmixすれば近い見た目になるが、本家の
「強度0でも下地補正が効く」挙動は base 側との2枚mixでのみ一致する。

出自: [gpupixel](https://github.com/pixpark/gpupixel) (Apache-2.0) 同梱の
lookup_gray/origin/skin/light.png をチェーン合成して焼き直した派生物。
