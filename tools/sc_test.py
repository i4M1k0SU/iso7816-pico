#!/usr/bin/env python3
"""Exercise the iso7816-pico card through any PC/SC reader (Linux / macOS / Windows).

Counterpart of sc_test.ps1 built on pyscard (`pip install pyscard`; on Linux pcsc-lite and its daemon pcscd must be
installed and running).  Connects to the first reader whose name contains --reader, prints the ATR and protocol, then
runs the demo card's commands (applet, file system, PINs, channels, chaining, extended length) and checks the status
words.  61 xx (GET RESPONSE) and 6C xx (wrong Le) are followed here in case the reader driver does not.

    tools/sc_test.py                          run the suite, protocol chosen by the reader
    tools/sc_test.py --protocol T1            require T=1 (the card's ATR must offer it)
    tools/sc_test.py --apdu "80 10 00 00 00"  send one APDU
    tools/sc_test.py --bootsel                also send the REBOOT BOOTSEL APDU at the end
"""
import argparse
import sys

try:
    from smartcard.scard import (
        SCARD_PROTOCOL_T0, SCARD_PROTOCOL_T1, SCARD_RESET_CARD, SCARD_S_SUCCESS, SCARD_SCOPE_USER,
        SCARD_SHARE_DIRECT, SCARD_SHARE_SHARED, SCARD_LEAVE_CARD, SCardConnect, SCardDisconnect,
        SCardEstablishContext, SCardGetErrorMessage, SCardListReaders, SCardReconnect, SCardReleaseContext,
        SCardStatus, SCardTransmit, SCARD_PCI_T0, SCARD_PCI_T1,
    )
except ImportError:
    sys.exit("pyscard is required: pip install pyscard")

SCARD_W_REMOVED_CARD = 0x80100069
SCARD_W_UNRESPONSIVE_CARD = 0x80100066
SCARD_W_UNPOWERED_CARD = 0x80100067
SCARD_E_PROTO_MISMATCH = 0x8010000F


def hexs(data):
    return " ".join(f"{b:02X}" for b in data)


def from_hex(s):
    s = "".join(c for c in s if c in "0123456789abcdefABCDEF")
    return [int(s[i:i + 2], 16) for i in range(0, len(s), 2)]


def check(rc, what):
    if rc != SCARD_S_SUCCESS:
        raise SystemExit(f"{what} failed: 0x{rc & 0xFFFFFFFF:08X} {SCardGetErrorMessage(rc)}")


class Card:
    def __init__(self, reader_match, proto_mask):
        rc, self.ctx = SCardEstablishContext(SCARD_SCOPE_USER)
        check(rc, "SCardEstablishContext")
        rc, readers = SCardListReaders(self.ctx, [])
        check(rc, "SCardListReaders")
        names = [r for r in readers if reader_match in r]
        if not names:
            raise SystemExit(f"no reader matching '{reader_match}' (found: {', '.join(readers)})")
        self.reader = names[0]
        print(f"reader : {self.reader}")

        rc, self.card, self.proto = SCardConnect(self.ctx, self.reader, SCARD_SHARE_SHARED, proto_mask)
        if rc & 0xFFFFFFFF in (SCARD_W_UNRESPONSIVE_CARD, SCARD_W_UNPOWERED_CARD):
            # Windows remembers a "mute" card until it is re-inserted.  A direct connection plus a
            # reconnect-with-reset makes the reader try again, which is what you want after reflashing the Pico.
            print("card marked mute; forcing a cold reset ...")
            rc, self.card, _ = SCardConnect(self.ctx, self.reader, SCARD_SHARE_DIRECT, 0)
            check(rc, "SCardConnect(direct)")
            rc, self.proto = SCardReconnect(self.card, SCARD_SHARE_SHARED, proto_mask, SCARD_RESET_CARD)
            if rc != SCARD_S_SUCCESS:
                SCardDisconnect(self.card, SCARD_LEAVE_CARD)
                rc, self.card, self.proto = SCardConnect(self.ctx, self.reader, SCARD_SHARE_SHARED, proto_mask)
        check(rc, "SCardConnect")

        rc, _, _, proto, atr = SCardStatus(self.card)
        check(rc, "SCardStatus")
        self.proto = proto
        self.pci = SCARD_PCI_T1 if proto == SCARD_PROTOCOL_T1 else SCARD_PCI_T0
        name = {SCARD_PROTOCOL_T0: "T=0", SCARD_PROTOCOL_T1: "T=1"}.get(proto, f"0x{proto:X}")
        print(f"ATR    : {hexs(atr)}   ({name})")

    def close(self):
        # Reset the card on disconnect: the PC/SC layer otherwise keeps it powered and the next run continues the
        # same session (security status kept, nothing written back to the card's NVM).
        SCardDisconnect(self.card, SCARD_RESET_CARD)
        SCardReleaseContext(self.ctx)

    def transmit(self, apdu):
        """Returns (data, sw); follows 61 xx / 6C xx itself."""
        data = []
        for _ in range(40):
            rc, resp = SCardTransmit(self.card, self.pci, list(apdu))
            check(rc, "SCardTransmit")
            if len(resp) < 2:
                raise SystemExit("short response")
            sw1, sw2 = resp[-2], resp[-1]
            data += resp[:-2]
            if sw1 == 0x6C:
                apdu = apdu[:4] + [sw2]
                continue
            if sw1 == 0x61:
                apdu = [0x00, 0xC0, 0x00, 0x00, sw2]
                continue
            return data, (sw1 << 8) | sw2
        raise SystemExit("too many 61/6C hops")


class Suite:
    def __init__(self, card):
        self.card = card
        self.fail = 0

    def step(self, label, apdu_hex, expect_sw, expect_hex=None, expect_len=None):
        data, sw = self.card.transmit(from_hex(apdu_hex))
        ok = sw == expect_sw
        if expect_hex is not None:
            ok = ok and data == from_hex(expect_hex)
        if expect_len is not None:
            ok = ok and len(data) == expect_len
        if not ok:
            self.fail += 1
        shown = hexs(data)
        if len(shown) > 60:
            shown = shown[:60] + f" ..({len(data)} bytes)"
        elif data and all(0x20 <= b <= 0x7E for b in data):
            shown += '  "' + bytes(data).decode("ascii") + '"'
        shown_apdu = apdu_hex[:44] + ".." if len(apdu_hex) > 44 else apdu_hex
        print(f"[{'PASS' if ok else 'FAIL'}] {label:<30} {shown_apdu:<46} -> SW {sw:04X} {shown}")
        return data, sw

    def section(self, title):
        print(f"\n--- {title} ---")

    def run(self, proto):
        s = self.step
        seq200 = hexs([(i % 256) for i in range(1, 201)])
        hello16 = "50 69 63 6F 21 20 66 72 6F 6D 20 69 73 6F 37 38"
        rec1 = "72 65 63 6F 72 64 20 31 20 20 20 20 20 20 20 20"
        rec2 = "72 65 63 6F 72 64 20 32 20 20 20 20 20 20 20 20"
        rec3 = "74 68 69 72 64 20 72 65 63 6F 72 64 20 20 20 20"

        self.section("demo applet (CLA 80)")
        s("SELECT by AID", "00 A4 04 00 05 F0 50 49 43 4F", 0x9000, "6F 07 84 05 F0 50 49 43 4F")
        s("SELECT unknown AID", "00 A4 04 00 02 11 22", 0x6A82)
        s("GET VERSION", "80 10 00 00 00", 0x9000)
        s("ECHO", "80 20 00 00 04 DE AD BE EF", 0x9000, "DE AD BE EF")
        c1, _ = s("GET COUNTER", "80 30 00 00 04", 0x9000)
        c2, _ = s("GET COUNTER (Le=00)", "80 30 00 00 00", 0x9000)
        n1 = int.from_bytes(bytes(c1), "big")
        n2 = int.from_bytes(bytes(c2), "big")
        if n2 != n1 + 1:
            print(f"[FAIL] counter did not increment ({n1} -> {n2})")
            self.fail += 1
        else:
            print(f"[PASS] counter incremented ({n1} -> {n2})")
        s("ECHO 200 bytes", f"80 20 00 00 C8 {seq200}", 0x9000, seq200)
        s("SLOW (keep-alive 2 s)", "80 40 00 00 00", 0x9000, "64 6F 6E 65")
        s("unknown INS", "00 11 00 00 00", 0x6D00)
        s("unknown CLA", "90 10 00 00 00", 0x6E00)

        self.section("transparent EF 0101 + PIN 01")
        s("UPDATE BINARY (no PIN)", "00 D6 00 00 03 41 42 43", 0x6982)
        s("VERIFY wrong PIN", "00 20 00 01 04 30 30 30 30", 0x63C2)
        s("VERIFY status", "00 20 00 01 00", 0x63C2)
        s("VERIFY 1234", "00 20 00 01 04 31 32 33 34", 0x9000)
        s("VERIFY status", "00 20 00 01 00", 0x9000)
        s("UPDATE BINARY", "00 D6 00 00 05 50 69 63 6F 21", 0x9000)
        s("READ BINARY 16", "00 B0 00 00 10", 0x9000, hello16)
        s("READ BINARY 256", "00 B0 00 00 00", 0x9000, expect_len=256)
        s("READ BINARY via SFI 1", "00 B0 81 05 04", 0x9000, "20 66 72 6F")
        s("READ BINARY bad offset", "00 B0 01 00 10", 0x6B00)
        s("UPDATE chained (1/2)", "10 D6 00 F0 04 43 48 41 49", 0x9000)
        s("UPDATE chained (2/2)", "00 D6 00 F0 04 4E 45 44 21", 0x9000)
        s("READ BINARY @F0", "00 B0 00 F0 08", 0x9000, "43 48 41 49 4E 45 44 21")
        s("CHANGE REF DATA 1234->4321", "00 24 00 01 08 31 32 33 34 34 33 32 31", 0x9000)
        s("VERIFY 4321", "00 20 00 01 04 34 33 32 31", 0x9000)
        s("CHANGE REF DATA back", "00 24 00 01 08 34 33 32 31 31 32 33 34", 0x9000)
        s("RESET RETRY (PUK)", "00 2C 01 01 08 31 32 33 34 35 36 37 38", 0x9000)

        self.section("SELECT by FID / path, FCP")
        s("SELECT MF", "00 A4 00 00 02 3F 00", 0x9000)
        s("SELECT DF 7F10 (FCP)", "00 A4 01 04 02 7F 10", 0x9000)
        s("SELECT EF 0102, no data", "00 A4 02 0C 02 01 02", 0x9000, expect_len=0)
        s("SELECT parent", "00 A4 03 00 00", 0x9000)
        s("SELECT path from MF", "00 A4 08 04 04 7F 10 01 01", 0x9000)
        s("SELECT by DF name", "00 A4 04 00 05 F0 50 49 43 4F", 0x9000)
        s("SELECT missing FID", "00 A4 02 00 02 01 99", 0x6A82)

        self.section("records (EF 0102 linear fixed, 0103 cyclic, 0104 linear variable)")
        s("READ RECORD 1 (SFI 2)", "00 B2 01 14 10", 0x9000, rec1)
        s("READ RECORD 2", "00 B2 02 04 10", 0x9000, rec2)
        s("READ RECORD next (3)", "00 B2 00 02 10", 0x9000, expect_len=16)
        s("READ RECORD 5", "00 B2 05 04 10", 0x6A83)
        s("UPDATE RECORD 3", f"00 DC 03 04 10 {rec3}", 0x9000)
        s("READ RECORD 3", "00 B2 03 04 10", 0x9000, rec3)
        s('SEARCH RECORD "rec"', "00 A2 01 04 03 72 65 63", 0x9000, "01 02")
        s("APPEND RECORD cyclic", "00 E2 00 18 08 01 02 03 04 05 06 07 08", 0x9000)
        s("APPEND RECORD cyclic", "00 E2 00 18 08 11 12 13 14 15 16 17 18", 0x9000)
        s("READ RECORD 1 cyclic", "00 B2 01 1C 08", 0x9000, "11 12 13 14 15 16 17 18")
        s("READ RECORD 2 cyclic", "00 B2 02 1C 08", 0x9000, "01 02 03 04 05 06 07 08")
        s("SELECT EF 0104", "00 A4 02 0C 02 01 04", 0x9000)
        s("READ RECORD (PIN 81)", "00 B2 01 04 00", 0x6982)
        s("VERIFY PIN 81", "00 20 00 81 04 30 30 30 30", 0x9000)
        s("UPDATE RECORD 1 (var)", "00 DC 01 04 03 61 62 63", 0x9000)
        s("READ RECORD 1 (var)", "00 B2 01 04 00", 0x9000, "61 62 63")

        self.section("GET DATA, GET CHALLENGE, authentication hooks")
        s("GET DATA 0100", "00 CA 01 00 00", 0x9000, "00 11 22 33 44 55 66 77")
        s("GET DATA unknown", "00 CA 03 00 00", 0x6A88)
        ch, _ = s("GET CHALLENGE", "00 84 00 00 08", 0x9000, expect_len=8)
        crypto = hexs([b ^ 0x5A for b in ch])
        s("EXTERNAL AUTH (good)", f"00 82 00 00 08 {crypto}", 0x9000)
        s("EXTERNAL AUTH (no challenge)", f"00 82 00 00 08 {crypto}", 0x6985)
        s("INTERNAL AUTH", "00 88 00 00 04 01 02 03 04 00", 0x9000, "5B 58 59 5E")
        s("PSO (no hook)", "00 2A 80 86 02 01 02", 0x6A81)

        self.section("logical channels")
        s("MANAGE CHANNEL open", "00 70 00 00 01", 0x9000, "01")
        s("SELECT AID on ch 1", "01 A4 04 00 05 F0 50 49 43 4F", 0x9000)
        s("READ BINARY on ch 1", "01 B0 00 00 05", 0x9000, "50 69 63 6F 21")
        s("UPDATE on ch 1 (no PIN there)", "01 D6 00 00 01 41", 0x6982)
        s("MANAGE CHANNEL close", "00 70 80 01 00", 0x9000)
        s("READ BINARY on closed ch", "01 B0 00 00 05", 0x6881)

        self.section("extended length")
        if proto == SCARD_PROTOCOL_T0:
            s("ENVELOPE (ext. ECHO)", "00 C2 00 00 0B 80 20 00 00 00 00 04 DE AD BE EF", 0x9000, "DE AD BE EF")
            big = [(i % 256) for i in range(1, 301)]
            s("ENVELOPE 300 B (1/2)", f"00 C2 00 00 FF 80 20 00 00 00 01 2C {hexs(big[:248])}", 0x9000)
            s("ENVELOPE 300 B (2/2)", f"00 C2 00 00 34 {hexs(big[248:])}", 0x9000, hexs(big))
        else:
            s("ECHO 4E (ext. Lc/Le)", "80 20 00 00 00 00 04 DE AD BE EF 00 00", 0x9000, "DE AD BE EF")
            big = hexs([(i % 256) for i in range(1, 1001)])
            s("ECHO 1000 B extended", f"80 20 00 00 00 03 E8 {big} 00 00", 0x9000, big)
            s("READ BINARY 256 (SFI 1)", "00 B0 81 00 00", 0x9000, expect_len=256)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reader", default="SCR33", help="substring of the reader name (default: SCR33)")
    ap.add_argument("--protocol", choices=["T0", "T1", "Any"], default="Any", help="protocol to request")
    ap.add_argument("--apdu", help="send one APDU (hex) instead of the suite")
    ap.add_argument("--bootsel", action="store_true", help="send REBOOT BOOTSEL (80 E0 00 00 00) at the end")
    args = ap.parse_args()

    mask = {"T0": SCARD_PROTOCOL_T0, "T1": SCARD_PROTOCOL_T1}.get(args.protocol,
                                                                    SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1)
    card = Card(args.reader, mask)
    try:
        if args.apdu:
            data, sw = card.transmit(from_hex(args.apdu))
            print(f"-> SW {sw:04X} {hexs(data)}")
            return 0
        suite = Suite(card)
        suite.run(card.proto)
        print()
        if suite.fail:
            print(f"{suite.fail} check(s) FAILED")
        else:
            print("all checks passed")
        if args.bootsel:
            suite.step("REBOOT BOOTSEL", "80 E0 00 00 00", 0x9000)
            print("the Pico should now show up as the RPI-RP2 drive")
        return 1 if suite.fail else 0
    finally:
        card.close()


if __name__ == "__main__":
    sys.exit(main())
