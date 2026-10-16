#include "pcmcia.h"
#include "state.h"
#include "../util/log.h"
#include <cstring>

namespace wm {

namespace {

// Card Information Structure of an NE2000-compatible Ethernet PC Card: function "network", the
// configuration register at 0x3F8, I/O entries 0x300/0x320/0x340/0x360 (32 ports, 8/16-bit) and the
// manufacturer ID ne2000.dll's DetectNE2000 matches. One CIS byte per even attribute address.
const uint8_t kCis[] = {
    0x01, 3, 0xDC, 0x00, 0xFF,                       // common memory descriptor
    0x17, 3, 0x49, 0x00, 0xFF,                       // attribute memory descriptor
    0x21, 2, 0x06, 0x03,                             // function ID: network adapter
    0x15, 27, 0x04, 0x01, 'P', 'C', 'M', 'C', 'I', 'A', 0x00, 'E', 't', 'h', 'e', 'r', 'n', 'e', 't',
    ' ', 'C', 'a', 'r', 'd', 0x00, 0x00, 0x00, 0x00, 0xFF,   // version 1 strings
    0x13, 3, 'C', 'I', 'S',                          // link target
    0x1A, 5, 0x01, 0x24, 0xF8, 0x03, 0x03,           // configuration: registers at 0x3F8
    0x1B, 17, 0xE0, 0x81, 0x1D, 0x3F, 0x55, 0x4D, 0x5D, 0x06, 0x86, 0x46, 0x26, 0xFC, 0x24, 0x65, 0x30, 0xFF, 0xFF,
    0x1B, 7, 0x20, 0x08, 0xCA, 0x60, 0x00, 0x03, 0x1F,   // I/O 0x300, 32 ports
    0x1B, 7, 0x21, 0x08, 0xCA, 0x60, 0x20, 0x03, 0x1F,   // I/O 0x320
    0x1B, 7, 0x22, 0x08, 0xCA, 0x60, 0x40, 0x03, 0x1F,   // I/O 0x340
    0x1B, 7, 0x23, 0x08, 0xCA, 0x60, 0x60, 0x03, 0x1F,   // I/O 0x360
    0x20, 4, 0x01, 0x8A, 0x00, 0x01,                 // manufacturer ID
    0x14, 0,                                         // no link
    0xFF, 0,                                         // end
};

constexpr uint32_t kRamBase = 0x4000, kRamEnd = 0x8000;

// ISR bits
constexpr uint8_t PRX = 0x01, PTX = 0x02, RDC = 0x40, RST = 0x80;

}  // namespace

void Pcmcia::serialize(StateIO &s) {
    s.tag("PCMC");
    s.io(cardIn); s.io(index); s.io(power); s.io(intCtl); s.io(cscChange); s.io(cscConfig); s.io(winEnable);
    s.io(ioCtl); s.io(chipInfo); s.io(ioStart); s.io(ioEnd); s.io(fcsr);
    s.io(cr); s.io(pstart); s.io(pstop); s.io(bnry); s.io(tsr); s.io(tpsr); s.io(isr); s.io(imr);
    s.io(rcr); s.io(tcr); s.io(dcr); s.io(rsr); s.io(curr);
    s.io(tbcr); s.io(rsar); s.io(rbcr); s.io(dmaAddr); s.io(dmaCount);
    s.io(par); s.io(mar); s.io(prom); s.io(ram);
    if (!s.writing) updateIrq();
}

// ---------------------------------------------------------------------------------------------
// Bus decode

uint32_t Pcmcia::read(uint32_t pa, int size) {
    uint32_t off = pa - 0x10000000u;
    if (off < 0x01000000u) return cardIn && powered() ? memRead(off & 0xFFFF, size) : 0;
    uint32_t port = off - 0x01000000u, cardPort;
    if (port == 0x3E0) return index;
    if (port == 0x3E1) return controllerRead(index);
    if (mapIo(port, cardPort)) return nicRead(cardPort, size);
    return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
}

void Pcmcia::write(uint32_t pa, uint32_t v, int size) {
    uint32_t off = pa - 0x10000000u;
    if (off < 0x01000000u) {
        if (cardIn && powered()) memWrite(off & 0xFFFF, v, size);
        return;
    }
    uint32_t port = off - 0x01000000u, cardPort;
    if (port == 0x3E0) { index = (uint8_t)v; return; }
    if (port == 0x3E1) { controllerWrite(index, (uint8_t)v); return; }
    if (mapIo(port, cardPort)) nicWrite(cardPort, v, size);
}

bool Pcmcia::mapIo(uint32_t port, uint32_t &cardPort) const {
    if (!cardIn || !powered()) return false;
    for (int w = 1; w >= 0; w--) {
        if (!(winEnable & (0x40 << w))) continue;
        if (port >= ioStart[w] && port <= ioEnd[w]) {
            cardPort = port - ioStart[w];
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// PD6710 controller

uint8_t Pcmcia::controllerRead(uint8_t reg) {
    switch (reg) {
        case 0x00: return 0x83;  // revision the BSP's driver expects
        case 0x01: return cardIn ? (uint8_t)(0x0C | (powered() ? 0x20 : 0)) : 0;  // CD1|CD2, READY
        case 0x02: return power;
        case 0x03: return intCtl;
        case 0x04: { uint8_t v = cscChange; cscChange = 0; return v; }
        case 0x05: return cscConfig;
        case 0x06: return winEnable;
        case 0x07: return ioCtl;
        case 0x08: return (uint8_t)ioStart[0];
        case 0x09: return (uint8_t)(ioStart[0] >> 8);
        case 0x0A: return (uint8_t)ioEnd[0];
        case 0x0B: return (uint8_t)(ioEnd[0] >> 8);
        case 0x0C: return (uint8_t)ioStart[1];
        case 0x0D: return (uint8_t)(ioStart[1] >> 8);
        case 0x0E: return (uint8_t)ioEnd[1];
        case 0x0F: return (uint8_t)(ioEnd[1] >> 8);
        case 0x16: return 1;  // 5 V card, so it is treated as 16-bit
        case 0x1F: { uint8_t v = chipInfo; chipInfo &= ~0xC0; return v; }
        default: return 0;
    }
}

void Pcmcia::controllerWrite(uint8_t reg, uint8_t v) {
    switch (reg) {
        case 0x02: {
            bool was = powered();
            power = v;
            if (cardIn && !was && powered()) resetNic();
            updateIrq();
            break;
        }
        case 0x03: intCtl = v; updateIrq(); break;
        case 0x05: cscConfig = v; break;
        case 0x06: winEnable = v; break;
        case 0x07: ioCtl = v; break;
        case 0x08: ioStart[0] = (ioStart[0] & 0xFF00) | v; break;
        case 0x09: ioStart[0] = (uint16_t)((ioStart[0] & 0xFF) | (v << 8)); break;
        case 0x0A: ioEnd[0] = (ioEnd[0] & 0xFF00) | v; break;
        case 0x0B: ioEnd[0] = (uint16_t)((ioEnd[0] & 0xFF) | (v << 8)); break;
        case 0x0C: ioStart[1] = (ioStart[1] & 0xFF00) | v; break;
        case 0x0D: ioStart[1] = (uint16_t)((ioStart[1] & 0xFF) | (v << 8)); break;
        case 0x0E: ioEnd[1] = (ioEnd[1] & 0xFF00) | v; break;
        case 0x0F: ioEnd[1] = (uint16_t)((ioEnd[1] & 0xFF) | (v << 8)); break;
        case 0x1F: chipInfo = 0xC0; break;
        default: break;  // memory windows and timing: not needed
    }
}

void Pcmcia::setInserted(bool on) {
    if (on == cardIn) return;
    cardIn = on;
    if (!on) power = 0;
    resetNic();
    cscChange |= 0x08;                    // card detect changed
    if (cscConfig & 0x08) ic.raise(IRQ_EINT3);  // the controller's status-change line
    updateIrq();
    LOGI("pcmcia: network card %s", on ? "inserted" : "removed");
}

void Pcmcia::setMac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) prom[2 * i] = prom[2 * i + 1] = mac[i];
    prom[28] = prom[29] = prom[30] = prom[31] = 'W';  // "WW": a 16-bit card
    prom[14] = prom[15] = 'W';
}

// ---------------------------------------------------------------------------------------------
// Card attribute memory

uint32_t Pcmcia::memRead(uint32_t a, int size) {
    if (a >= kRamBase && a + (size - 1) < kRamEnd) {
        uint32_t v = 0;
        memcpy(&v, &ram[a - kRamBase], size);
        return v;
    }
    if (a == 0x3FA) return fcsr;
    if (a / 2 < sizeof kCis) {
        uint32_t b = kCis[a / 2];
        return size == 1 ? b : (b << 8) | b;
    }
    return 0;
}

void Pcmcia::memWrite(uint32_t a, uint32_t v, int size) {
    if (a >= kRamBase && a + (size - 1) < kRamEnd) {
        memcpy(&ram[a - kRamBase], &v, size);
        return;
    }
    if (a == 0x3FA) {
        fcsr = (uint8_t)v;
        if (fcsr & 0x01) fcsr &= ~0x03;  // interrupt acknowledge
    }
}

// ---------------------------------------------------------------------------------------------
// DP8390

void Pcmcia::resetNic() {
    cr = 0x21;  // stopped, remote DMA idle
    pstart = pstop = bnry = tsr = tpsr = curr = rcr = rsr = dcr = 0;
    tcr = 0x06;  // loopback until configured
    tbcr = rsar = rbcr = dmaAddr = dmaCount = 0;
    isr = RST;
    memset(par, 0, sizeof par);
    memset(mar, 0, sizeof mar);
    memset(ram, 0, sizeof ram);
    updateIrq();
}

void Pcmcia::raise(uint8_t bits) {
    isr |= bits;
    fcsr |= 0x02;
    updateIrq();
}

void Pcmcia::updateIrq() {
    bool level = cardIn && powered() && (intCtl & 0x0F) && (isr & imr & 0x7F);
    gpio.setExtLevel(8, level);
}

uint8_t Pcmcia::dmaByte(bool write, uint8_t v) {
    uint8_t r = 0;
    if (dmaAddr >= kRamBase && dmaAddr < kRamEnd) {
        if (write) ram[dmaAddr - kRamBase] = v;
        else r = ram[dmaAddr - kRamBase];
    } else if (!write && dmaAddr < sizeof prom) {
        r = prom[dmaAddr];
    }
    dmaAddr++;
    if (pstop && dmaAddr >= (uint32_t)pstop << 8) dmaAddr = (uint16_t)((pstart << 8) + (dmaAddr - (pstop << 8)));
    return r;
}

uint32_t Pcmcia::nicRead(uint32_t port, int size) {
    if (port == 0x10 || port == 0x11) {  // remote DMA data port
        if (!dmaCount) return 0;
        int n = size == 1 ? 1 : 2;
        uint32_t v = dmaByte(false, 0);
        if (n == 2) v |= (uint32_t)dmaByte(false, 0) << 8;
        dmaCount = dmaCount > n ? dmaCount - n : 0;
        if (!dmaCount) raise(RDC);
        return v;
    }
    if (port == 0x1F) return 0;  // reset port: writing resets
    if (port > 0x0F) return 0xFF;
    switch (cr >> 6) {
        case 0:
            switch (port) {
                case 0x00: return cr;
                case 0x03: return bnry;
                case 0x04: return tsr;
                case 0x07:
                    if ((cr & 0x01) && !(isr & RST)) raise(RST);  // the driver polls for the reset bit
                    return isr;
                case 0x08: return (uint8_t)dmaAddr;
                case 0x09: return (uint8_t)(dmaAddr >> 8);
                case 0x0C: return rsr;
                default: return 0;
            }
        case 1:
            if (port == 0) return cr;
            if (port <= 6) return par[port - 1];
            if (port == 7) return curr;
            return mar[port - 8];
        case 2:
            switch (port) {
                case 0x00: return cr;
                case 0x01: return pstart;
                case 0x02: return pstop;
                case 0x04: return tpsr;
                case 0x0C: return rcr;
                case 0x0D: return tcr;
                case 0x0E: return dcr;
                case 0x0F: return imr;
                default: return 0;
            }
        default: return cr;
    }
}

void Pcmcia::nicWrite(uint32_t port, uint32_t v, int size) {
    if (port == 0x10 || port == 0x11) {
        if (!dmaCount) return;
        int n = size == 1 ? 1 : 2;
        dmaByte(true, (uint8_t)v);
        if (n == 2) dmaByte(true, (uint8_t)(v >> 8));
        dmaCount = dmaCount > n ? dmaCount - n : 0;
        if (!dmaCount) raise(RDC);
        return;
    }
    if (port == 0x1F) { resetNic(); return; }
    if (port > 0x0F) return;
    uint8_t b = (uint8_t)v;
    if (port == 0) {
        cr = b;
        if (cr & 0x01) isr |= RST;
        if (cr & 0x02) {
            uint8_t rd = (cr >> 3) & 7;
            if (rd & 4) dmaCount = 0;                 // abort / complete remote DMA
            else if (rd == 1 || rd == 2) { dmaCount = rbcr; dmaAddr = rsar; }
            if (cr & 0x04) {                          // transmit
                tsr = 0;
                bool loopback = !(dcr & 0x08) || (tcr & 0x06);
                uint32_t start = (uint32_t)tpsr << 8;
                if (!loopback && onTransmit && start >= kRamBase && start + tbcr <= kRamEnd && tbcr)
                    onTransmit(&ram[start - kRamBase], tbcr);
                tsr = 0x01;
                cr &= ~0x04;
                raise(PTX);
            }
        }
        updateIrq();
        return;
    }
    switch (cr >> 6) {
        case 0:
            switch (port) {
                case 0x01: pstart = b; break;
                case 0x02: pstop = b; break;
                case 0x03: bnry = b; isr &= ~RST; break;
                case 0x04: tpsr = b; break;
                case 0x05: tbcr = (tbcr & 0xFF00) | b; break;
                case 0x06: tbcr = (uint16_t)((tbcr & 0xFF) | (b << 8)); break;
                case 0x07: isr &= ~b; if (!(isr & 0x7F)) fcsr &= ~0x02; break;
                case 0x08: rsar = (rsar & 0xFF00) | b; break;
                case 0x09: rsar = (uint16_t)((rsar & 0xFF) | (b << 8)); break;
                case 0x0A: rbcr = (rbcr & 0xFF00) | b; break;
                case 0x0B: rbcr = (uint16_t)((rbcr & 0xFF) | (b << 8)); break;
                case 0x0C: rcr = b; break;
                case 0x0D: tcr = b; break;
                case 0x0E: dcr = b; break;
                case 0x0F: imr = b; break;
            }
            break;
        case 1:
            if (port >= 1 && port <= 6) par[port - 1] = b;
            else if (port == 7) curr = b;
            else if (port >= 8) mar[port - 8] = b;
            break;
        default: break;
    }
    updateIrq();
}

bool Pcmcia::accept(const uint8_t *f, size_t len) const {
    if (len < 14) return false;
    if (rcr & 0x10) return true;  // promiscuous
    bool bcast = true;
    for (int i = 0; i < 6; i++) bcast &= f[i] == 0xFF;
    if (bcast) return rcr & 0x04;
    if (f[0] & 1) {  // multicast: CRC-32 hash of the address picks a bit of MAR
        if (!(rcr & 0x08)) return false;
        uint32_t crc = 0xFFFFFFFF;
        for (int i = 0; i < 6; i++) {
            uint8_t c = f[i];
            for (int k = 0; k < 8; k++, c >>= 1) crc = (crc >> 1) ^ (((crc ^ c) & 1) ? 0xEDB88320 : 0);
        }
        // The DP8390 indexes MAR with the top 6 bits of the big-endian CRC, i.e. the reflected one.
        uint32_t rev = 0;
        for (int k = 0; k < 32; k++) if (crc & (1u << k)) rev |= 1u << (31 - k);
        uint32_t h = rev >> 26;
        return mar[h >> 3] & (1 << (h & 7));
    }
    return memcmp(f, par, 6) == 0;
}

bool Pcmcia::receive(const uint8_t *frame, size_t len) {
    if (!receiving() || !(dcr & 0x08) || (rcr & 0x20)) return true;
    if (pstart < 0x40 || pstop > 0x80 || pstart >= pstop || curr < pstart || curr >= pstop) return true;
    if (!accept(frame, len)) return true;
    uint8_t pad[60];
    if (len < 60) {  // the wire minimum
        memset(pad, 0, sizeof pad);
        memcpy(pad, frame, len);
        frame = pad;
        len = 60;
    }
    uint32_t total = (uint32_t)len + 4;
    uint32_t pages = (total + 255) / 256;
    uint32_t ring = pstop - pstart;
    uint32_t used = curr >= bnry ? curr - bnry : ring - (bnry - curr);
    if (bnry < pstart || bnry >= pstop) used = 0;
    if (pages + used >= ring) return false;  // the guest has not caught up yet
    uint32_t at = (uint32_t)curr << 8;
    uint8_t next = (uint8_t)(curr + pages);
    if (next >= pstop) next = (uint8_t)(pstart + (next - pstop));
    uint8_t hdr[4] = {(uint8_t)(0x01 | ((frame[0] & 1) ? 0x20 : 0)), next, (uint8_t)total, (uint8_t)(total >> 8)};
    auto put = [&](const uint8_t *src, uint32_t n) {
        while (n) {
            if (at >= (uint32_t)pstop << 8) at = (uint32_t)pstart << 8;
            uint32_t room = ((uint32_t)pstop << 8) - at;
            uint32_t k = n < room ? n : room;
            memcpy(&ram[at - kRamBase], src, k);
            at += k;
            src += k;
            n -= k;
        }
    };
    put(hdr, 4);
    put(frame, (uint32_t)len);
    curr = next;
    rsr = hdr[0];
    raise(PRX);
    return true;
}

}  // namespace wm
