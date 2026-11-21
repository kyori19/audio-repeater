# Audio Repeater Exclusive

Windows 10 / 11 x64 向けの WASAPI Exclusive 音声中継ツールです。48 kHz の PCM24 入力を PCM16 出力につなぎます。モノラルとステレオに対応し、入力・出力のクロック差は libsamplerate で補正します。選んだ端子が排他形式に対応している必要があります。

## 使い方

1. Windows x64 の ZIP を展開し、常用するフォルダーから `audio-repeater.exe` を起動します。
2. 入力と出力を選び、**Start** で中継を始めます。**Stop** で現在の中継と再接続待ちを停止します。
3. **Register at sign-in** を押すと、現在の端子とプリセットを保存して Windows のスタートアップに登録します。次のサインインからトレイに常駐し、保存した端子が使えるようになると中継を再開します。同じ場所の **Unregister at sign-in** を押すと登録を解除します。手動操作用の設定と現在の再生状態は保持されます。

ウィンドウの閉じるボタンはトレイに格納します。完全に終了するには **Exit** を選びます。端子が見つからないときは同じ端子 ID が戻るまで待機します。

## 音が乱れたとき

設定画面またはトレイメニューの **Save diagnostics...** からテキストファイルを保存できます。実行中は非表示でも約1秒ごとに採取し、直近120回の履歴をメモリに保持します。症状が出たらそのまま保存してください。保存先はその場で選べます。レポートには端子 ID、フォーマット、バッファーサイズ、録音・再生別の取得失敗数と HRESULT、キュー量、ドリフト補正、欠落フレーム数、セッション再作成の理由、デバイス変更回数が含まれます。音声サンプルは収録しません。

録音側と再生側で独立に、4回連続、または直近32回中8回のバッファー取得失敗でセッションを作り直します。2秒間処理が進まない場合、選択した端子の状態が変わった場合、WASAPI がセッションを無効化した場合も、同じ入出力を開き直します。過剰なキューを短縮した箇所には2 msの補間を入れます。

## ビルドと検証

CMake 3.26 以上、Visual Studio 2022 の C++ デスクトップ開発環境と Windows SDK を使用します。

```powershell
git clone --recurse-submodules https://github.com/kyori19/audio-repeater.git
cd audio-repeater
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

配布用は別ディレクトリに `-DBUILD_TESTING=OFF` で構成します。`--list-devices` は端子 ID を表示します。`--probe --input-id ID --output-id ID --duration-ms 1000` は有限時間の排他動作試験です。

## 出典

[aitokis/audio-repeater の PR #5](https://github.com/aitokis/audio-repeater/pull/5) のコミット `47ab914a7c0287cd2908a6d4bdeb55d5230735d2` をベースにしています。アプリの MIT ライセンスは [LICENSE](LICENSE)、libsamplerate の BSD ライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) に収録しています。
