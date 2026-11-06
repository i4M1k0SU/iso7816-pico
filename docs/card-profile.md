# カードを開発する

利用側が書くのはカードプロファイル (`card_profile_t` を 1 つ) とアプレット、そして `card_runtime_init()` に
プロファイルを渡す `main.c` である ([embedding.md](embedding.md))。雛形は
[examples/demo/card_profile.c](../examples/demo/card_profile.c) と
[examples/demo/applets/demo_applet.c](../examples/demo/applets/demo_applet.c)、
型と各フィールドの説明は [include/iso7816_pico/card.h](../include/iso7816_pico/card.h) にある。

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
起動時にフラッシュから読み込まれ、変更済み (dirty) の印が付いていれば (UPDATE BINARY などの標準コマンドは自動で付ける。
アプレットのハンドラやフックで構造体を書き換えたら `card_nvm_changed()` を呼ぶ)、次のコマンドを待つ間に文字の送受信が `NVM_IDLE_SAVE_MS` (既定 200 ms) 途絶えた時点で書き戻される。
RST が Low になったときにも、まだ dirty なら書き戻す (予備)。PIN / PUK の試行回数は例外で、照合の前に同期で保存する
(「PIN」の節)。

RST Low だけを書き戻しの契機にしないのは次の理由による。

* リーダーから給電するボード (README の配線どおり C1 VCC をダイオード経由で VSYS へ) では、非活性化で VCC が
  1 ms 程度で落ちる。RST Low で始めた 4 KB の消去と書き込みは途中で電源を失い、何も残らない。
* RST Low 中の保存にウォームリセットが重なると、ATR が 40 000 クロックの期限を過ぎる。

アイドル時保存の代償として、書き込み中の 50-100 ms は割り込みが止まり CPU も塞がる。T=0 ではリーダーはカードの応答を
待ってから送るので問題にならないが、T=1 ではその間に届いたブロックが 8 段の PIO 受信 FIFO からあふれることがある。
その場合リーダーは誤ったブロックまたは無応答として扱い、再送 (R ブロック) で回復するので、失うのは時間だけである。
EF の本体や `sec_pin_t` の配列はこの構造体の中に置く。コンソールの `erase` で消去すると、次の起動からソース上の初期値に
戻る。

### レイアウト版

フラッシュ上のイメージには、それを書いたファームウェアのレイアウト ID が記録される。レイアウト ID は
`NVM_LAYOUT_FRAMEWORK << 16 | .nvm_version` である。

* `.nvm_version` (`uint16_t`、既定 0) はアプリケーションの構造体の版である。構造体のメンバ、大きさ、意味を変えたら
  上げる。サイズが変わる変更はサイズでも検出されるが、サイズが同じ変更は版を上げなければ検出されない。
* `NVM_LAYOUT_FRAMEWORK` (`src/core/hal/nvm.h`) はフレームワークが構造体の中に置かせている型 (`sec_pin_t`) の版である。
  `sec_pin_t` (`SEC_PIN_MAX_LEN` を含む) を変えるときはフレームワーク側でこれを上げる。
* 起動時は、どのレイアウトとサイズのものも含めて、通し番号が最大で CRC の正しいイメージを探す。レイアウト ID と
  サイズがこのファームウェアと一致すればそれを読み込む。一致しなければ、そのイメージを今の構造体として解釈することは
  しない。`hooks.nvm_migrate` があればそれに渡し、真を返せばフックが `.nvm_blob` に書いた内容を使う。フックがない、
  または偽を返した場合は初期値のまま起動する。どちらの場合も両方のレイアウト ID をログに出し、blob を dirty にするので、
  最初のアイドル時 (または RST Low) に新しいレイアウトで保存され、次の起動からは普通に読み込まれる。それより古い、
  レイアウトの一致するイメージが残っていても、古い内容なので使わない。

  ```text
  NVM: stored image has layout 00000000 (560 bytes), firmware expects 00010000 (560 bytes): no nvm_migrate hook, using defaults
  NVM: defaults (560 bytes, slot 3/16, seq 41)
  ```

* `hooks.nvm_migrate(uint32_t old_layout, const void *old_blob, size_t old_size)` の `old_blob` はフラッシュ上の
  古いイメージ (読み出し専用、8 バイト境界) で、呼ばれた時点の `.nvm_blob` は初期値である。残せるものを `.nvm_blob` に
  移して真を返す。偽を返すときは `.nvm_blob` に触れない。ファイルシステムと PIN の初期化より前、起動時に 1 度だけ
  呼ばれる。

  ```c
  static bool my_nvm_migrate(uint32_t old_layout, const void *old_blob, size_t old_size) {
      if ((old_layout & 0xFFFF0000u) != ((uint32_t)NVM_LAYOUT_FRAMEWORK << 16)) return false;  // PIN の形式が違う
      if ((old_layout & 0xFFFF) != 1 || old_size != sizeof(nvm_v1_t)) return false;
      const nvm_v1_t *old = old_blob;
      memcpy(s_nvm.ef_hello, old->ef_hello, sizeof old->ef_hello);   // 新しいメンバは初期値のまま
      memcpy(s_nvm.pins, old->pins, sizeof old->pins);
      return true;
  }
  ```

  `NVM_LAYOUT_FRAMEWORK` を使うには `hal/nvm.h` を include する (`src/core` はインクルードパスに入っている)。
* 版の古いファームウェアに戻した場合も同じ規則で、新しいレイアウトのイメージは使われず、初期値で上書きされる。
* レイアウト ID を持たない V1 形式のヘッダ (レイアウト ID 導入前のファームウェアが書いたもの) はレイアウト 0 として扱う。
  したがってそのカードは、レイアウト ID を持つファームウェアを書き込んだ最初の起動で 1 度だけ初期値に戻る
  (`nvm_migrate` を用意すれば `old_layout == 0` のイメージから引き継げる)。

書き戻しは次の規則で行う (`src/core/hal/nvm.c`)。

* フラッシュ上のイメージと同じ内容なら書かない。
* 領域 (`NVM_FLASH_BYTES`、既定 64 KB) をスロット (ヘッダ + 構造体を 4 KB 単位に切り上げ) に分け、通し番号を付けて順繰りに書く。
  消去するのは次のスロットだけなので、各セクタの消去回数はスロット数分の 1 になる (構造体が 4 KB 未満なら 16 分の 1)。
* 起動時は通し番号が最大で CRC の正しいスロットを採用する (レイアウトが違えば上の「レイアウト版」のとおり)。書き込み中に
  電源が落ちても直前のイメージが残る。
* ヘッダ (24 バイト、リトルエンディアンの 32 ビット語 6 つ) は magic、サイズ、通し番号、CRC-32 (構造体のみ)、レイアウト ID、
  ヘッダ CRC (先頭 20 バイト、つまり magic からレイアウト ID までの CRC-32) からなる。構造体の CRC と合わせて、
  ヘッダのどのビットが化けてもそのイメージは見えなくなる。通し番号が化けて古いイメージが最新に見えることも、
  レイアウト ID が化けて正しいイメージがレイアウト変更と誤認され初期値で上書きされることもない。ヘッダ CRC の合わない
  イメージがあれば、起動時に `NVM: ignored 1 image(s) with a bad header CRC` のようにログに出す
  (`nvm_status()->bad_headers`)。
* ヘッダの形式は 3 つある。書くのは常に V3 で、V1 と V2 も読める。V2 のイメージは書き換え後の最初の起動でそのまま
  読み込まれ (レイアウト変更にはならない)、dirty になるので最初のアイドル時 (または RST Low) に V3 で書き直される。

  | 形式 | magic | 大きさ | 内容 |
  |------|-------|--------|------|
  | V1 | `7NVM` (`4D564E37`) | 16 バイト | magic、サイズ、通し番号、CRC。レイアウト 0 として扱う |
  | V2 | `7VN2` (`324E5637`) | 24 バイト | V1 + レイアウト ID + 予備 (`FFFFFFFF`、検査しない) |
  | V3 | `7VN3` (`334E5637`) | 24 バイト | V1 + レイアウト ID + ヘッダ CRC |
* そのためにはスロットが 2 つ以上必要である。1 つしか取れない大きさ (既定の 64 KB なら 32 744 バイトを超える構造体) では
  保存のたびに唯一のイメージを消してから書くことになるので、`nvm_load()` / `nvm_save()` は拒否し (保存されない)、
  起動時にその旨をログに出す。`_Static_assert(NVM_BLOB_FITS(sizeof(nvm_data_t)), "...")` (デモにある) でビルド時に検査できる。
  大きな構造体には `NVM_FLASH_BYTES` を増やす。
* 書き戻しはアイドル時に起きるので、PC/SC のホストが切断時にカードをリセットしなくても (Windows の `SCARD_LEAVE_CARD`)
  セッション中に保存される。

## 3. ファイルシステム

`fs_file_t` の配列で木構造を定義する (`[0]` が MF、`parent` ポインタで親を指定)。

| フィールド | 意味 |
|-----------|------|
| `fid`, `type` | ファイル ID と種別 (`FS_DF`, `FS_EF_TRANSPARENT`, `FS_EF_LINEAR_FIXED`, `FS_EF_LINEAR_VARIABLE`, `FS_EF_CYCLIC`) |
| `name`, `name_len` | DF 名 (AID)。アプレットの AID と同じ名前の DF は、そのアプレット選択時にカレント DF になる |
| `data`, `size` | EF 本体。レコード EF は先頭 4 バイトがヘッダ (`FS_REC_HDR(使用数, 先頭スロット)`)、`FS_LINEAR_FIXED_BYTES()` 等でサイズを計算する |
| `rec_size` | レコード長 (可変長なら最大長) |
| `sfi` | ショート EF 識別子 (1..30、0 は「なし」)。READ BINARY の P1 b8 / READ RECORD の P2 で参照する |
| `ac_read`, `ac_update` | アクセス条件: `FS_AC_ALWAYS`, `FS_AC_NEVER`, または必要な PIN の参照番号 (VERIFY の P2) |

アクセス条件は論理チャネルごとのセキュリティ状態で評価される。より複雑な規則は `hooks.check_access` で置き換える。

ショート EF 識別子として有効なのは 1..30 だけである。BINARY 系コマンドの P1 = 80 (SFI 0) と 9F (SFI 31)、
RECORD 系コマンドの P2 b8..b4 = 31 は 6A86 (P1-P2 誤り) になる。RECORD 系の P2 b8..b4 = 0 はカレント EF を指す。
SFI を持たない EF (`sfi = 0`) は SFI では参照できない (`fs_by_sfi(df, 0)` は NULL を返す)。

レコードを読み書きするコマンドは、存在するレコードだけを対象にする。

* 固定長 EF は全レコードが最初から存在する。UPDATE / WRITE RECORD は容量までの任意の番号に書ける。
* 可変長 EF とサイクリック EF のレコードは APPEND RECORD (E2) で追加したものだけである (ヘッダの使用数まで)。
  UPDATE / WRITE RECORD で使用数を超える番号や「次」を指定すると 6A83 になり、使用数も記録ポインタも変わらない。
  デモの可変長 EF 0104 は空で始まるので、最初の 1 件は APPEND RECORD で作る。
* `fs_record_write()` も同じ規則で既存のレコードだけを上書きし、レコードを増やすのは `fs_record_append()` だけである。

## 4. PIN

`sec_pin_t` の配列 (`.pins` / `.n_pins`)。参照番号 (`ref`)、値、許容長、リトライ回数、PUK を持ち、
VERIFY (20) / CHANGE REFERENCE DATA (24) / RESET RETRY COUNTER (2C) / ENABLE・DISABLE VERIFICATION (28/26)
をそのまま使用できる。ハンドラからは `sec_is_verified(ch, ref)` / `sec_set_verified()` で参照・設定する
(EXTERNAL AUTHENTICATE 成功時に PIN 相当の権限を与える、など)。

`sec_pin_t` の配列は NVM の構造体の中に置かなければならない (デモの `nvm_data_t.pins`)。VERIFY、CHANGE REFERENCE
DATA、ENABLE/DISABLE VERIFICATION の PIN 照合と RESET RETRY COUNTER の PUK 照合では、リトライ回数を減らした構造体全体を
照合の前にフラッシュへ書く (`sec.c` の `try_pin()`)。誤った PIN を送って 63Cx を受け取った直後に電源を切っても、減った回数が
残るので総当たりできない。配列が構造体の外にあるとこの保存は役に立たず、回数は電源断で元に戻る。
書き込みは 50-100 ms (割り込み停止) かかるが、既定のパラメータでは T=0 の WWT (約 0.7 s) / T=1 の BWT (約 1.2 s) に収まるので
keep-alive は送らない。保存に失敗した場合はログに出し、回数は RAM 上で数え続ける (後のアイドル時保存で再試行する)。
正しい PIN で回数を戻した変更は通常のアイドル時保存で書くので、照合 1 回につき最大 2 回の書き込みになる。

試行回数を減らす前に、データ欄の長さをすべて検査する。形の誤ったコマンドは 6700 を返し、回数を消費しない。

| コマンド | 回数を減らす前に 6700 とする条件 |
|---------|-------------------------------|
| VERIFY (P1 = 00) | PIN が `SEC_PIN_MAX_LEN` より長い (`pin_transform` と `plain_len` があれば `plain_len` と異なる)。空のデータ欄は照合せず状態を返す |
| ENABLE / DISABLE VERIFICATION (P1 = 00) | データ欄が空、または VERIFY と同じ長さ条件。P1 = 01 はデータ欄があると 6700 |
| CHANGE REFERENCE DATA (P1 = 00) | 旧 PIN の後ろに新 PIN がない、新 PIN が `min_len` / `max_len` / `SEC_PIN_MAX_LEN` の範囲外。新 PIN を `pin_transform` が拒めば 6A80 (これも照合前) |
| RESET RETRY COUNTER (P1 = 00) | PUK の後ろに新 PIN がない、新 PIN の長さが範囲外 (CHANGE REFERENCE DATA と同じ) |
| RESET RETRY COUNTER (P1 = 01) | データ欄が PUK の長さと異なる (余分なバイトも不可) |
| RESET RETRY COUNTER (P1 = 02 / 03) | 02 は新 PIN の長さが範囲外、03 はデータ欄がある (どちらも回数は使わず、照合済みが必要) |

格納できる値の長さは `SEC_PIN_MAX_LEN` (`config.h`、既定 16)、PIN の数は `SEC_MAX_PINS` (`sec.h`、32。セキュリティ状態を
PIN ごとに 1 ビットで持つため) が上限である。起動時の `sec_init()` は、PIN が多すぎるプロファイル (先頭の 32 個だけを使う) と、
`len` / `puk_len` / `max_len` が `SEC_PIN_MAX_LEN` を超える PIN をログに出す。

PIN と PUK は既定では平文のまま NVM に置かれる。平文を残したくない場合は `hooks.pin_transform` を実装する。
格納時 (CHANGE REFERENCE DATA、RESET RETRY COUNTER) と照合時 (VERIFY、PUK) の両方で値がこの関数を通るので、
ソルト付きハッシュなどを返せばフラッシュには変換後の値しか残らない。その場合、プロファイルに書く初期値は変換後の形で、
`sec_pin_t.plain_len` に PIN と PUK の平文長 (固定) を入れる (旧 PIN + 新 PIN のように 1 つのデータ欄に 2 つの値が入るコマンドを
そこで分割する)。`plain_len` があると、照合する値も新しい PIN もこの長さでなければ 6700 になる。
`pin_transform` があって `plain_len` が 0 の PIN はプロファイルの誤りで、起動時にログに出る。分割できない
CHANGE REFERENCE DATA P1 = 00 と RESET RETRY COUNTER P1 = 00 / 01 は、端末の誤りではないので 6700 ではなく 6F00 を返し、
回数を消費しない (VERIFY は分割が要らないのでそのまま使える)。

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
    .name = "demo", .aid = s_aid, .aid_len = sizeof s_aid,   // AID は 1..16 バイト
    .ctx = NULL,         // アプリケーション用 (インスタンスの状態など)。フレームワークは触れない
    .cmds = s_cmds, .n_cmds = 2,
    .select = NULL,      // NULL なら 6F len { 84 len AID } を返す
    .deselect = NULL,    // void (*)(const applet_t *self, uint8_t channel)
    .process = NULL,     // 表にないコマンドの受け皿 (switch で書く場合)。APDU_UNHANDLED を返すと次の層へ
    .classify = NULL,    // process で扱うコマンドの T=0 case (switch で書く場合は必須)
    .reset = NULL,       // ATR ごとの初期化: void (*)(const applet_t *self)
};
```

* ハンドラは `uint16_t handler(const apdu_t *cmd, apdu_resp_t *resp)`。`cmd->data/lc/le/channel` を読み、
  `resp_put()` / `resp_put_tlv()` で応答データを積み、SW を返す。
* `cmd->applet` は、ディスパッチャがコマンドを受け取った時点で CLA が指すチャネルで選択されていたアプレットである
  (なければ NULL)。表のハンドラ、`process`、`select`、標準コマンド、すべてのフック (`sm_unwrap` / `sm_wrap` を含む) に
  渡る `cmd` で設定済みである。`select` の中では選択されようとしているアプレット自身を指す。`reset`、`deselect`、
  `classify` は第 1 引数 `self` で自分を受け取る。1 つの実装で複数の AID (インスタンス) を扱うときは、インスタンスごとに
  `applet_t` を作り、状態を `.ctx` に置いて `cmd->applet->ctx` / `self->ctx` で引く。

  ```c
  typedef struct { uint32_t counter; } inst_t;
  static inst_t s_inst1, s_inst2;
  static uint16_t h_count(const apdu_t *cmd, apdu_resp_t *resp) {
      inst_t *in = cmd->applet->ctx;
      in->counter++;
      resp_put_u8(resp, (uint8_t)in->counter);
      return SW_OK;
  }
  static void inst_reset(const applet_t *self) { ((inst_t *)self->ctx)->counter = 0; }
  const applet_t app1 = { .name = "app1", .aid = s_aid1, .aid_len = 6, .ctx = &s_inst1, .cmds = s_cmds, .n_cmds = 1,
                          .reset = inst_reset };
  const applet_t app2 = { .name = "app2", .aid = s_aid2, .aid_len = 6, .ctx = &s_inst2, .cmds = s_cmds, .n_cmds = 1,
                          .reset = inst_reset };
  ```
* 応答バッファは `APDU_BUF_SIZE` (既定 4096) バイトである。`resp_put*()` は入りきらない分を捨て、`resp->overflow` を
  立てる (立ったままになる)。ハンドラが 9000 / 61xx / 62xx / 63xx を返したときに `overflow` が立っていれば、
  ディスパッチャは不完全な応答を送らず、データなしの 6F00 に置き換えて `<コマンド名>: response overflow (cap 4096)` を
  ログに出す。`select` の FCI があふれた場合は選択不成立 (6F00) になる。大きな応答を組み立てる前に
  `resp->cap - resp->len` で残りを確かめる。READ BINARY は拡張 Le がバッファより大きければ入るだけ返す
  (あふれではない)。
* **T=0 の case** を表に書くのは、T=0 ではヘッダだけから P3 が Lc か Le か判断できないためである。
  61xx / 6Cxx / GET RESPONSE / 再発行時の応答保持 (コマンドを 2 回実行しない) はフレームワークが行う。
  T=1 と拡張長 APDU では case は自動判定である。
* コマンドを表に書かず `process` の switch で解読するアプレットは、`.classify` も用意する。表にない INS は T=0 では
  case 1 扱い (P3 を無視し、データ部を受信しない) になるので、`classify` がないとデータ部が失われる。`classify` は
  CLA が指すチャネルで選択中のアプレットについて、その表の次、標準コマンド表と `hooks.classify` の前に参照される。
  自分のコマンドでなければ `APDU_CASE_UNKNOWN` を返す。

  ```c
  static apdu_case_t my_classify(const applet_t *self, uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2) {
      if ((cla & 0xF0) != 0x80) return APDU_CASE_UNKNOWN;
      switch (ins) {
      case 0x42: return APDU_CASE3;   // データを受け取る
      case 0x44: return APDU_CASE4;   // データを受け取り、返す
      default:   return APDU_CASE_UNKNOWN;
      }
  }
  ```
* `select` は SELECT のたびに呼ばれ、名前が AID と同じ DF があればそれがカレント DF になった状態で呼ばれる。9000 か
  警告 (62xx / 63xx) を返せば選択が成立する。それ以外の SW を返すと選択は不成立で、そのチャネルではどのアプレットも
  選択されていない状態になる (前のアプレットは `deselect` 済み、セキュリティ状態は消去、カレントファイルは SELECT 前の
  まま、応答データは捨てる)。以後のコマンドはアプレットに届かない。
* `.default_applet` に指定したアプレットは ATR 直後から暗黙に選択されている。SELECT なしでコマンドを受けるカードはこれで作る。
  暗黙選択でも `select` は呼ばれる (SELECT by DF name、P2 = 0C の合成コマンドを渡し、応答は捨てる。9000 以外ならログに
  出す)。したがって `select` で行う準備 (デモではカレント EF の設定) は ATR 直後から効いている。
  MF から直接 READ BINARY 等を受けるカードは `default_applet = NULL` + `iso_commands = true` で作る。
* MANAGE CHANNEL でチャネルを開くと、基本チャネルから開いた場合は既定アプレット (なければ MF) が、他のチャネルから
  開いた場合はそのチャネルのアプレットとカレント DF / EF が新しいチャネルで選択される (ISO 7816-4)。どちらの場合も新しい
  チャネルで `select` が呼ばれ、失敗すればチャネルは開かずその SW を返す。他のチャネルから開いた場合のカレントファイルは
  `select` の後で元のチャネルのものに戻す。セキュリティ状態は引き継がない。
* アプレットが扱わなかったコマンド (表にない / `APDU_UNHANDLED`) は、`iso_commands` が真なら標準コマンドへ、
  それでも残れば `hooks.unhandled` へ、最後に `6D00` (INS 不明) または `6E00` (CLA 違い) になる。`6E00` になるのは、
  そのチャネルで選択中のアプレットの表か標準コマンド (`iso_commands` が真のとき)、または SELECT / MANAGE CHANNEL に、
  別の CLA でその INS のハンドラがある場合だけである。ハンドラのない (分類だけの) 表の項目、選択されていないアプレットの
  コマンド、`process` の switch の中のコマンドは数えない。
* ファイルシステムのないプロファイル (`files = NULL`) でも `iso_commands` が真なら、SELECT は 6A82、カレント EF を
  対象とする READ BINARY / READ RECORD 等は 6986、短縮 EF 識別子付きのものは 6A82 を返す。
* T=1 では GET RESPONSE (`00 C0`) と ENVELOPE (`00 C2`) もディスパッチャに届く。アプレットやフックが扱わなければ、
  GET RESPONSE は 6985 (T=0 で取り出す応答がないときと同じ)、ENVELOPE は 6D00 である。

## 6. フック (`card_profile.hooks`)

| フック | 呼ばれる場面 | 既定動作 |
|--------|-------------|---------|
| `on_atr`, `on_deactivate` | ATR 送出後 / RST Low | なし |
| `check_access`, `on_select`, `on_file_changed` | ACL 評価 / ファイル選択 / 書き換え後 | PIN 規則 / なし |
| `classify` | T=0 で表にもアプレットの `classify` にもない INS の case 判定 | case 1 扱い |
| `unhandled` | 誰も処理しなかったコマンド | 6D00 / 6E00 |
| `get_data`, `put_data` | 00 CA / 00 DA | 6A81 |
| `internal_authenticate`, `external_authenticate` | 00 88 / 00 82 (GET CHALLENGE の値は `sec_last_challenge()`) | 6A81 |
| `manage_security_environment`, `perform_security_operation`, `generate_asymmetric_key_pair` | 00 22 / 00 2A / 00 46 (ISO 7816-8) | 6A81 |
| `create_file`, `delete_file` | 00 E0 / 00 E4 (ISO 7816-9) | 6A81 |
| `sm_unwrap`, `sm_wrap` | CLA のセキュアメッセージング bit が立ったコマンドの前後 | 6882 |
| `pin_transform` | PIN / PUK の保存前と照合前の変換 (「PIN」の節) | 平文のまま |
| `nvm_migrate` | 起動時、フラッシュのイメージのレイアウト ID かサイズが違うとき (「レイアウト版」) | 初期値 |
| `random_bytes` | GET CHALLENGE 等 | RP2040 / RP2350 のハードウェア乱数 |

暗号が必要なら pico-sdk 同梱の mbedtls (`pico_mbedtls`) をリンクしてフックの中で使用する。

## 7. プロファイルのその他

| フィールド | 意味 |
|-----------|------|
| `iso_commands` | 標準コマンド ([internals.md](internals.md) の表) を有効にする |
| `max_channels` | 論理チャネル数 (1..`CARD_MAX_CHANNELS`。0 は 1、それを超える値は `CARD_MAX_CHANNELS` とみなす。`CARD_MAX_CHANNELS` は既定 4、最大 20) |
| `proprietary_cla_decode` | CLA 8X..FX でもチャネル / チェイニング / SM bit を解釈するか。既定 false なので CLA 90 は「80 + チェイニング」ではなく別の CLA である |
| `tx_extra_guard` | 自分の送信文字に付ける追加ガード ETU |
| `name`, `version` | コンソール接続時のバナーと `card` コマンドに表示する名前と版。NULL なら CMake の `pico_set_program_name` / `pico_set_program_version` の値 |

## 8. コンソールコマンド

USB シリアルコンソールに独自のコマンドを足せる。`console_cmd_t` の表 (名前、`help` で表示する説明、ハンドラ) を
`.console_cmds` / `.n_console_cmds` に登録する。コンソールは「名前 引数...」の 1 行を Enter で受け取り、名前が一致した
ハンドラに残りの文字列 (前後の空白を除いたもの、無ければ空文字列) を渡す。出力は `log_printf()` (`iso7816_pico/log.h`) で行う。
フレームワーク自身の名前 (help status card trace txperr gpio pins apdu save erase bootsel reboot) が優先される。
`help` (または空行の Enter) は組み込みコマンドを `built-in:`、プロファイルのコマンドを `.name` (カード名) の見出しの下に
分けて表示する。
ハンドラはアイドルループ、すなわちコマンドの合間やリーダー待ちの間に呼ばれるので、長く回る処理では
`watchdog_update()` と `log_drain()` を挟む。

```c
static void cmd_counter(const char *args) {
    if (*args) s_counter = strtoul(args, NULL, 0);        // "counter 42" sets it
    log_printf("counter = %lu", (unsigned long)s_counter);
}

static const console_cmd_t s_console_cmds[] = {
    { "counter", "[value]  show or set the demo counter", cmd_counter },
};
// card_profile: .console_cmds = s_console_cmds, .n_console_cmds = 1
```

デモカードは `dump` (NVM の内容) と `unblock` (PIN のリトライ回数を戻す) を定義している。

## ハンドラから使用できる API

| 関数 | 用途 |
|------|------|
| `resp_put`, `resp_put_u8`, `resp_put_tlv` | 応答データの追加 (入りきらない分は捨てて `overflow` を立てる。成功 SW なら 6F00 になる) |
| `APDU_RESP_INIT(buf, cap)`, `resp_reset()` | 自前の `apdu_resp_t` の初期化 / 空に戻す (`overflow` も消す) |
| `card_keepalive()` | 時間のかかる処理中にリーダーを待たせる (T=0: NULL バイト 60、T=1: S(WTX)) |
| `card_nvm_changed()` | 永続データを変更したのでアイドル時 (または RST Low) に保存する |
| `card_request_bootsel()` | 応答送出後に BOOTSEL へ再起動する |
| `card_put_default_fci()` | 6F { 84 AID } 形式の FCI (AID が 16 バイトを超えればログに出し `overflow` を立てる) |
| `fs_cursor`, `fs_child`, `fs_by_name`, `fs_by_sfi`, `fs_select_file`, `fs_record_*`, `fs_put_fcp` | ファイルシステム操作 |
| `sec_find`, `sec_is_verified`, `sec_set_verified`, `sec_last_challenge`, `sec_random` | セキュリティ状態 |

## 非互換変更: アプレット文脈、応答あふれ、NVM レイアウト版

公開 API を利用者が使い始める前に、1 度だけ互換性のない変更をまとめて行った。それ以前のプロファイルとアプレットは
次のとおり直す。

| 変更 | 利用側で必要な対応 |
|------|------------------|
| `applet_t.reset` が `void (*)(const applet_t *self)` になった | 引数 `self` を受け取る (使わなければ `(void)self;`) |
| `applet_t.deselect` が `void (*)(const applet_t *self, uint8_t channel)` になった | 第 1 引数 `self` を足す |
| `applet_t.classify` が `apdu_case_t (*)(const applet_t *self, uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2)` になった | 第 1 引数 `self` を足す (`hooks.classify` は変わらない) |
| `apdu_t` に `const struct applet *applet`、`applet_t` に `void *ctx` が増えた | 不要 (指示付き初期化子なら 0 / NULL になる) |
| `apdu_resp_t` に `bool overflow` が増えた | `{ buf, cap, 0 }` のような位置指定の初期化子は `APDU_RESP_INIT(buf, cap)` に置き換える (`-Wextra` で警告になる) |
| 入りきらない応答は 9000 ではなく 6F00 (データなし) になる | 大きな応答は `resp->cap - resp->len` を確かめて組み立てる |
| 17 バイト以上の AID の既定 FCI は 6F00 になる | AID を 16 バイト以内にする |
| `card_profile_t.nvm_version` と `hooks.nvm_migrate` が増え、NVM のヘッダが 24 バイトになった | 構造体を変えるたびに `nvm_version` を上げる。引き継ぎたければ `nvm_migrate` を書く |
| 旧形式の NVM イメージはレイアウト 0 として扱う | 既存のカードは書き換え後の最初の起動で 1 度だけ初期値に戻る (`nvm_migrate` で `old_layout == 0` から引き継げる) |
