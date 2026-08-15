# モバイル対応計画（対応予定）

Status: **計画**（2026-08-16 調査済・実装は未着手）。
このドキュメントは引き継ぎ用。調査の結論と、着手する人向けの手順を残す。

## 結論

スマホ対応は**ほぼ実現済みの延長線**にある。wasm 版が現状でもスマホの
ブラウザで動く作りになっており（Pointer Events + `touch-action:none`、
WebGPU→CPU 自動フォールバック、viewport meta、lsd.html は portrait 判定
まで実装済み）、残る作業は小さい。ネイティブアプリは当面不要で、必要に
なったときにいつでも包める（コアはポータブル C++17 + フラット C ABI）。

## フェーズ

### Phase M1 — Web 仕上げ（半日〜1日）

- [x] `-msimd128` を wasm ビルドに追加（2026-08-16 済。CPU パス高速化。
      代償として **Safari 16.4 / iOS 16.4 未満ではロード不可**＝iPhone 7
      以前切り捨て。許容と判断）
- [ ] `getUserMedia` に `facingMode` 追加＋イン/アウトカメラ切替ボタン。
      現状は `{width, height}` のみでスマホだとどちらが掴まれるか不定。
      フロントカメラ時は左右ミラーも
- [ ] 実機確認。配信は GitHub Pages（プレイグラウンドが既に
      https://takatronix.github.io/fluent_scene/ で公開中なのでそのまま）。
      **HTTP の LAN 配信ではスマホのカメラは動かない**（secure context 必須）

### Phase M2 — PWA 化（半日）

- [ ] manifest.json + アイコン。ホーム画面追加・全画面起動になる。
      ネイティブアプリの動機の大半（アイコンから起動したい）はこれで足りる

### Phase M3 — ネイティブ（需要が出たら）

順番は **Android → iOS**。

- **Android**: NDK + 既存 Vulkan バックエンドがほぼそのまま載る
  （Vulkan は Android のネイティブ API）。カメラ→GPU テクスチャの
  ゼロコピーパスが取れるのが wasm 版との本質的な差分
- **iOS**: MoltenVK（Vulkan→Metal 変換）経由か Metal バックエンド追加が
  必要で、移植作業が一段多い。**実行速度の問題ではない**（MoltenVK の
  オーバーヘッドは小さい）。Web 版なら iOS 26 Safari は WebGPU 対応済みで
  GPU パスに乗る

ネイティブが正当化される条件（どれかが要件になったら M3 に着手）:

1. カメラのゼロコピー（wasm 版は video→canvas→ヒープのコピー経路が
   4K/高 fps でボトルネック）
2. 露出・フォーカス・複数カメラ同時など Web API で触れないカメラ制御
3. App Store / Play Store 配布・収益化

### Phase M3-CI — ライブラリ自動ビルド（**実装済 2026-08-16**）

`.github/workflows/mobile-libs.yml` が main への push（src/include/CMake
変更時）で走り、artifact を吐く:

- **Android**: NDK arm64-v8a、`libfluent_scene.a` + `libfs_harfbuzz.a` +
  `libfreetype.a` + ヘッダ一式
- **iOS**: device arm64 + simulator arm64 を libtool でマージした
  **fluent_scene.xcframework**（依存込み1本、そのまま Xcode に投げられる）

実現の仕組み（CMakeLists 側、ローカル検証済み）:

- `FS_FETCH_TEXT=ON` — freetype 2.13.3 を FetchContent、harfbuzz 10.2.0 は
  **単一ファイル amalgamation（src/harfbuzz.cc、上流公認の埋め込み経路）**
  でビルド。harfbuzz 自身のビルドシステムを走らせないので、in-tree
  freetype を find_package できない問題を根本から回避
- `FS_BUILD_TOOLS=OFF` — examples/tools/tests を丸ごとスキップ
- どちらもクロスコンパイル時は自動で既定値が切り替わる
- Vulkan は任意依存のまま: glslc の無いランナーでは CPU レンダラのみの
  ライブラリになる（それで本番品質）。CI で Vulkan 有効化は将来課題

残: wasm/api.cpp は emscripten 専用（`EMSCRIPTEN_KEEPALIVE`）なので、
モバイル向け C ABI として使うにはマクロを可搬化する小改修が要る
（M3 本体の最初の一歩）。

## 前提知識

- wasm ビルドは `./wasm/build.sh --webgpu` で（`--webgpu` を忘れると
  WebGPU バックエンドが消える）。要 emsdk + glslc + naga
- モバイル WebGPU 対応状況: Android Chrome 121+（2024/1〜）、
  iOS 26 Safari（2025/9〜）。それ以前は CPU レンダラにフォールバック
  （1 LSB パリティ検証済み）
- face/gaze/lsd デモは MediaPipe を CDN から読むためオフラインでは顔系のみ不動
- CJK フォント版（約 21 MB）はモバイルには重い。サブセット化が必要
