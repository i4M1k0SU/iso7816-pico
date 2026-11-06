# ビルド・書き込み・動作確認

## ビルドと書き込み

VS Code の Raspberry Pi Pico 拡張が入れた SDK / GCC / picotool (`~/.pico-sdk`) を使用する。
Windows は PowerShell 版、Linux / macOS (と Git Bash) はシェル版を使う。

```powershell
.\tools\flash.ps1                  # cmake + ninja → picotool load -f -x
.\tools\flash.ps1 -NoBuild         # 書き込みのみ
.\tools\flash.ps1 -NoFlash         # ビルドのみ
.\tools\flash.ps1 -Board pico2     # Pico 2 (RP2350) 向けに build-pico2/ でビルドして書き込む
```

```sh
tools/flash.sh                     # 同上 (PICO_SDK_ROOT で ~/.pico-sdk 以外も指定できる)
tools/flash.sh --no-build          # 書き込みのみ
tools/flash.sh --no-flash          # ビルドのみ
tools/flash.sh --board pico2       # Pico 2 向け (build-pico2/)
```

どちらもリポジトリ単独のビルド (`examples/demo` → `build/examples/demo/iso7816-pico-demo.uf2`) を書き込む。
`-Board` / `--board` に `pico` 以外を指定すると `build-<ボード名>/` を `-DPICO_BOARD=<ボード名>` で構成して使う
(`build.ninja` がまだない場合だけ構成し、あればそのまま ninja を実行する)。Pico 2 向けはコンパイルのみ確認済みで、
実機は未検証である。利用側プロジェクトからの使い方は [embedding.md](embedding.md)。

SDK、GCC、picotool の版は `CMakeLists.txt` の VS Code 拡張用ブロック (`sdkVersion` / `toolchainVersion` /
`picotoolVersion`) に固定された版を使う。その版が `~/.pico-sdk` に入っていなければ警告を出し、入っている中で最新の版を
使う。CMake と Ninja は入っている中で最新の版を使う。シェル版は `PICO_SDK_PATH` / `PICO_TOOLCHAIN_PATH` が設定済みなら
それを優先する。

Linux では picotool が USB デバイスを開けるように、picotool 付属の udev ルール (`udev/99-picotool.rules`) を入れるか
sudo で実行する。

`picotool load -f` は、動作中のファームウェアの USB リセットインターフェイス経由で BOOTSEL に再起動させてから書き込むので、
ボタン操作は不要である。

## BOOTSEL に入れる方法 (5 通り)

1. `picotool reboot -f -u` または `tools\flash.ps1` / `tools/flash.sh` (USB ベンダーリセットインターフェイス)
2. USB シリアル (COMx) を **1200 bps** で開いて閉じる (スクリプトでは「open failed」と出ることがあるが、その時点で BOOTSEL に入っている)
3. USB シリアルコンソールで `bootsel` を入力する
4. PC/SC から APDU `80 E0 00 00 00` を送る (`tools\sc_test.ps1 -Bootsel` / `tools/sc_test.py --bootsel`)
5. 動作中に基板の BOOTSEL ボタンを約 0.3 秒押す (リセット不要)

さらに 3 秒のウォッチドッグを有効にしているので、ファームウェアが固まっても自動的に再起動する。

以前のファームウェアが USB に応答しない状態だと (1)(2) は効かない。その場合は一度 USB を抜き差し
(BOOTSEL を押しながら挿すと確実) してから `tools\flash.ps1` を実行する。

## PC/SC テスト

Windows 版 (`sc_test.ps1`) は追加ソフト不要で winscard.dll を直接呼ぶ。Python 版 (`sc_test.py`) は pyscard
(`pip install pyscard`) を使い、Linux (pcsc-lite + pcscd)、macOS、Windows で同じ 67 項目を実行する。

```powershell
.\tools\sc_test.ps1                              # デモカードを一通り検証 (プロトコルはリーダー任せ)
.\tools\sc_test.ps1 -Protocol T1                 # T=1 を要求 (ATR が T=1 を出す設定のとき)
.\tools\sc_test.ps1 -Apdu "80 10 00 00 00"       # 任意の APDU を 1 つ送る
.\tools\sc_test.ps1 -Bootsel                     # 最後に BOOTSEL へ再起動
.\tools\sc_test.ps1 -Reader "ACS"                # リーダー名の部分一致 (既定 SCR33)
```

```sh
tools/sc_test.py                                 # 同上
tools/sc_test.py --protocol T1
tools/sc_test.py --apdu "80 10 00 00 00"
tools/sc_test.py --bootsel
tools/sc_test.py --reader "ACS"
```

実行例 (SCR3310、Windows 11):

```
ATR    : 3B 08 50 69 63 6F 37 38 31 36   (T=0)
[PASS] SELECT by AID                  00 A4 04 00 05 F0 50 49 43 4F  -> SW 9000 6F 07 84 05 F0 50 49 43 4F
[PASS] GET VERSION                    80 10 00 00 00                 -> SW 9000 "Pico7816 demo 1.0"
[PASS] ECHO 200 bytes                 80 20 00 00 C8 01 02 ...       -> SW 9000 01 02 ...
...
all checks passed
```

付属のデモカード (`examples/demo/card_profile.c`) で確認済みの内容:

* T=0 のみの ATR、T=1 のみの ATR、T=0/T=1 両対応 + TA1=13 の ATR の 3 種類。
  最後のものは Windows が PPS で **T=1, F/D=372/4 (ETU 19 µs)** に切り替え、その速度で全テストが通る。
  特定モードの生 ATR `3B 90 13 10 00` (TA2、PPS なしで F/D=372/4) でも全テストが通る。
* `sc_test.ps1` の 67 項目 (プロトコルごと): アプレットコマンド、2 秒かかるコマンドの keep-alive (NULL バイト / S(WTX))、
  透過 EF の READ/UPDATE/チェイニング、PIN の VERIFY/CHANGE/RESET RETRY (PUK)、SELECT (FID / 子 DF / 親 / パス / DF 名) と FCP、
  固定長・可変長・サイクリックレコード、SEARCH RECORD、GET DATA、GET CHALLENGE + EXTERNAL/INTERNAL AUTHENTICATE フック、
  論理チャネル (MANAGE CHANNEL とチャネルごとのセキュリティ状態)、拡張長 APDU (T=0: ENVELOPE 300 バイト、T=1: 1000 バイトの Lc/Le)。
* NVM: セッション中に書き換えた EF はアイドル時に、PIN カウンタは照合のたびにフラッシュへ保存され、電源再投入後も残る。

## ホスト側テスト

`tests/` には Pico を使わずに PC 上で動くテストがある。`proto/` と `apdu/` の各層とデモカードをそのままホスト用にコンパイルし、
PHY を偽物 (`tests/fake_phy.c`) に差し替えて、リーダー側の T=0 / T=1 モデル (`tests/reader_t0.c` / `reader_t1.c`) から駆動する。
`sc_test` と同じ 67 項目を両プロトコルで実行するほか、実機では起こせない異常系 (T=1 の EDC 誤り・再送・RESYNCH・ABORT・
ブロック途切れ・バッファ超過、T=0 の 6C 再発行・ENVELOPE の終端、PPS の拒否、チェイニングの不整合、チャネルの枯渇) を確認する。

```sh
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure     # または build-tests/iso7816_tests -v (カードのログ付き)
```

GitHub Actions (`.github/workflows/ci.yml`) は push ごとにこのテストと、Pico SDK を取得しての実機向けビルド (Pico と Pico 2、
UF2 を成果物として保存)、そして `tests/embed/` (submodule 利用側を模した最小プロジェクト) のビルドを実行する。
使用する action はコミット SHA で固定している。実機向けビルドのツールは VS Code 拡張と同じ版 (Arm GNU Toolchain 15.2.Rel1、
ビルド済みの picotool / pioasm 2.3.1) を SHA-256 を確かめて取得し、Pico SDK (サブモジュールは `lib/tinyusb` のみ) と
合わせて Actions のキャッシュに置く。キャッシュが当たれば apt もダウンロードも picotool / pioasm のビルドも行わない。

### サニタイザーと静的解析

CI はホスト側テストを通常のビルドに加えて AddressSanitizer + UndefinedBehaviorSanitizer 付きでも実行する
(`host-tests-sanitize` ジョブ)。`-DISO7816_TESTS_SANITIZE=ON` で有効になり、`-fno-sanitize-recover=all` によって
未定義動作も 1 件でテストを失敗させる。

```sh
cmake -S tests -B build-san -DISO7816_TESTS_SANITIZE=ON
cmake --build build-san
UBSAN_OPTIONS=print_stacktrace=1 ctest --test-dir build-san --output-on-failure
```

`clang-tidy` ジョブは、ホスト側テストでコンパイルされるすべての C ファイル (`src/core`、デモ、`tests/`、およびそれらが
読み込む `include/` のヘッダ) をリポジトリの `.clang-tidy` で検査し、警告を 1 件でも出せば失敗する。clang-tidy の版は
23.1.4 に固定している。有効にしたチェックと、外したチェックとその理由は `.clang-tidy` に書いてある。解析器の誤検出は
該当行で `// NOLINTNEXTLINE(<チェック名>)` と理由のコメントを付けて抑止する。

```sh
pip install clang-tidy==23.1.4
cmake -S tests -B build-tests -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
jq -r '.[].file' build-tests/compile_commands.json | xargs clang-tidy -p build-tests --quiet
```

## コンソール (USB シリアル、115200 bps 任意)

ATR や APDU のトレースが流れる。ログは専用リングバッファに溜めて空き時間にだけ送るので、
ホストがシリアルを読んでいなくてもカード応答は遅れない (接続していない間の履歴も残る)。
入出力は USB CDC だけで行い、アプリケーションが UART stdio を有効にしていてもそちらには流さない。

接続するとまず次のバナーとコマンド一覧が出る (デモを単体でビルドした場合)。

```
=== Pico7816 demo 1.0 (e6eabb5) ===
iso7816-pico 1.0
commands (type a name and Enter; Enter alone shows this list):
built-in:
  help     list the commands
  ...
```

1 行目はカード名と版、括弧内はファームウェアをビルドしたコミット (未コミットの変更があれば `-dirty`、git がなければ
`unknown`)、2 行目はフレームワークの版である。フレームワークを submodule として組み込んだプロジェクトでは
2 行目にもフレームワーク自身のコミットが付く ([embedding.md](embedding.md) の「版とコミットの表示」)。
`help` と `card` も同じ 2 行から始まる。

コマンドは 1 行ずつ入力し、Enter で実行する (入力はエコーされ、BS で消せる)。何も入力せずに Enter を押すと
`help` と同じ一覧が出る。一覧は組み込みコマンド (`built-in:`) とカードプロファイルのコマンド (カード名の見出し) に
分かれる。

| コマンド | 動作 |
|---------|------|
| `help`    | コマンド一覧 (空行の Enter でも同じ) |
| `status`  | 統計 (CLK 周波数、F/D、ETU、プロトコル、セッション数、エラー、直近の ATR の診断)。エラーの `rx overrun` は受信 FIFO があふれて読み落とした回数、`repetition` はリーダーのエラー信号で送り直した文字数、`rx latency` は受信文字を取り出すまでの遅れの最大値とエラー信号に間に合わなかった文字数 ([internals.md](internals.md) の「T=0 のエラー信号と文字の繰り返し」) |
| `card`    | カードプロファイル (ATR、アプレット、ファイル木、PIN、NVM サイズ) |
| `trace`   | 直近のセッションの最初の 128 件の TX/RX トレース (時刻付き。`T` 送信、`P` パリティを誤らせた送信、`R` 受信語、`E` / `e` リーダーのエラー信号の開始 / 終了) |
| `txperr <index> [count]` | 診断用。セッションの index 番目の送信文字 (TS = 0) を count 回 (既定 1) パリティを誤らせて送る。まだその文字を送っていなければ今のセッション、送り終えていれば次のセッションで効く。count 0 で取り消す |
| `gpio`    | 全 GPIO を 15 秒監視し、動いたピンと立ち上がり/立ち下がりの順序を表示する (配線確認用)。セッション中は拒否する |
| `pins`    | RST/CLK/I/O の状態を 5 秒間表示する。セッション中は拒否する |
| `apdu <hex>` | リーダーなしでコマンド APDU をディスパッチャに流し、応答データと SW を表示する (例: `apdu 00 A4 04 00 05 F0 50 49 43 4F`)。セッション中は拒否する。セキュリティ状態は次のリーダーセッションまで保持される |
| `save`    | NVM を今すぐ保存する。セッション中は拒否する (セッション中の変更はアイドル時保存とリセット時に書かれる) |
| `erase`   | NVM を消去して再起動する (工場出荷状態)。セッション中は拒否する |
| `bootsel` | BOOTSEL で再起動する |
| `reboot`  | 通常再起動 |
| `dump`, `unblock` | デモカードが追加したコマンド (NVM の表示、PIN リトライ回数のリセット)。追加の仕方は [card-profile.md](card-profile.md) の「コンソールコマンド」 |

「セッション中」は ATR を送ってから次のリセットまで、つまり PC/SC の接続が開いている間である。コマンドはコマンド待ちの
アイドル処理から実行されるので、`gpio` / `pins` (数秒間ループする) や `save` / `erase` (割り込みを止めてフラッシュを書く)
をセッション中に走らせると、その間にリーダーが送った文字が 8 段の受信 FIFO からあふれるか、リーダーがタイムアウトする。
そのためセッション中は `refused: a card session is active` を返して実行しない。
`bootsel` と `reboot` はセッション中でも実行する (セッションを捨てるのが目的なので)。

## 配線の確認

コンソールの `gpio` で全 GPIO を監視しながらリーダーに活性化させると、ISO 7816-3 の順序 (I/O 解放 → CLK 開始 →
40000 クロック後に RST High、非活性化では RST が先に Low) から各接点のピンを特定できる。配線を変えたときはこれで確認する。
`gpio` はセッション中は拒否されるので、リーダーの接続を開く前に入力し、15 秒以内に活性化させる (監視中のカードは
ATR を返さない)。

## コードの整形

C のソースとヘッダはリポジトリの `.clang-format` (clang-format 18.1.8) に従う。CI の `clang-format` ジョブが
`clang-format --dry-run -Werror` で全ファイルを検査するので、コミット前に整形しておく。

```sh
pip install clang-format==18.1.8
git ls-files '*.c' '*.h' | xargs clang-format -i --style=file
```

手で揃えた表 (デモのファイル表など) は `// clang-format off` と `// clang-format on` で囲んで除外できる。

