#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ps2x::iop::sif
{
    inline constexpr uint32_t ChangeAddress = 0x80000000u;
    inline constexpr uint32_t SetSreg = 0x80000001u;
    inline constexpr uint32_t Init = 0x80000002u;
    inline constexpr uint32_t Reset = 0x80000003u;
    inline constexpr uint32_t FileIoComplete = 0x80000011u; // IOP -> EE, option = completion slot
    inline constexpr uint32_t End = 0x80000008u;
    inline constexpr uint32_t Bind = 0x80000009u;
    inline constexpr uint32_t Call = 0x8000000au;
    inline constexpr uint32_t OtherData = 0x8000000cu;

    // SMFLAG (SIF register 4) bits the IOP raises while booting. Values match PS2SDK sifdma.h.
    inline constexpr uint32_t StatSifInit = 0x10000u;
    inline constexpr uint32_t StatCmdInit = 0x20000u;
    inline constexpr uint32_t StatBootEnd = 0x40000u;
    // EE cycles between accepting a reset command and the rebooted IOP raising its boot flags
    // (10 ms at 294.912 MHz). The SDK clears the same flags right after sending the command, so
    // the boot must never complete inside the sceSifSetDma call that delivers it.
    inline constexpr uint64_t RebootLatencyEeCycles = 2949120u;

    enum class ParseError { None, Size, UnknownCommand, Options, Payload };
    // Wire addresses are always PS2 32-bit values, never native pointers.
    struct Packet
    {
        std::array<uint32_t, 16> words{};
        uint32_t size = 0;
        uint32_t extraSize() const { return words[0] >> 8; }
        uint32_t command() const { return words[2]; }
    };

    // Pure structural validation. Memory ownership and transaction state are
    // separate adapter obligations. RPC packets in the audited SDK are64 bytes.
    inline ParseError parse(std::span<const uint8_t> bytes, Packet &out)
    {
        if (bytes.size() < 16 || bytes.size() > 64 || (bytes.size() & 3))
            return ParseError::Size;
        Packet packet{};
        packet.size = static_cast<uint32_t>(bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i)
            packet.words[i / 4] |= uint32_t(bytes[i]) << (8 * (i % 4));
        if ((packet.words[0] & 255) != bytes.size())
            return ParseError::Size;
        uint32_t expected = 0;
        switch (packet.command())
        {
        case Init:
            if (packet.words[3] > 1) return ParseError::Options;
            expected = packet.words[3] ? 16 : 20;
            break;
        case ChangeAddress: expected = 20; break;
        case SetSreg: expected = 24; break;
        case FileIoComplete: expected = 16; break;
        case End: case Bind: case Call: case OtherData: expected = 64; break;
        default: return ParseError::UnknownCommand;
        }
        if (packet.size != expected) return ParseError::Size;
        if (packet.command() != Call && packet.command() != End && packet.extraSize())
            return ParseError::Payload;
        if (!packet.extraSize() && packet.words[1]) return ParseError::Payload;
        if ((packet.command() == Init && packet.words[3] == 0) || packet.command() == ChangeAddress)
            if (!packet.words[4] || (packet.words[4] & 15)) return ParseError::Payload;
        if (packet.command() == SetSreg && packet.words[4] >= 32) return ParseError::Payload;
        if (packet.command() == Call &&
            (packet.words[9] > 0x7fffffffu || packet.words[11] > 0x7fffffffu || packet.words[12] > 1))
            return ParseError::Payload;
        if (packet.command() == OtherData && packet.words[10] > 0x7fffffffu)
            return ParseError::Payload;
        out = packet;
        return ParseError::None;
    }

    // SifCmdResetData_t (PS2SDK sifcmd-common.h): 16 byte header, arglen, mode and an 80 byte
    // argument buffer, 104 bytes in total. The SDK never initialises the header dsize, so only
    // psize (the first byte) is validated. The IOP honours exactly arglen characters.
    struct ResetPacket
    {
        static constexpr uint32_t WireSize = 104;
        static constexpr uint32_t ArgMax = 79;
        uint32_t arglen = 0;
        uint32_t mode = 0;
        std::array<char, 80> arg{};
        std::string argument() const { return std::string(arg.data(), arglen); }
    };

    inline uint32_t wordAt(std::span<const uint8_t> bytes, size_t offset)
    {
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
               (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
    }

    inline ParseError parseReset(std::span<const uint8_t> bytes, ResetPacket &out)
    {
        if (bytes.size() != ResetPacket::WireSize || bytes[0] != ResetPacket::WireSize)
            return ParseError::Size;
        if (wordAt(bytes, 8) != Reset)
            return ParseError::UnknownCommand;
        ResetPacket packet{};
        packet.arglen = wordAt(bytes, 16);
        packet.mode = wordAt(bytes, 20);
        if (packet.arglen > ResetPacket::ArgMax)
            return ParseError::Payload;
        for (uint32_t i = 0; i < packet.arglen; ++i)
            packet.arg[i] = static_cast<char>(bytes[24 + i]);
        out = packet;
        return ParseError::None;
    }

    inline std::array<uint8_t,64> encode(const Packet &packet)
    {
        std::array<uint8_t,64> bytes{};
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<uint8_t>(packet.words[i / 4] >> (8 * (i % 4)));
        return bytes;
    }
}
