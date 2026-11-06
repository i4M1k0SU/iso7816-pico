# 自分のプロジェクトに組み込む

このリポジトリは単独でもビルドできる (デモカードが `build/examples/demo/iso7816-pico-demo.uf2` になる) が、
本来の使い方は **利用側プロジェクトに Git submodule として置き、CMake のライブラリとしてリンクする** ことである。
利用側が書くのはカードプロファイルとアプレット、それに数行の `main.c` だけで、フレームワーク内部のソースや
include パスを列挙する必要はない。

## 1. submodule を追加する

```sh
git submodule add https://github.com/i4M1k0SU/iso7816-pico.git external/iso7816-pico
git submodule update --init
```

clone する側は `git clone --recurse-submodules` を使う。更新は `git -C external/iso7816-pico pull` の後に親で commit する。

## 2. CMakeLists.txt

利用側の `CMakeLists.txt` は通常の Pico プロジェクトに 2 行足すだけである。

```cmake
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(PICO_BOARD pico CACHE STRING "Board type")        # pico2 なら RP2350

include(pico_sdk_import.cmake)                        # SDK 同梱のファイルをコピーしておく
project(my_card C CXX ASM)
pico_sdk_init()

# 必要なら設定を上書きする (add_subdirectory より前)
set(ISO7816_PIN_IO 4 CACHE STRING "")
set(ISO7816_PIN_CLK 1 CACHE STRING "")
set(ISO7816_PIN_RST 2 CACHE STRING "")
set(ISO7816_SELFTEST_PIN_CLK 16 CACHE STRING "")       # 起動時の自己診断に使う未接続ピン (-1 で自己診断なし)
set(ISO7816_SELFTEST_PIN_IO 17 CACHE STRING "")
set(APDU_BUF_SIZE 4096 CACHE STRING "")

add_subdirectory(external/iso7816-pico)               # ライブラリだけが定義される (デモは作られない)

add_executable(my_card main.c card_profile.c applets/my_applet.c)
target_link_libraries(my_card PRIVATE iso7816_pico::pico)
pico_enable_stdio_usb(my_card 1)                      # USB コンソールと無操作 BOOTSEL のために必要
pico_enable_stdio_uart(my_card 0)                     # UART stdio は既定で有効。GP0 / GP1 を取るので切る
pico_add_extra_outputs(my_card)
```

上書きできる設定と既定値は [include/iso7816_pico/config.h](../include/iso7816_pico/config.h) にある
(`ISO7816_PIN_*`、`ISO7816_SELFTEST_PIN_*`、`APDU_BUF_SIZE`、`CARD_MAX_CHANNELS`、`SEC_PIN_MAX_LEN`、`NVM_FLASH_BYTES`、
`LOG_RING_SIZE`、`WATCHDOG_TIMEOUT_MS`、`NVM_IDLE_SAVE_MS`)。値はコンパイル定義として利用側の翻訳単位にも伝わるので、
公開ヘッダの構造体サイズが食い違うことはない。ピンはフレームワークのコンパイル時に検査し、GP0〜GP29 の範囲外、
接点ピン同士の重複、自己診断ピンと接点ピンの重複は `#error` になる。`NVM_FLASH_BYTES` が 4096 の倍数でなければ
`_Static_assert` で止まる。

SDK は UART stdio を既定で有効にするので、`pico_enable_stdio_uart(<target> 0)` を必ず書く。有効のままだと UART0 が
GP0 / GP1 (GP1 は CLK 接点の既定ピン) を取り、アプリケーションの `printf()` も 115200 bps の UART を待つ。
フレームワークのコンソールとログは USB CDC だけに書くので、UART の有無に影響されない。
`pico_stdio_usb` はライブラリの公開依存であり、外すことはできない (コンソール、picotool のリセットインターフェイス、
1200 bps による BOOTSEL がこれに依存する)。TinyUSB を独自のデバイス構成で使うアプリケーションとは併用できない。
フレームワークが使うハードウェア資源の一覧は [8. フレームワークが使う資源](#8-フレームワークが使う資源) にある。

VS Code の Raspberry Pi Pico 拡張で作ったプロジェクトなら、拡張が生成する `CMakeLists.txt` の先頭ブロック
(SDK / picotool / pioasm の場所) をそのまま残し、`pico_sdk_init()` の後に上の `add_subdirectory` 以降を足せばよい。

## 3. main.c

```c
#include "iso7816_pico/runtime.h"

extern const card_profile_t my_card_profile;   // card_profile.c

int main(void) {
    card_runtime_init(&my_card_profile);       // stdio, PIO, 自己診断, NVM 読込, FS / PIN, ATR, ウォッチドッグ
    card_runtime_run();                        // リーダーの活性化ごとに 1 セッション。戻らない
}
```

USB や他の周辺機器を同じループで回したいときは `card_runtime_set_idle_hook()` に関数を登録する。
コマンドの合間とリーダー待ちの間に呼ばれる。

## 4. カードプロファイルとアプレット

[examples/demo/card_profile.c](../examples/demo/card_profile.c) と
[examples/demo/applets/demo_applet.c](../examples/demo/applets/demo_applet.c) をコピーして書き換える。
各項目の意味は [card-profile.md](card-profile.md)、型は [include/iso7816_pico/card.h](../include/iso7816_pico/card.h)。
利用側のソースから使うヘッダはすべて `#include "iso7816_pico/..."` である。

最小構成の例は [tests/embed/main.c](../tests/embed/main.c) にある (ATR 1 つ、アプレット 1 つ、ファイルシステムなし)。
`tests/embed/CMakeLists.txt` は上の手順そのもので、CI がこれをビルドして組み込み方法が壊れていないことを確認する。

## 5. ビルドと書き込み

```sh
cmake -S . -B build -G Ninja                   # PICO_SDK_PATH を設定しておくか、VS Code 拡張のブロックに任せる
cmake --build build
picotool load -f -x build/my_card.uf2          # 動作中のファームウェアを USB 経由で BOOTSEL に落として書く
```

Pico 2 (RP2350) 向けは `-DPICO_BOARD=pico2` を付けて別のビルドディレクトリで構成する。

## 6. FetchContent を使う場合

submodule を持ちたくなければ FetchContent でも同じターゲットが使える。

```cmake
include(FetchContent)
FetchContent_Declare(iso7816_pico
    GIT_REPOSITORY https://github.com/i4M1k0SU/iso7816-pico.git
    GIT_TAG        <commit SHA>)
FetchContent_MakeAvailable(iso7816_pico)
target_link_libraries(my_card PRIVATE iso7816_pico::pico)
```

## 7. システムクロックを下げる (動作電流の削減)

リーダーの VCC から Pico を動かすなど動作電流を抑えたい場合は、SDK の仕組みでシステムクロックを下げられる。
フレームワーク側に設定項目はなく、利用側の実行ファイルに compile definition を付けるだけで `clocks_init()`
(main より前) から有効になる。

```cmake
# 12 MHz 水晶 -> VCO 960 MHz / 5 / 4 = 48 MHz。USB の PLL は 48 MHz のまま変わらない。
target_compile_definitions(my_card PRIVATE
        SYS_CLK_KHZ=48000
        PLL_SYS_VCO_FREQ_KHZ=960000
        PLL_SYS_POSTDIV1=5
        PLL_SYS_POSTDIV2=4
)
```

* PLL の 3 つの値はまとめて指定する (揃っていないと SDK が `#error` を出す)。`PLL_SYS_VCO_FREQ_KHZ` は SDK 2.x では
  `PLL_SYS_VCO_FREQ_HZ` の別名である。ボードヘッダで定義しても同じ効果になる。
* 定義は **利用側の実行ファイル** に付ける。SDK のクロック初期化コードは実行ファイル側にコンパイルされるので、
  フレームワークのライブラリには何も要らない。
* PIO はシステムクロックで動いてリーダーの CLK を数えるので、システムクロックは CLK の 8 倍以上必要である
  ([internals.md](internals.md))。48 MHz なら CLK 6 MHz 程度までのリーダーが対象で、下回るとセッション開始時に警告が
  出る。コンソールの `status` でシステムクロックと CLK を確認できる。
* USB (コンソール、picotool による書き込み) は PLL_USB で動くので影響しない。電源投入時の突入電流は基板の
  コンデンサで決まり、変わらない。
* 48 MHz では SCR3310 (CLK 4.8 MHz) で T=0、T=1、PPS による F/D=372/4 の動作を確認済み。

## 8. フレームワークが使う資源

アプリケーションが周辺機器を足すときは、次の資源を避ける。

| 資源 | 使い方 |
|------|--------|
| pio0 | SM0 (受信) と SM1 (送信) を `phy_init()` が `pio_sm_claim()` で確保する。命令メモリは 32 語中 30 語を使う。アプリケーションの `pio_claim_unused_sm(pio0, ...)` は SM2 / SM3 を返すが、pio0 に載るプログラムは 2 語までである |
| pio1 | CLK 計測用の `clkcount` (3 語、SM 1 つ) を常駐させる。起動時の自己診断中だけ `clkgen` (2 語、SM 1 つ) を加え、終わると外す。SM は `pio_claim_unused_sm()` で取るので、残りはアプリケーションが使える |
| pio2 (RP2350) | 使わない |
| GPIO | `ISO7816_PIN_IO` / `_CLK` / `_RST` (既定 GP4 / GP1 / GP2)、RST の割り込みは `gpio_add_raw_irq_handler()` による共有ハンドラ。`ISO7816_SELFTEST_PIN_CLK` / `_IO` (既定 GP16 / GP17) は起動時に数 ms だけ使う |
| フラッシュ | 末尾の `NVM_FLASH_BYTES` (既定 64 KB)。書き込みは `flash_safe_execute()` (割り込み停止、他コアを停止) |
| USB | `pico_stdio_usb` (CDC コンソール、リセットインターフェイス) |
| ウォッチドッグ | `WATCHDOG_TIMEOUT_MS` (既定 3 秒、0 で無効) |

* `card_runtime_init()` は pio0 の SM0 / SM1 を確保し命令メモリをほぼ埋めるので、pio0 を使うアプリケーションの初期化は
  その後に行うか pio1 / pio2 を使う。先に pio0 の SM0 / SM1 を確保したり 3 語以上のプログラムを載せたりしていると
  `phy_init()` が panic する。
* 自己診断は `ISO7816_SELFTEST_PIN_CLK` に試験クロックを出し、`ISO7816_SELFTEST_PIN_IO` をプルアップ付きの I/O として使い、
  終わると両ピンを `GPIO_FUNC_NULL` に戻す。どちらも何も接続しないピンにする。GP16 / GP17 は SDK の SPI0 の既定ピンなので、
  そこを使うアプリケーションは別のピンを指定するか、どちらかを -1 にして自己診断を止める (起動ログは
  `selftest: disabled` になる)。
* NVM 領域は `PICO_FLASH_SIZE_BYTES - NVM_FLASH_BYTES` から始まる。`card_runtime_init()` はプログラムの末尾
  (`__flash_binary_end`) がその手前で終わっているかを確かめ、重なっていれば `NVM: program ends at ... NOT persisted` を
  ログに出して以後フラッシュを一切消去・書き込みしない (プログラム自身を消さないため)。大きなアプリケーションや、
  ボードのフラッシュ容量の指定 (`PICO_BOARD`、`PICO_FLASH_SIZE_BYTES`) の誤りで起こる。
* core1 を使う場合、`flash_safe_execute()` が core1 を止められるように、core1 側で最初に
  `flash_safe_execute_core_init()` を呼ぶ。呼ばないと NVM の保存がすべて失敗し、ログに
  `NVM: flash_safe_execute() failed (...)` と `NVM: save FAILED` が出る。
