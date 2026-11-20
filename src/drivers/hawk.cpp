// Hawk HK500-series keyboards: HK510 and HK520 / HK530.
//
// They talk to Hawk Hub through a read-only GET_DEVICE_INFO (0x10) request for a 56-byte
// device-info block. The request is split into 8-byte-header frames; the payload per frame is the
// report length minus the report id minus that header:
//   2.4 GHz receiver  0C45:FEFE, usage page FF60 / usage 61, 33-byte reports (id 0 + 32 bytes)
//                     -> three frames at addresses 0, 24 and 48 (24 + 24 + 8 bytes)
//   USB cable         0C45:8006 (HK510) / 0C45:8009 (HK520 / HK530), usage page FF68 / usage 61,
//                     64-byte reports -> one frame with all 56 bytes
//   request  AA 10 <chunk> <addrLo> <addrHi> 00 <last> 00 ...
//   reply    55 10 <chunk> <addrLo> <addrHi> ...  data starts at byte 8
// The block holds the keyboard's identity at [4..7] (VID:PID), work mode at [16] (0 USB,
// 1 Bluetooth, 2 2.4 GHz), the battery level at [17] and the charge state at [18]
// (0 not charging, 1 charging, 2 full).
//
// The vendor app treats [17] as a live reading only while the keyboard is wireless; in USB mode it
// shows the charge state but no battery percentage, so this driver does the same (percent -1, with
// charging / full set). The USB keyboard is preferred when both the cable and the receiver are
// attached, and the receiver is then claimed so the keyboard is not listed twice.
//
// The same receiver id is also used by AULA F87 Pro V2 and AJAZZ dongles (sonix.cpp), so the
// identity in the reply must name a Hawk keyboard; otherwise the device is left unclaimed.
// Only GET_DEVICE_INFO is ever sent, so the driver is read-only.
//
// Source: the HK500 driver in Hawk Hub's Electron renderer bundle (HK500_DEVICE_IDS,
// commandUsagePageFor, getDeviceInfo). HK540 (36B0:304F, usage page FF00) uses a different,
// incompatible chip protocol and is explicitly unsupported by Hawk Hub itself, so it is not
// claimed here.
#include "drivers.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>

namespace {

constexpr USHORT kHawkVid = 0x0C45;
constexpr USHORT kHawkReceiverPid = 0xFEFE;
constexpr USHORT kHk510Pid = 0x8006;
constexpr USHORT kHk520Pid = 0x8009;
constexpr USHORT kUsbPage = 0xFF68;
constexpr USHORT kReceiverPage = 0xFF60;
constexpr USHORT kCommandUsage = 0x0061;
constexpr BYTE kRequestMarker = 0xAA;
constexpr BYTE kReplyMarker = 0x55;
constexpr BYTE kGetDeviceInfo = 0x10;
constexpr int kFrameHeader = 8;
constexpr int kInfoLen = 56;
// The receiver occasionally ignores a whole poll (5 of 30 first requests in a live test). A retry
// 100 ms later answered every time, so each attempt gets 500 ms and the driver retries once.
constexpr int kAttemptMs = 500;
constexpr int kRetryDelayMs = 100;

struct Target {
    USHORT pid;
    USHORT page;
    bool usb;
};

bool IsHawkModel(USHORT pid) { return pid == kHk510Pid || pid == kHk520Pid; }

// HK520 and HK530 share the USB product id; the product string tells them apart there. On the
// receiver the string is always "2.4G Dongle", so the combined name is used.
const wchar_t* ModelName(USHORT pid, const HidColl& c, bool usb) {
    if (pid == kHk510Pid) return L"Hawk HK510";
    if (pid != kHk520Pid) return nullptr;
    if (usb) {
        std::wstring product = HidProduct(c);
        for (wchar_t& ch : product) ch = (wchar_t)towupper(ch);
        if (product.find(L"HK530") != std::wstring::npos) return L"Hawk HK530";
        if (product.find(L"HK520") != std::wstring::npos) return L"Hawk HK520";
    }
    return L"Hawk HK520/HK530";
}

// One output report: the report id in front, then the 8-byte frame header and payload.
bool SendFrame(HidDev& d, const HidColl& c, int chunk, int addr, bool last) {
    std::vector<BYTE> out(c.outLen(), 0);
    out[0] = c.outId;
    out[1] = kRequestMarker;
    out[2] = kGetDeviceInfo;
    out[3] = (BYTE)chunk;
    out[4] = (BYTE)(addr & 0xFF);
    out[5] = (BYTE)((addr >> 8) & 0xFF);
    out[7] = last ? 1 : 0;
    return d.Write(out);
}

// Collects every reply chunk and copies its payload into the 56-byte block. Returns false on
// timeout or when a chunk is missing. Read() strips a leading zero report id only, so a numbered
// collection returns its id first and the frame starts one byte later.
bool ReadInfo(HidDev& d, const HidColl& c, int payload, int frames, std::vector<BYTE>& info, int timeoutMs) {
    info.assign(kInfoLen, 0);
    const int off = c.inId ? 1 : 0;
    const unsigned want = (1u << frames) - 1;
    unsigned got = 0;
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    while (got != want) {
        LONGLONG left = (LONGLONG)end - (LONGLONG)GetTickCount64();
        if (left <= 0) return false;
        std::vector<BYTE> r;
        int n = d.Read(r, (int)left);
        if (n < off + kFrameHeader + 1) continue;  // timeout or a short unrelated report
        const BYTE* f = r.data() + off;
        if (f[0] != kReplyMarker || f[1] != kGetDeviceInfo) continue;
        int addr = f[3] | ((int)f[4] << 8);
        if (addr < 0 || addr >= kInfoLen || addr % payload != 0) continue;
        int idx = addr / payload;
        if (idx >= frames) continue;
        unsigned bit = 1u << idx;
        if (got & bit) continue;  // duplicate chunk
        int take = std::min(payload, kInfoLen - addr);
        if (n < off + kFrameHeader + take) continue;  // truncated chunk: retry on the next poll
        std::memcpy(info.data() + addr, f + kFrameHeader, (size_t)take);
        got |= bit;
    }
    return true;
}

bool QueryInfo(HidDev& d, const HidColl& c, std::vector<BYTE>& info, int timeoutMs) {
    int payload = (int)c.outLen() - 1 - kFrameHeader;
    if (payload < 8) return false;
    int frames = (kInfoLen + payload - 1) / payload;
    if (frames < 1 || frames > 8) return false;
    d.Flush();
    for (int i = 0; i < frames; ++i) {
        int addr = i * payload;
        int take = std::min(payload, kInfoLen - addr);
        if (!SendFrame(d, c, take, addr, i == frames - 1)) return false;
    }
    return ReadInfo(d, c, payload, frames, info, timeoutMs);
}

bool PollTarget(PollContext& ctx, const Target& t) {
    if (ctx.IsClaimed(kHawkVid, t.pid)) return false;
    for (const HidColl* c : ctx.Find(kHawkVid, t.pid, [&t](const HidColl& c) {
             return c.page() == t.page && c.usage() == kCommandUsage && c.inLen() == c.outLen() &&
                    c.outLen() >= 1 + kFrameHeader + 8 && c.outLen() <= 1 + 64;
         })) {
        HidDev d;
        if (!d.Open(*c)) continue;
        std::vector<BYTE> info;
        if (!QueryInfo(d, *c, info, kAttemptMs)) {
            Sleep(kRetryDelayMs);
            if (!QueryInfo(d, *c, info, kAttemptMs)) continue;
        }
        USHORT vid = (USHORT)(info[4] | (info[5] << 8));
        USHORT pid = (USHORT)(info[6] | (info[7] << 8));
        if (vid != kHawkVid || !IsHawkModel(pid)) continue;
        if (t.usb && pid != c->pid) continue;  // the block must name this wired keyboard
        const wchar_t* name = ModelName(pid, *c, t.usb);
        if (!name) continue;
        int mode = info[16], level = info[17], charge = info[18];
        if (t.usb) {
            // USB mode: the vendor app does not treat the level as a live reading. A wireless
            // reading on the cable collection is still shown when it is valid.
            if (mode != 0 && (mode != 2 || level < 1 || level > 100)) continue;
        } else {
            if (mode != 2 || level < 1 || level > 100) continue;  // cable / Bluetooth mode
        }
        DeviceStatus s;
        s.key = L"hawk:" + Hex4(pid);  // the same across cable and receiver
        s.name = name;
        s.kind = DeviceKind::Keyboard;
        s.via = t.usb ? L"USB cable" : L"2.4 GHz receiver";
        s.percent = (t.usb && mode == 0) ? -1 : level;
        s.charging = charge == 1;
        s.full = charge == 2;
        // An idle keyboard sometimes misses a poll: keep the last value until it answers again.
        s.graceSec = 1800;
        ctx.Add(s, kHawkVid, t.pid);
        return true;
    }
    return false;
}

}  // namespace

void PollHawk(PollContext& ctx) {
    const Target kUsbTargets[] = {
        { kHk510Pid, kUsbPage, true },
        { kHk520Pid, kUsbPage, true },
    };
    for (const Target& t : kUsbTargets) {
        if (PollTarget(ctx, t)) {
            // The keyboard's receiver may still be plugged in; do not list the same keyboard twice.
            ctx.Claim(kHawkVid, kHawkReceiverPid);
            return;
        }
    }
    if (ctx.IsClaimed(kHawkVid, kHawkReceiverPid)) return;
    PollTarget(ctx, { kHawkReceiverPid, kReceiverPage, false });
}
