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
set(APDU_BUF_SIZE 4096 CACHE STRING "")

add_subdirectory(external/iso7816-pico)               # ライブラリだけが定義される (デモは作られない)

add_executable(my_card main.c card_profile.c applets/my_applet.c)
target_link_libraries(my_card PRIVATE iso7816_pico::pico)
pico_enable_stdio_usb(my_card 1)                      # USB コンソールと無操作 BOOTSEL のために必要
pico_add_extra_outputs(my_card)
```

上書きできる設定と既定値は [include/iso7816_pico/config.h](../include/iso7816_pico/config.h) にある
(`ISO7816_PIN_*`、`APDU_BUF_SIZE`、`CARD_MAX_CHANNELS`、`SEC_PIN_MAX_LEN`、`NVM_FLASH_BYTES`、`LOG_RING_SIZE`、
`WATCHDOG_TIMEOUT_MS`、`NVM_IDLE_SAVE_MS`)。値はコンパイル定義として利用側の翻訳単位にも伝わるので、公開ヘッダの構造体サイズが
食い違うことはない。

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
