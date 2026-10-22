# BedrockZoom

[BedrockDynamicSDK](https://github.com/gamelist1990/BedrockDynamicSDK) 用の公開Zoomサンプルです。
Minecraft Bedrock 1.26.52.3 (Windows x64) を対象にしています。

## 特徴

- デフォルトは `C` を押している間だけズーム
- ズーム中のホイールで倍率を調整し、そのホイール入力は消費してホットバー移動を防止
- 調整した倍率を `config.json` に保存し、ズーム再開時・Minecraft再起動後も復元
- フレームレート非依存のスムーズなズームアニメーション
- `hold` / `toggle` の切り替え
- Minecraftのネイティブキーイベント（`InputKey`）と `LevelRendererPlayer::getFov` を直接使用
- `GetAsyncKeyState`、キーボードフック、WndProcフックは不使用
- `config.json` の保存を検知してゲーム中に自動反映
- メニュー、チャット、インベントリなどカーソル解放中は入力を解除

## 設定

初回起動時に次のファイルを自動作成します。

```text
C:\Users\<ユーザー名>\Documents\PEXData\Zoom\config.json
```

```json
{
  "enabled": true,
  "key": "C",
  "mode": "hold",
  "zoom_multiplier": 4.0,
  "scroll_adjustment": true,
  "scroll_step": 0.5,
  "smooth": true,
  "animation_speed": 12.0,
  "reload_interval_ms": 250
}
```

保存後、`reload_interval_ms` 以内に再読み込みされます。不正なJSONは無視し、最後に正常に
読めた設定を維持します。`key` は `A-Z`、`0-9`、`F1-F12`、`SPACE`、`TAB`、
`ENTER`、`ESC`、`SHIFT`、`CTRL`、`ALT` に対応します。

`scroll_adjustment` が有効な場合、ズームキーが有効な間だけホイール上で倍率を
`scroll_step` 増やし、ホイール下で減らします（1.0～50.0）。変更された
`zoom_multiplier` は自動保存されます。ズーム中に消費したホイールイベントはMinecraftへ
渡さないため、ホットバーの選択スロットは動きません。

同じフォルダの `Zoom.log` には、起動、対応プロファイル、FOV hookの初回動作、
致命的エラーだけを記録します。キー、スクロール、カーソル状態は毎回出力しないため、
通常利用時は数行に収まります。

## 仕組み

```text
Minecraft Keyboard::feed(InputKey, down)
  -> InputKey C の押下/解放
Minecraft MouseDevice::feed(MouseWheel, delta)
  -> ズーム中だけ倍率更新 + イベント消費
  -> Zoom Controller（hold/toggle + 時間ベース補間）
  -> LevelRendererPlayer::getFov の戻り値を光学倍率で変換
```

起動時にBedrockDynamicSDKのGitHub `main` からmanifestと実行中Minecraftに対応する
バージョン別JSONを取得します。対象EXEのハッシュがそのプロファイルと一致した場合だけ
フックを有効化するため、対応データをGitHubへ追加すれば既存DLLでも利用できます。
SDK依存は`BEDROCK_SDK_REF=main` がデフォルトなので、通常のビルドもSDK `main` の最新版を使用します。
再現ビルドが必要な場合だけCMake設定でコミットを明示できます。

## ビルド

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

成果物は `build\Release\BedrockZoom.dll` です。
