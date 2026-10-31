# ISO7816 Pico

Raspberry Pi Pico (RP2040) で ISO 7816 スマートカードを作るための **フレームワーク**。

ビットレベルのプロトコル (スタートビット / パリティ / ガードタイム / ETU 計時) は PIO が担い、
ISO 7816-3 (ATR / PPS / T=0 / T=1) と ISO 7816-4 (APDU / 論理チャネル / チェイニング / ファイルシステム / PIN / 標準コマンド)
はフレームワークが実装している。

**`src/card/` 配下 (カードプロファイルとアプレット) のみの記述で、カードを開発できる。**

```
              ┌──────────────────────────────────────────────────────┐
  開発者が     │ src/card/card_profile.c   ATR・ファイル・PIN・フック  │
  記述する部分 │ src/card/applets/*.c      アプレット (AID + コマンド) │
              └──────────────┬───────────────────────────────────────┘
                             │ card_profile_t / applet_t  (src/card/card.h)
  ┌──────────────────────────▼────────────────────────────────────┐
  │ src/apdu/   ISO 7816-4                                        │
  │   dispatch.c  CLA 解読・論理チャネル・チェイニング・SM フック・  │
  │               AID 選択・ルーティング (アプレット → 標準 → フック) │
  │   fs.c        MF/DF/EF, SELECT, BINARY/RECORD 系コマンド        │
  │   sec.c       PIN, VERIFY/CHANGE/RESET RETRY/GET CHALLENGE 等    │
  │   iso_cmds.c  標準コマンド表 (T=0 の case 判定にも使用)          │
  ├───────────────────────────────────────────────────────────────┤
  │ src/proto/  ISO 7816-3                                        │
  │   session.c   ATR → PPS → プロトコル選択 → コマンドループ        │
  │   atr.c       ATR の生成 / 解析   pps.c  PPS 応答と F/D 切替     │
  │   t0.c        T=0 (手続きバイト, 61xx/6Cxx, GET RESPONSE, ENVELOPE)│
  │   t1.c        T=1 (I/R/S ブロック, チェイニング, IFS, RESYNCH, WTX)│
  ├───────────────────────────────────────────────────────────────┤
  │ src/hal/    ハードウェア                                        │
  │   phy.c + iso7816.pio  PIO 文字層 (任意 F/D, 逆規約, エラー信号) │
  │   nvm.c       フラッシュへの永続化    bootsel_button.c            │
  ├───────────────────────────────────────────────────────────────┤
  │ src/sys/    main.c (メインループ, ウォッチドッグ), console.c      │
  └───────────────────────────────────────────────────────────────┘
```

## 配線

| カード接点 | Pico        | 備考                         |
|-----------|-------------|------------------------------|
| C1 VCC    | VSYS        | ダイオード経由                |
| C2 RST    | **GP2**     | 1 kΩ                          |
| C3 CLK    | **GP1**     | 1 kΩ                          |
| C5 GND    | GND         |                              |
| C7 I/O    | **GP4**     | 1 kΩ、オープンドレイン動作       |

ピン番号は [src/config.h](src/config.h) だけで定義する (PIO の `wait gpio` 命令は起動時に命令メモリを書き換えて CLK ピンに合わせる)。

## 動かす

```powershell
.\tools\flash.ps1            # cmake + ninja → picotool load -f -x  (ボタン操作不要)
.\tools\sc_test.ps1          # PC/SC (winscard.dll) でデモカードを一通り検証
```

Linux / macOS では `tools/flash.sh` と `tools/sc_test.py` (pyscard) が同じ役割を果たす。
Pico なしで動くホスト側テスト (`tests/`) と GitHub Actions の CI もある。

付属のデモカードは SCR3310 + Windows 11 で T=0、T=1、PPS (F/D=372/4) のすべてで `sc_test.ps1` の全項目が通る。
詳細は [docs/tools.md](docs/tools.md) にある。

## カードを開発する

1. [src/card/card_profile.c](src/card/card_profile.c) の ATR を決める (生バイト列か記述子)。
2. 永続化したいデータを 1 つの構造体にまとめ、EF と PIN をその中に置く。
3. `fs_file_t` の表でファイル木を、`sec_pin_t` の表で PIN を定義する。
4. `src/card/applets/` にアプレットを書き、コマンド表 (CLA / INS / T=0 case / ハンドラ) を埋める。
5. 必要なフック (GET DATA、認証、セキュアメッセージング、暗号コマンド) を実装する。

各項目の書き方は [docs/card-profile.md](docs/card-profile.md)、型の定義は [src/card/card.h](src/card/card.h) にある。
フレームワークが扱う機能の一覧、ディスパッチの順序、はまりどころ、既知の制限は [docs/internals.md](docs/internals.md) にある。

## ドキュメント

| ファイル | 内容 |
|---------|------|
| [docs/card-profile.md](docs/card-profile.md) | カードプロファイルとアプレットの書き方、ハンドラから使用できる API |
| [docs/internals.md](docs/internals.md) | 動作原理、セッションとディスパッチの流れ、実装済み機能、はまりどころ、既知の制限 |
| [docs/tools.md](docs/tools.md) | ビルド・書き込み、無操作 BOOTSEL の 5 通り、PC/SC テスト、コンソールコマンド |
