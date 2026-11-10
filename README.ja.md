# phonon

[English](README.md) | 日本語

Gradium のオンデバイス音声合成モデル [Phonon](https://gradium.ai/phonon) を使って、4D でオフラインのテキスト読み上げを行うプラグインです。モデルを1つまたは複数読み込み、テキストをバックグラウンドで読み上げます。読み上げはそれぞれ専用のスレッドで実行され、再生中に一時停止・再開・停止し、進行状況を取得できます。音声を WAV の `Blob` として受け取ったり、ファイルに保存したりすることもできます。Phonon エンジン（[`ptts`](https://crates.io/crates/ptts) 0.7.0）はプラグインに組み込まれており、ネットワークには何も送信しません。

## 動作環境

- 4D v21.1 以降（`.4DProject` の `compatibilityVersion` と互換）
- macOS 11 以降（Apple シリコンまたは Intel）、または AVX2 対応プロセッサ（2013年以降）の Windows x64
- Phonon のモデルフォルダー。例: [Phonon 31m](https://huggingface.co/gradium/phonon-31m-en)（英語、Apache 2.0、約 65 MB）。[モデル](#モデル)を参照

## インストール

[Releases](../../releases) ページから最新リリースをダウンロードします。

### macOS と Windows（共通ダウンロード）

1. リリースから `.zip` をダウンロードします
2. 展開して `phonon.bundle` フォルダーを取り出します
3. `.bundle` を 4D アプリケーションの **Plugins** フォルダー（またはデータベースの **Plugins** フォルダー）にコピーします
4. 4D を再起動します

### macOS のみ（公証済み DMG）

1. リリースから `.dmg` をダウンロードします
2. マウントして `.bundle` を **Plugins** フォルダーにコピーします
3. 4D を再起動します

## モデル

プラグインは、モデルの Hugging Face リポジトリと同じ構成のフォルダーを読み込みます。同じフォルダーを macOS と Windows の両方で使えます。Phonon 31m の場合は、次の4ファイルを1つのフォルダーにダウンロードします:

```bash
hf download gradium/phonon-31m-en config.json tokenizer.json model.q8.gguf default-voice.safetensors --local-dir phonon-31m-en
```

Phonon 40m モデル（英語、フランス語、ドイツ語、スペイン語、ポルトガル語、多言語）は CC BY-NC 4.0 で、利用には Hugging Face で利用条件への同意が必要です。その他の声は[ボイスカタログ](https://huggingface.co/gradium/phonon-voices)にあります。モデルカードは、実在の人物へのなりすましや、本人の同意のない声のクローンを禁じています。

## コマンド

すべてのオプションとステータスのプロパティを含む完全なリファレンスは [docs/phonon-reference.md](docs/phonon-reference.md)（英語）にあります。

| コマンド | 戻り値 | 用途 |
|---|---|---|
| `Phonon Load model(path; options)` | Longint | モデルフォルダーを読み込む。失敗時は `0` |
| `Phonon Load models(list)` | Collection | 複数のモデルフォルダーを並列に読み込む |
| `Phonon UNLOAD MODEL(model)` | — | モデルを解放する（使用中の読み上げは最後まで再生される） |
| `Phonon UNLOAD ALL` | — | すべてのモデルを解放する |
| `Phonon Get models` | Collection | 読み込み済みのすべてのモデルの情報 |
| `Phonon Get model info(model)` | Object | モデルの声、言語、サンプルレート |
| `Phonon ADD VOICE(model; name; path)` | — | モデルに声のファイルを追加する |
| `Phonon Play(model; text; options)` | Longint | バックグラウンドで読み上げ、読み上げ参照を返す |
| `Phonon Play multiple(list; options)` | Collection | 複数のテキストを同時に、または順番に読み上げる |
| `Phonon PAUSE(speech)` | — | 読み上げを一時停止する |
| `Phonon RESUME(speech)` | — | 読み上げを再開する |
| `Phonon STOP(speech)` | — | 読み上げを停止して解放する |
| `Phonon PAUSE ALL` | — | すべての読み上げを一時停止する |
| `Phonon RESUME ALL` | — | すべての読み上げを再開する |
| `Phonon STOP ALL` | — | すべての読み上げを停止して解放する |
| `Phonon SET VOLUME(speech; volume)` | — | 再生中に音量を変える |
| `Phonon Get status(speech)` | Object | 読み上げの状態、再生位置、長さ、エラー |
| `Phonon Get statuses(speeches)` | Collection | 複数またはすべての読み上げの状態 |
| `Phonon Synthesize(model; text; options)` | Blob | 音声を WAV データで返す（16ビット、モノラル、24 kHz） |
| `Phonon Get last error` | Text | このプロセスで最後に失敗したコマンドの理由 |
| `Phonon SET THREADS(count)` | — | CPU スレッド数。最初のモデルを読み込む前に設定する |
| `Phonon Get info` | Object | プラグインとエンジンのバージョン、件数 |

すべてのコマンドはスレッドセーフです。4D のエラーを発生させるコマンドはありません。失敗すると `0` または空の値を返して `Phonon Get last error` にメッセージを残し、再生中の失敗は読み上げを `error` 状態にします。

4D の終了時やプロジェクトを閉じるときに、プラグインはすべての読み上げを停止し、そのスレッドの終了を待ち、すべてのモデルを解放します。

**例:**

```4d
var $folder : 4D.Folder
$folder:=Folder(Get 4D folder(Current resources folder)+"phonon-31m-en"; fk platform path)

var $model : Integer
$model:=Phonon Load model($folder.path; {language: "en"})
If ($model=0)
	ALERT(Phonon Get last error)
	return 
End if 

var $speech : Integer
$speech:=Phonon Play($model; "Your report is ready."; {voice: "default"})

var $status : Object
Repeat 
	DELAY PROCESS(Current process; 10)
	$status:=Phonon Get status($speech)
Until ($status.state=Phonon state finished) || ($status.state=Phonon state error)
Phonon STOP($speech)

var $wav : Blob
$wav:=Phonon Synthesize($model; "Saved for later."; {})
```

## 定数

| 定数 | 型 | 値 | 説明 |
|---|---|---|---|
| `Phonon state pending` | Text | `pending` | 再生前: モデル、前の読み上げ、最初の音声のいずれかを待っている |
| `Phonon state playing` | Text | `playing` | 再生中 |
| `Phonon state paused` | Text | `paused` | 一時停止中 |
| `Phonon state finished` | Text | `finished` | 最後まで再生した |
| `Phonon state error` | Text | `error` | 失敗した。理由はステータスの `error` |
| `Phonon state unknown` | Text | `unknown` | その参照の読み上げはない（解放済み、または存在しない） |
| `Phonon output default` | Text | `default` | 既定の出力デバイスで再生する |
| `Phonon output none` | Text | `none` | デバイスを使わず、実時間で無音再生する |

## ソースからのビルド

### 前提条件

- CMake 3.20 以降
- Xcode（macOS）または Visual Studio 2022（Windows）
- [Rust](https://rustup.rs)（stable）。CMake が `cargo` を実行して [`phonon-core`](phonon-core) からエンジンをビルドし、必要な Rust ターゲットを `rustup` で追加します

### クローン

```bash
git clone --recurse-submodules https://github.com/miyako/4d-plugin-phonon.git
cd 4d-plugin-phonon
```

### ビルド（macOS）

```bash
cd phonon
mkdir -p cmake-build && cd cmake-build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build .
```

### ビルド（Windows）

```pwsh
cd phonon
mkdir cmake-build; cd cmake-build
cmake .. -A x64
cmake --build . --config Release
```

### テストの実行

[tool4d](https://developer.4d.com/docs/Admin/cli/)（無料、ライセンス不要）が必要です。初回の実行で Phonon 31m（約 65 MB）を `phonon-test/models` にダウンロードします:

```bash
/path/to/tool4d --dataless --startup-method=test_all --project=$(pwd)/phonon-test/Project/phonon.4DProject
```

## CI/CD

このプロジェクトは、生成された標準の3ワークフロー構成のネイティブプラグインパイプラインを使います:

| ワークフロー | トリガー | 用途 |
|---|---|---|
| `test.yml` | プッシュ / PR / 手動 / 再利用 | 同じリビジョンの成果物をビルド、テスト、検証する |
| `bump-version.yml` | 手動、デフォルトブランチのみ | `VERSION` を上げてコミットする。タグは作らない |
| `release.yml` | 手動、デフォルトブランチのみ、入力なし | テストを再利用し、署名、公証、検証、ドラフト作成を経て `vVERSION` を公開する |

### 必要なシークレット（`release.yml` のみ）

**Settings → Secrets and variables → Actions** で設定します:

| シークレット | 説明 |
|---|---|
| `APPLE_DEVELOPER_ID_CERTIFICATE` | 秘密鍵を含む Developer ID Application `.p12` を Base64 エンコードしたもの |
| `APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD` | `.p12` 書き出し時のパスワード |
| `KEYCHAIN_PASSWORD` | CI ランナーの一時キーチェーン用の独立したパスワード |
| `NOTARYTOOL_APPLE_ID` | 公証に使う Apple ID のメールアドレス |
| `NOTARYTOOL_TEAM_ID` | Apple Developer のチーム ID（英数字10文字） |
| `NOTARYTOOL_PASSWORD` | Apple のアプリ用パスワード（アカウントのパスワードではない） |

## ライセンス

[MIT](LICENSE)。プラグインは Phonon エンジン（MIT OR Apache-2.0）を含む Rust クレートを静的にリンクしています。それらのライセンスは [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES/phonon-core.txt) にあります。`phonon-core/Cargo.lock` を変更したら、`python3 phonon-core/third_party_notices.py` でこのファイルを再生成してください。Phonon のモデルにはそれぞれ独自のライセンスがあります（[モデル](#モデル)を参照）。
