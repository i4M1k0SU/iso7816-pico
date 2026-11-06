# 内部構造と実装メモ

## 動作原理

```
         reader CLK (C3) ──┐
                           ▼
  PIO SM0 (rx) : wait 1 pin → wait 0 pin (start) → CLK エッジを数えて 0.5 ETU → 9 bit を bit 中央でサンプル → FIFO
  PIO SM1 (tx) : FIFO から 1 bit ずつ out pindirs (1 = Low 駆動, 0 = 解放) → CLK を F/D 回数えて 1 ETU
```

* ETU はリーダーのクロックをそのまま数えて生成する (`wait 0 gpio` / `wait 1 gpio` ループ)。
  クロック周波数の測定や sysclk からの分周は不要で、リーダーが何 MHz を出しても追従する。
  受信側の半 ETU 待ちは CLK の **エッジ** を数えるため、F/D=372/4 の 93 クロックのような奇数 ETU でもサンプル位置が正確である。
* I/O は `out pindirs` でオープンドレインを実現する。送信中は自分の文字を RX が読み戻すので、
  それを送信完了の検出と衝突検知に使用する。
* リーダーからの文字にパリティ誤りがあると、10.5 ETU から 2 ETU の間 I/O を Low にするエラー信号を返し、
  リーダーに再送させる (T=0 の文字再送。T=1 では無効)。
* RST の立ち下がりは GPIO 割り込みでラッチし、短いリセットパルスも取りこぼさない。
  この割り込みは `gpio_add_raw_irq_handler()` で登録した共有の raw ハンドラであり、RST のイベントだけを見て確認応答する。
  SDK の唯一の GPIO コールバック (`gpio_set_irq_callback()`) は占有しないので、アプレットやフックが他の GPIO の割り込みを併用できる。
  RST が High になると TX の OSR に 1 フレーム分の「解放」を仕込んでから起動するので、ちょうど 12 ETU 後に ATR が始まる
  (規格の 400〜40000 クロックの範囲内)。CLK 周波数は pio1 の `clkcount` プログラムで
  500 us 間の立ち上がりエッジを数えて実測する (`wait pin` はパッド入力を読むので、CLK はどの GPIO でもよく、ピン機能は pio0 のまま)。
* リーダーから受けた文字の後、応答は先頭エッジ間隔が 16 ETU (T=1 は BGT 22 ETU) になるまで待ってから送る。
  これを守らないとリーダーが応答を取りこぼす。
* 起動時にループバック自己診断を行う: 空きピン GP16 にシステムクロックの 1/32 (125 MHz なら 3.9 MHz) の試験クロックを
  PIO で出し、GP17 を I/O として ATR の送受信と ETU の実測、さらに F/D=372/4 + 逆規約での送受信を行い、結果を
  コンソールに表示する。
* PIO はシステムクロックで動き、リーダーの CLK を `wait gpio` で数える (1 周期あたり最大 4 命令、ビット境界でさらに
  5 命令)。ETU を正確に数えるにはシステムクロックが CLK の 8 倍以上 (`PHY_MIN_SYSCLK_PER_CLK`) 必要で、下回ると
  セッション開始時に警告を出す。125 MHz なら CLK 15 MHz 程度、48 MHz なら 6 MHz 程度のリーダーまで追従できる。

## セッションの流れ

1. RST High → `phy_start_session()` (既定 F/D、規約は ATR から) → ATR 送出。
2. 最初の受信バイトが FF なら PPS。応答後に `phy_apply_params()` で F/D とプロトコルを切り替える。
   ATR が特定モード (TA2 あり) なら PPS には応じず、ATR 送出の直後に `phy_apply_params()` で TA1 の F/D へ切り替える。
3. `session.c` が T=0 (`t0.c`) または T=1 (`t1.c`) のコマンドループを回す。
4. 両トランスポートは完全な APDU を組み立てて `dispatch_process()` に渡し、SW と応答データを受け取る。
5. RST Low で `phy_enter_reset()`、NVM が dirty なら保存 (内容が変わっていなければ書かない。スロット方式の詳細は
   [card-profile.md](card-profile.md) の「永続データ」)。

### T=0 の ENVELOPE の終端

T=0 では拡張長 APDU を ENVELOPE (INS C2) に分けて送る。ISO 7816-4 は ENVELOPE の連鎖がどこで終わるかを定めていないため、
`t0.c` (`handle_envelope()`) は次の規約を採る。

* Lc (P3) = 255 の ENVELOPE は「続きがある」を意味し、データを蓄積して 9000 を返す。
* Lc が 255 未満の ENVELOPE (Lc = 0、つまり P3 = 00 を含む) が最後の片である。蓄積したバイト列全体を 1 つの APDU として
  解読して実行し、解読できなければ 6A80、蓄積が `APDU_BUF_SIZE` を超えれば 6700 を返して連鎖を破棄する。
* したがって APDU の全長が 255 の倍数になるホストは、最後にデータなしの ENVELOPE (`xx C2 00 00 00`) を送らなければならない。
* ENVELOPE の TPDU 自体はケース 3 であり、リーダーはデータを送り終えて SW1 SW2 だけを待っている。そのため内側の APDU が
  応答データを返す場合は、内側がケース 2 (Lc なし) でもケース 4 でも、また Le の値にかかわらず、必ず 61xx で知らせて
  GET RESPONSE で取り出させる。ENVELOPE に 2 度目の ACK とデータを返したり 6Cxx を返したりはしない。応答データがなければ
  SW だけを返す。

## ディスパッチの順序

1. CLA 解読 (チャネル、チェイニング、SM)。閉じたチャネルは 6881。
2. SM bit があれば `hooks.sm_unwrap`。
3. チェイニング中なら蓄積して 9000 を返し、最後の片で全体を処理する。
4. SELECT by DF name → アプレット (前方一致)、なければファイルシステムの DF。
5. MANAGE CHANNEL。
6. チャネルのアプレット: コマンド表のハンドラ → `process`。
7. `iso_commands` が真かつ CLA が interindustry なら標準コマンド表。
8. `hooks.unhandled`。
9. 6E00 (INS は他の CLA で既知) または 6D00。
10. SM bit があれば `hooks.sm_wrap`。

## 実装済みの機能

| 層 | 機能 | 状態 |
|----|------|------|
| 7816-3 | ATR 生成/解析, 直接・逆規約, 任意 F/D (PPS), 追加ガードタイム, パリティエラー信号 (T=0 文字再送) | 実装 (逆規約と F/D=372/4 はループバック自己診断で検証、PPS は SCR3310 で検証) |
| 7816-3 | T=0: 手続きバイト, 61xx / 6Cxx, GET RESPONSE, ENVELOPE (拡張長), NULL バイト | 実機検証済み |
| 7816-3 | T=1: I/R/S ブロック, 両方向チェイニング, IFS, RESYNCH, ABORT, WTX, LRC / CRC | 実機検証済み (CRC は未検証) |
| 7816-4 | 短縮長・拡張長 APDU, コマンドチェイニング (CLA b5), 論理チャネル (MANAGE CHANNEL), SM フック | 実機検証済み (SM はフックのみ) |
| 7816-4 | SELECT (A4: P1=00/01/02/03/04/08/09, P2 の FCI/FCP/FMD/なし) | 実機検証済み |
| 7816-4 | READ/WRITE/UPDATE/ERASE BINARY (B0/D0/D6/0E, SFI 対応) | 実機検証済み (ERASE は未検証) |
| 7816-4 | READ/WRITE/UPDATE/APPEND/ERASE/SEARCH RECORD (B2/D2/DC/E2/0C/A2), 固定長・可変長・サイクリック | 実機検証済み (ERASE は未検証) |
| 7816-4 | GET DATA / PUT DATA (CA/DA) → フック | 実機検証済み |
| 7816-4 | VERIFY / CHANGE REFERENCE DATA / RESET RETRY COUNTER / ENABLE・DISABLE VERIFICATION (20/24/2C/28/26) | 実機検証済み (28/26 は未検証) |
| 7816-4 | GET CHALLENGE (84), EXTERNAL/INTERNAL AUTHENTICATE (82/88) → フック | 実機検証済み |
| 7816-8/9 | MSE / PSO / GENERATE KEY PAIR / CREATE・DELETE FILE (22/2A/46/E0/E4) → フック | 未実装時は 6A81 を返す |
| カード | フラッシュ永続化, ウォッチドッグ, 5 通りの無操作 BOOTSEL | 実機検証済み |

## はまりどころ

* RP2040 / RP2350 の PIO 命令メモリは **書き込み専用** (読むと 0)。`wait gpio` のピン番号を差し替えるときは
  プログラム配列の元の命令を加工して書き込む必要がある。
* autopull 有効時に `out null, 32` を `pio_sm_exec` で強制実行して OSR を空にすると、その命令が autopull 待ちで止まったままになり、
  次に FIFO へ入れた 1 ワードを飲み込む。代わりに `mov osr, null` で 0 を 1 フレーム分詰めておき、
  それを「ATR 前の 12 ETU 待ち」として使用する。
* `FDEBUG.TXSTALL` は送信完了の検出には使用できない。送信完了は自分の文字のエコーで判定する。
* RX (23 命令) と TX (5 命令) で pio0 がほぼ埋まるため、CLK 計測用のカウンタ (`clkcount`, 常駐) と
  自己診断用のクロック生成 (`clkgen`, 自己診断中のみ) は pio1 に置く。
* Windows は ATR を返さなかったカードを「無応答 (mute)」として記憶し、以後の `SCardConnect` はリーダーに触れずに即座に失敗する
  (カードを抜き差しするまで)。`SCARD_SHARE_DIRECT` で接続してから `SCardReconnect(SCARD_RESET_CARD)` を呼ぶと再活性化する
  (`tools/sc_test.ps1` に組み込み済み)。
* CLA 8X..FX (proprietary class) のチェイニング / チャネル bit は既定では解釈しない。解釈すると CLA 90 が「80 のチェイニング」になり、
  独自 CLA が潰れる。

## 既知の制限

* クロック停止 (clock stop)、T=1 の CRC、ERASE BINARY/RECORD、ENABLE/DISABLE VERIFICATION、
  受信パリティ誤り時のエラー信号は実装済みだが SCR3310 では発生させられず未検証である。
* セキュアメッセージング、ISO 7816-8 の暗号コマンド、CREATE/DELETE FILE はフックのみ (本体はユーザー実装)。
* ファイルの木は静的 (ソース上の表)。実行時にファイルを作るには `create_file` フックと自前の領域管理が必要である。
* 論理チャネルは基本符号化の 0..3 (最大 4 チャネル) に対応する。拡張符号化 (CLA 4X..7X) は解読のみ。
