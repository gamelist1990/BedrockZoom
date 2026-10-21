# BedrockZoom

[BedrockDynamicSDK](https://github.com/gamelist1990/BedrockDynamicSDK) 用の公開Zoomサンプルです。
Minecraft Bedrock 1.26.52.3 (Windows x64) を対象にしています。

## 特徴

- デフォルトは `C` を押している間だけズーム
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
  "smooth": true,
  "animation_speed": 12.0,
  "reload_interval_ms": 250
}
```

保存後、`reload_interval_ms` 以内に再読み込みされます。不正なJSONは無視し、最後に正常に
読めた設定を維持します。`key` は `A-Z`、`0-9`、`F1-F12`、`SPACE`、`TAB`、
`ENTER`、`ESC`、`SHIFT`、`CTRL`、`ALT` に対応します。

## 仕組み

```text
Minecraft native key-event callback
  -> InputKey C の押下/解放
  -> Zoom Controller（hold/toggle + 時間ベース補間）
  -> LevelRendererPlayer::getFov の戻り値を光学倍率で変換
```

対象EXEのハッシュがSDKプロファイルと一致した場合だけフックを有効化します。SDK依存は
`BEDROCK_SDK_REF=main` がデフォルトなので、通常のビルドは常にSDK `main` の最新版を使用します。
再現ビルドが必要な場合だけCMake設定でコミットを明示できます。

## ビルド

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

成果物は `build\Release\BedrockZoom.dll` です。
