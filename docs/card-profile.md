# カードを開発する

編集するのは [src/card/card_profile.c](../src/card/card_profile.c) と `src/card/applets/` だけである。
型と各フィールドの説明は [src/card/card.h](../src/card/card.h) にある。

## 1. ATR (ISO 7816-3)

実カードから取った生バイト列をそのまま使用する (`.atr` / `.atr_len`) か、記述子から生成する (`.atr_desc`)。
どちらの場合もフレームワークは ATR を解析し、規約 (3B/3F)、F/D (TA1)、追加ガードタイム (TC1)、特定モード (TA2)、
提供プロトコル (TDi)、T=1 の IFSC/CWI/BWI/EDC (TA3/TB3/TC3) を読み取る。

```c
static const atr_desc_t s_atr = {
    .levels = 3,
    .level = {
        { .present = ATR_TA, .ta = 0x13 },                   // TA1: F=372, D=4 を提案
        { .present = 0, .protocol = 0 },                     // TD1 → T=0 を最初に提供
        { .present = ATR_TA | ATR_TB, .ta = 0xFE, .tb = 0x45, .protocol = 1 }, // TD2 → T=1, IFSC=254
    },
    .historical = { 'P', 'i', 'c', 'o', '7', '8', '1', '6' }, .historical_len = 8,
};
```

* `allow_pps` が真なら PPS に応答し、PIO が作れる F/D (F/D が整数で 8 クロック以上) を受け入れる。
  `pps_accept` で細かく制限できる。TA2 (特定モード) を含む ATR では PPS に応じない。
* TA2 を含む ATR (特定モード) では、ATR は既定値 (F=372, D=1) で送り、送信直後に PPS なしで TA1 の F/D
  (TA2 のビット 5 が立っていれば既定値のまま) へ切り替え、TA2 の下位 4 ビットが示すプロトコルで動作する。
  このとき PPS 要求 (`FF ...`) は通常のコマンドとして扱われる (T=0 では CLA FF なので 6E00)。
* PIO はクロックのエッジを数えて ETU を作るので、F/D=372/4 のような奇数 ETU (93 クロック) も正確である。
* `card_profile.c` には T=0 のみ、T=1 のみ、T=0/T=1 両対応 + TA1=13 の 3 種類の記述子があり、
  いずれも SCR3310 で動作を確認している。

## 2. 永続データ

`nvm_data_t` のような構造体を 1 つ用意して `.nvm_blob` / `.nvm_size` に登録する。
起動時にフラッシュから読み込まれ、セッション中に `nvm_mark_dirty()` (UPDATE BINARY や PIN カウンタの変更で自動) されていれば
RST が Low になった後に書き戻される。フラッシュ書き込みでコマンド応答が遅れることはない。
EF の本体や `sec_pin_t` の配列はこの構造体の中に置く。構造体のサイズが変わると保存済みイメージは捨てられ、
ソース上の初期値に戻る (コンソール `x` でも初期化できる)。

書き戻しは次の規則で行う (`src/hal/nvm.c`)。

* フラッシュ上のイメージと同じ内容なら書かない (VERIFY 成功のようにカウンタを減らして戻しただけの変更は無視される)。
* 領域 (`NVM_FLASH_BYTES`、既定 64 KB) をスロット (ヘッダ + 構造体を 4 KB 単位に切り上げ) に分け、通し番号を付けて順繰りに書く。
  消去するのは次のスロットだけなので、各セクタの消去回数はスロット数分の 1 になる (構造体が 4 KB 未満なら 16 分の 1)。
* 起動時は通し番号が最大で CRC の正しいスロットを採用する。書き込み中に電源が落ちても直前のイメージが残る。
* PC/SC のホストが切断時にカードをリセットしない場合 (Windows の `SCARD_LEAVE_CARD`) は RST が Low にならず書き戻しが起きない。
  `tools/sc_test.*` は `SCARD_RESET_CARD` で切断する。

## 3. ファイルシステム

`fs_file_t` の配列で木構造を定義する (`[0]` が MF、`parent` ポインタで親を指定)。

| フィールド | 意味 |
|-----------|------|
| `fid`, `type` | ファイル ID と種別 (`FS_DF`, `FS_EF_TRANSPARENT`, `FS_EF_LINEAR_FIXED`, `FS_EF_LINEAR_VARIABLE`, `FS_EF_CYCLIC`) |
| `name`, `name_len` | DF 名 (AID)。アプレットの AID と同じ名前の DF は、そのアプレット選択時にカレント DF になる |
| `data`, `size` | EF 本体。レコード EF は先頭 4 バイトがヘッダ (`FS_REC_HDR(使用数, 先頭スロット)`)、`FS_LINEAR_FIXED_BYTES()` 等でサイズを計算する |
| `rec_size` | レコード長 (可変長なら最大長) |
| `sfi` | ショート EF 識別子 (1..30)。READ BINARY の P1 b8 / READ RECORD の P2 で参照する |
| `ac_read`, `ac_update` | アクセス条件: `FS_AC_ALWAYS`, `FS_AC_NEVER`, または必要な PIN の参照番号 (VERIFY の P2) |

アクセス条件は論理チャネルごとのセキュリティ状態で評価される。より複雑な規則は `hooks.check_access` で置き換える。

## 4. PIN

`sec_pin_t` の配列 (`.pins` / `.n_pins`)。参照番号 (`ref`)、値、許容長、リトライ回数、PUK を持ち、
VERIFY (20) / CHANGE REFERENCE DATA (24) / RESET RETRY COUNTER (2C) / ENABLE・DISABLE VERIFICATION (28/26)
をそのまま使用できる。ハンドラからは `sec_is_verified(ch, ref)` / `sec_set_verified()` で参照・設定する
(EXTERNAL AUTHENTICATE 成功時に PIN 相当の権限を与える、など)。

PIN と PUK は既定では平文のまま NVM に置かれる。平文を残したくない場合は `hooks.pin_transform` を実装する。
格納時 (CHANGE REFERENCE DATA、RESET RETRY COUNTER) と照合時 (VERIFY、PUK) の両方で値がこの関数を通るので、
ソルト付きハッシュなどを返せばフラッシュには変換後の値しか残らない。その場合、プロファイルに書く初期値は変換後の形で、
`sec_pin_t.plain_len` に PIN と PUK の平文長 (固定) を入れる (旧 PIN + 新 PIN のように 1 つのデータ欄に 2 つの値が入るコマンドを
そこで分割する)。

## 5. アプレット

`applet_t` を 1 つ作って `.applets` に並べる。SELECT by DF name (00 A4 04 ..) で AID が (前方一致で) 合うアプレットが選ばれ、
以後そのチャネルのコマンドは最初にそのアプレットへ渡される。

```c
static const apdu_cmd_desc_t s_cmds[] = {
    //       CLA   mask  INS   T=0 case     名前            ハンドラ
    APDU_CMD(0x80, 0xF0, 0x10, APDU_CASE2, "GET VERSION",  h_get_version),
    APDU_CMD(0x80, 0xF0, 0x20, APDU_CASE4, "ECHO",         h_echo),
};
const applet_t my_applet = {
    .name = "demo", .aid = s_aid, .aid_len = sizeof s_aid,
    .cmds = s_cmds, .n_cmds = 2,
    .select = NULL,      // NULL なら 6F len { 84 len AID } を返す
    .process = NULL,     // 表にないコマンドの受け皿 (switch で書く場合)。APDU_UNHANDLED を返すと次の層へ
    .reset = NULL,       // ATR ごとの初期化
};
```

* ハンドラは `uint16_t handler(const apdu_t *cmd, apdu_resp_t *resp)`。`cmd->data/lc/le/channel` を読み、
  `resp_put()` / `resp_put_tlv()` で応答データを積み、SW を返す。
* **T=0 の case** を表に書くのは、T=0 ではヘッダだけから P3 が Lc か Le か判断できないためである。
  61xx / 6Cxx / GET RESPONSE / 再発行時の応答保持 (コマンドを 2 回実行しない) はフレームワークが行う。
  T=1 と拡張長 APDU では case は自動判定である。
* `.default_applet` に指定したアプレットは ATR 直後から暗黙に選択されている。SELECT なしでコマンドを受けるカードはこれで作る。
  MF から直接 READ BINARY 等を受けるカードは `default_applet = NULL` + `iso_commands = true` で作る。
* アプレットが扱わなかったコマンド (表にない / `APDU_UNHANDLED`) は、`iso_commands` が真なら標準コマンドへ、
  それでも残れば `hooks.unhandled` へ、最後に `6D00` (INS 不明) または `6E00` (他の CLA にその INS がある) になる。

## 6. フック (`card_profile.hooks`)

| フック | 呼ばれる場面 | 既定動作 |
|--------|-------------|---------|
| `on_atr`, `on_deactivate` | ATR 送出後 / RST Low | なし |
| `check_access`, `on_select`, `on_file_changed` | ACL 評価 / ファイル選択 / 書き換え後 | PIN 規則 / なし |
| `classify` | T=0 で表にない INS の case 判定 | case 1 扱い |
| `unhandled` | 誰も処理しなかったコマンド | 6D00 / 6E00 |
| `get_data`, `put_data` | 00 CA / 00 DA | 6A81 |
| `internal_authenticate`, `external_authenticate` | 00 88 / 00 82 (GET CHALLENGE の値は `sec_last_challenge()`) | 6A81 |
| `manage_security_environment`, `perform_security_operation`, `generate_asymmetric_key_pair` | 00 22 / 00 2A / 00 46 (ISO 7816-8) | 6A81 |
| `create_file`, `delete_file` | 00 E0 / 00 E4 (ISO 7816-9) | 6A81 |
| `sm_unwrap`, `sm_wrap` | CLA のセキュアメッセージング bit が立ったコマンドの前後 | 6882 |
| `random_bytes` | GET CHALLENGE 等 | RP2040 のハードウェア乱数 |

暗号が必要なら pico-sdk 同梱の mbedtls (`pico_mbedtls`) をリンクしてフックの中で使用する。

## 7. プロファイルのその他

| フィールド | 意味 |
|-----------|------|
| `iso_commands` | 標準コマンド ([internals.md](internals.md) の表) を有効にする |
| `max_channels` | 論理チャネル数 (1..4) |
| `proprietary_cla_decode` | CLA 8X..FX でもチャネル / チェイニング / SM bit を解釈するか。既定 false なので CLA 90 は「80 + チェイニング」ではなく別の CLA である |
| `tx_extra_guard` | 自分の送信文字に付ける追加ガード ETU |

## ハンドラから使用できる API

| 関数 | 用途 |
|------|------|
| `resp_put`, `resp_put_u8`, `resp_put_tlv` | 応答データの追加 (バッファ上限 `APDU_BUF_SIZE` で切り詰める) |
| `card_keepalive()` | 時間のかかる処理中にリーダーを待たせる (T=0: NULL バイト 60、T=1: S(WTX)) |
| `card_nvm_changed()` | 永続データを変更したので次の RST Low で保存する |
| `card_request_bootsel()` | 応答送出後に BOOTSEL へ再起動する |
| `card_put_default_fci()` | 6F { 84 AID } 形式の FCI |
| `fs_cursor`, `fs_child`, `fs_by_name`, `fs_by_sfi`, `fs_select_file`, `fs_record_*`, `fs_put_fcp` | ファイルシステム操作 |
| `sec_find`, `sec_is_verified`, `sec_set_verified`, `sec_last_challenge`, `sec_random` | セキュリティ状態 |
