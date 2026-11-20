// Exercise the Hawk HK510 / HK520 / HK530 poller without HID hardware.
#include "../src/drivers/drivers.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

static std::deque<std::vector<BYTE>> replies;
static std::vector<std::vector<BYTE>> writes;
static int opens;
static bool openOk = true, writeOk = true;
static std::wstring productName;

bool HidDev::Open(const HidColl& c) { ++opens; c_ = c; return openOk; }
void HidDev::Close() {}
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    writes.push_back(data);
    return writeOk;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    if (replies.empty()) return -1;
    out = replies.front();
    replies.pop_front();
    return (int)out.size();
}
std::wstring Hex4(USHORT value) {
    wchar_t text[5];
    swprintf(text, 5, L"%04X", value);
    return text;
}
std::wstring HidProduct(const HidColl&) { return productName; }

static HidColl Collection(USHORT pid, USHORT page, USHORT len, BYTE id = 0) {
    HidColl c;
    c.vid = 0x0C45;
    c.pid = pid;
    c.iface = 3;
    c.caps.UsagePage = page;
    c.caps.Usage = 0x0061;
    c.caps.InputReportByteLength = c.caps.OutputReportByteLength = len;
    c.inId = c.outId = id;
    return c;
}
static HidColl Dongle() { return Collection(0xFEFE, 0xFF60, 33); }
static HidColl Usb(USHORT pid = 0x8006, BYTE id = 0) { return Collection(pid, 0xFF68, 65, id); }

static std::vector<BYTE> Info(USHORT pid = 0x8006, BYTE mode = 2, BYTE level = 86, BYTE charge = 0) {
    std::vector<BYTE> b(56, 0);
    b[4] = 0x45;
    b[5] = 0x0C;
    b[6] = (BYTE)(pid & 0xFF);
    b[7] = (BYTE)(pid >> 8);
    b[16] = mode;
    b[17] = level;
    b[18] = charge;
    return b;
}

// Mimics HidDev::Read: an unnumbered report loses its leading zero id, a numbered one keeps it.
static std::vector<BYTE> Reply(const std::vector<BYTE>& info, int addr, const HidColl& c) {
    const int len = c.outLen();
    const int off = c.inId ? 1 : 0;
    const int payload = len - 1 - 8;
    const int take = std::min(payload, 56 - addr);
    std::vector<BYTE> r((size_t)(len - (c.inId ? 0 : 1)), 0);
    r[off] = 0x55;
    r[off + 1] = 0x10;
    r[off + 2] = (BYTE)take;
    r[off + 3] = (BYTE)(addr & 0xFF);
    r[off + 4] = (BYTE)(addr >> 8);
    if (addr == 48) r[off + 6] = 1;
    std::memcpy(r.data() + off + 8, info.data() + addr, (size_t)take);
    return r;
}

static void QueueReplies(const std::vector<BYTE>& info, const HidColl& c) {
    replies.clear();
    const int payload = c.outLen() - 1 - 8;
    const int frames = (56 + payload - 1) / payload;
    for (int i = 0; i < frames; ++i) replies.push_back(Reply(info, i * payload, c));
}

static PollContext Poll(const std::vector<BYTE>& info, const HidColl& c) {
    opens = 0;
    writes.clear();
    QueueReplies(info, c);
    PollContext ctx;
    ctx.colls = { c };
    PollHawk(ctx);
    return ctx;
}

// One GET_DEVICE_INFO frame, with the report id in front.
static std::vector<BYTE> Frame(int len, BYTE id, int chunk, int addr, bool last) {
    std::vector<BYTE> f((size_t)len, 0);
    f[0] = id;
    f[1] = 0xAA;
    f[2] = 0x10;
    f[3] = (BYTE)chunk;
    f[4] = (BYTE)(addr & 0xFF);
    f[5] = (BYTE)(addr >> 8);
    f[7] = last ? 1 : 0;
    return f;
}

int main() {
    // 2.4 GHz receiver: three 33-byte frames, 24 + 24 + 8 bytes.
    PollContext ctx = Poll(Info(), Dongle());
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].key == L"hawk:8006" && ctx.out[0].name == L"Hawk HK510");
    assert(ctx.out[0].kind == DeviceKind::Keyboard && ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].percent == 86 && !ctx.out[0].charging && !ctx.out[0].full);
    assert(ctx.out[0].connected && !ctx.out[0].asleep && ctx.out[0].graceSec == 1800);
    assert(ctx.IsClaimed(0x0C45, 0xFEFE));
    assert(opens == 1 && writes.size() == 3);
    assert(writes[0] == Frame(33, 0, 24, 0, false));
    assert(writes[1] == Frame(33, 0, 24, 24, false));
    assert(writes[2] == Frame(33, 0, 8, 48, true));
    PollHawk(ctx);
    assert(opens == 1 && writes.size() == 3);  // already claimed: no second query

    // USB cable: one 65-byte frame. In USB mode the vendor app shows no battery level, only the
    // charge state, so the entry stays connected with an unknown percentage.
    ctx = Poll(Info(0x8006, 0, 0, 1), Usb());
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].key == L"hawk:8006" && ctx.out[0].name == L"Hawk HK510");
    assert(ctx.out[0].via == L"USB cable" && ctx.out[0].percent == -1);
    assert(ctx.out[0].charging && !ctx.out[0].full && !ctx.out[0].asleep && ctx.out[0].connected);
    assert(ctx.IsClaimed(0x0C45, 0x8006));
    assert(opens == 1 && writes.size() == 1);
    assert(writes[0] == Frame(65, 0, 56, 0, true));

    ctx = Poll(Info(0x8006, 0, 100, 2), Usb());
    assert(ctx.out.size() == 1 && ctx.out[0].percent == -1 && ctx.out[0].full && !ctx.out[0].charging);
    ctx = Poll(Info(0x8006, 0, 0, 0), Usb());
    assert(ctx.out.size() == 1 && ctx.out[0].percent == -1 && !ctx.out[0].charging && !ctx.out[0].full);

    // A 64-byte USB collection has a 55-byte payload, so the block is split into 55 + 1 bytes.
    {
        HidColl compact = Collection(0x8006, 0xFF68, 64);
        ctx = Poll(Info(0x8006, 0, 0, 1), compact);
        assert(ctx.out.size() == 1 && ctx.out[0].charging && ctx.out[0].percent == -1);
        assert(writes.size() == 2);
        assert(writes[0] == Frame(64, 0, 55, 0, false));
        assert(writes[1] == Frame(64, 0, 1, 55, true));
    }

    // A live wireless reading on the cable collection is still shown.
    ctx = Poll(Info(0x8006, 2, 64, 0), Usb());
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 64 && !ctx.out[0].charging);

    // HK520 and HK530 share the USB product id; the product string picks the name.
    productName = L"Hawk HK520";
    ctx = Poll(Info(0x8009, 0, 0, 1), Usb(0x8009));
    assert(ctx.out.size() == 1 && ctx.out[0].name == L"Hawk HK520" && ctx.out[0].key == L"hawk:8009");
    productName = L"Hawk HK530";
    ctx = Poll(Info(0x8009, 0, 0, 1), Usb(0x8009));
    assert(ctx.out.size() == 1 && ctx.out[0].name == L"Hawk HK530");
    productName.clear();
    ctx = Poll(Info(0x8009, 0, 0, 1), Usb(0x8009));
    assert(ctx.out.size() == 1 && ctx.out[0].name == L"Hawk HK520/HK530");

    // Cable and receiver both attached: one entry, the cable wins and the receiver is claimed.
    {
        opens = 0;
        writes.clear();
        QueueReplies(Info(0x8006, 0, 0, 1), Usb());
        PollContext both;
        both.colls = { Dongle(), Usb() };
        PollHawk(both);
        assert(opens == 1 && both.out.size() == 1 && both.out[0].via == L"USB cable");
        assert(both.IsClaimed(0x0C45, 0x8006) && both.IsClaimed(0x0C45, 0xFEFE));
    }

    // A numbered collection: the report id is written and kept in the reply.
    {
        HidColl numbered = Usb(0x8006, 6);
        ctx = Poll(Info(0x8006, 0, 0, 1), numbered);
        assert(writes.size() == 1 && writes[0][0] == 6 && writes[0] == Frame(65, 6, 56, 0, true));
        assert(ctx.out.size() == 1 && ctx.out[0].charging && ctx.out[0].percent == -1);
    }

    // USB mode on the receiver is not a wireless reading: the USB keyboard is polled separately.
    assert(Poll(Info(0x8006, 0, 100, 1), Dongle()).out.empty());
    assert(Poll(Info(0x8006, 1, 80, 0), Dongle()).out.empty());
    // Another brand's keyboard on the shared receiver id, invalid levels and modes are left alone.
    ctx = Poll(Info(0x1234), Dongle());
    assert(ctx.out.empty() && !ctx.IsClaimed(0x0C45, 0xFEFE));
    assert(Poll(Info(0x8006, 2, 0), Dongle()).out.empty());
    assert(Poll(Info(0x8006, 2, 101), Dongle()).out.empty());
    assert(Poll(Info(0x1234, 0, 0, 1), Usb()).out.empty());
    assert(!Poll(Info(0x1234, 0, 0, 1), Usb()).IsClaimed(0x0C45, 0x8006));
    // The identity block must name the wired keyboard, not just any Hawk model.
    assert(Poll(Info(0x8009, 0, 0, 1), Usb(0x8006)).out.empty());

    // Unrelated notifications, short reports and duplicate chunks are skipped.
    {
        std::vector<BYTE> info = Info();
        std::vector<BYTE> notify(32, 0);
        notify[0] = 0x55;
        notify[1] = 0xFA;  // device notification, not our command
        std::vector<BYTE> shortReport(6, 0);
        shortReport[0] = 0x55;
        shortReport[1] = 0x10;
        opens = 0;
        replies = { notify, shortReport, Reply(info, 0, Dongle()), Reply(info, 0, Dongle()), Reply(info, 24, Dongle()),
                    Reply(info, 48, Dongle()) };
        PollContext noisy;
        noisy.colls = { Dongle() };
        PollHawk(noisy);
        assert(noisy.out.size() == 1 && noisy.out[0].percent == 86);
    }

    // A collection that is not a Hawk command interface is not even opened.
    for (int fault = 0; fault < 8; ++fault) {
        HidColl c = Usb();
        if (fault == 0) c.vid = 0x1234;
        if (fault == 1) c.pid = 0x8010;
        if (fault == 2) c.caps.UsagePage = 0xFF60;
        if (fault == 3) c.caps.Usage = 0x62;
        if (fault == 4) c.caps.InputReportByteLength = 64;
        if (fault == 5) c.caps.OutputReportByteLength = 63;
        if (fault == 6) c.caps.OutputReportByteLength = 16;
        if (fault == 7) c.caps.OutputReportByteLength = 66;
        opens = 0;
        PollContext skip;
        skip.colls = { c };
        PollHawk(skip);
        assert(skip.out.empty() && opens == 0);
    }

    writeOk = false;
    ctx = Poll(Info(), Dongle());
    assert(ctx.out.empty() && !ctx.IsClaimed(0x0C45, 0xFEFE));
    writeOk = true;
    openOk = false;
    assert(Poll(Info(), Dongle()).out.empty());
    openOk = true;
    replies.clear();
    PollContext none;
    none.colls = { Dongle() };
    PollHawk(none);
    assert(none.out.empty() && !none.IsClaimed(0x0C45, 0xFEFE));

    puts("Hawk driver tests passed");
}
