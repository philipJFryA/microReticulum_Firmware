// LXMF.h - Lightweight Extensible Message Format
// https://github.com/markqvist/lxmf
//
// Implements the LXMF wire format for the microReticulum T-Deck UI:
//   [16-byte destination hash]
//   [16-byte source hash]
//   [64-byte Ed25519 signature]
//   [msgpack [timestamp, title, content, fields]]
//
// The message-id is a SHA-256 hash of the Destination, Source and Payload
// (never transmitted directly; always inferred from the message itself).
//
// Also provides encode/decode helpers for LXMF announce app_data:
//   msgpack [display_name, stamp_cost, [supported_functionality]]

#pragma once

#ifndef MICRORETICULUM_LXMF_H
#define MICRORETICULUM_LXMF_H

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#include <vector>
#include <map>

#include <MsgPack.h>

#include <microReticulum/Identity.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Utilities/OS.h>

// LXMF application name and delivery aspect (matching the Python reference)
#define LXMF_APP_NAME "lxmf"
#define LXMF_DELIVERY_ASPECT "delivery"

namespace LXMF {

// ---- Protocol constants (LXMessage.py / LXMF.py) -------------------------

// RNS truncated hash length is 128 bits = 16 bytes
constexpr uint8_t DESTINATION_LENGTH = 16;
// Ed25519 signature length
constexpr uint8_t SIGNATURE_LENGTH   = 64;

// Core field constants from the LXMF.py specification
constexpr uint8_t FIELD_TICKET       = 0x0C;
constexpr uint8_t SF_COMPRESSION     = 0x00;

// ---- Minimal MessagePack reader -------------------------------------------
//
// A focused decoder for the specific data structures LXMF uses:
//   * an array of 4 items (timestamp, title, content, fields)
//   * announce app_data arrays [display_name, stamp_cost, [sf...]]
//
// The bundled Arduino MsgPack Unpacker is not used here because its feed()
// bootstrap loop underflows on an empty index table.
class MsgPackReader {
public:
    MsgPackReader(const uint8_t* data, size_t size)
        : _data(data), _size(size), _pos(0) {}

    // Read an array header, returning the element count.
    bool read_array_header(size_t& count) {
        uint8_t b;
        if (!read_byte(b)) return false;
        if ((b & 0xF0) == 0x90) { count = b & 0x0F; return true; }        // fixarray
        if (b == 0xDC) { uint16_t v; if (!read_be16(v)) return false; count = v; return true; }
        if (b == 0xDD) { uint32_t v; if (!read_be32(v)) return false; count = v; return true; }
        return false;
    }

    // Read a double-precision timestamp (accepts msgpack float64 or integer).
    bool read_double(double& value) {
        uint8_t b;
        size_t save = _pos;
        if (!read_byte(b)) return false;
        if (b == 0xCB) {                                                    // float64
            uint64_t bits = 0;
            for (int i = 0; i < 8; i++) {
                uint8_t byte;
                if (!read_byte(byte)) return false;
                bits = (bits << 8) | byte;
            }
            memcpy(&value, &bits, sizeof(value));
            return true;
        }
        // LMXC payloads may carry integer encodings for the timestamp.
        _pos = save;
        int64_t iv;
        if (read_int(iv)) { value = (double)iv; return true; }
        return false;
    }

    // Read bin/str data into `out`.
    bool read_bin(std::vector<uint8_t>& out) {
        uint8_t b;
        size_t save = _pos;
        if (!read_byte(b)) return false;
        size_t len;
        if (b == 0xC4) {                                                    // bin8
            uint8_t l;
            if (!read_byte(l)) return false;
            len = l;
        } else if (b == 0xC5) {                                             // bin16
            uint16_t l;
            if (!read_be16(l)) return false;
            len = l;
        } else if (b == 0xC6) {                                             // bin32
            uint32_t l;
            if (!read_be32(l)) return false;
            len = l;
        } else {
            // Not a bin - rewind and try str encoding.
            _pos = save;
            return read_str(out);
        }
        if (_pos + len > _size) return false;
        out.assign(_data + _pos, _data + _pos + len);
        _pos += len;
        return true;
    }

    // Read str data into `out`.
    bool read_str(std::vector<uint8_t>& out) {
        uint8_t b;
        if (!read_byte(b)) return false;
        size_t len;
        if ((b & 0xE0) == 0xA0) {                                           // fixstr
            len = b & 0x1F;
        } else if (b == 0xD9) {                                             // str8
            uint8_t l;
            if (!read_byte(l)) return false;
            len = l;
        } else if (b == 0xDA) {                                             // str16
            uint16_t l;
            if (!read_be16(l)) return false;
            len = l;
        } else if (b == 0xDB) {                                             // str32
            uint32_t l;
            if (!read_be32(l)) return false;
            len = l;
        } else {
            return false;
        }
        if (_pos + len > _size) return false;
        out.assign(_data + _pos, _data + _pos + len);
        _pos += len;
        return true;
    }

    // Skip any single msgpack object.
    bool skip_object() {
        uint8_t b;
        if (!read_byte(b)) return false;

        if ((b & 0x80) == 0x00) return true;                                // positive fixint
        if ((b & 0xE0) == 0xA0) { _pos += b & 0x1F; return true; }          // fixstr
        if ((b & 0xF0) == 0x90) {                                           // fixarray
            size_t count = b & 0x0F;
            for (size_t i = 0; i < count; i++) if (!skip_object()) return false;
            return true;
        }
        if ((b & 0xF0) == 0x80) {                                           // fixmap
            size_t count = b & 0x0F;
            for (size_t i = 0; i < count * 2; i++) if (!skip_object()) return false;
            return true;
        }
        if ((b & 0xE0) == 0xE0) return true;                                // negative fixint

        switch (b) {
            case 0xC0: return true;                                         // nil
            case 0xC2:
            case 0xC3: return true;                                         // bool
            case 0xC4: { uint8_t l; if (!read_byte(l)) return false; _pos += l; return true; }    // bin8
            case 0xC5: { uint16_t l; if (!read_be16(l)) return false; _pos += l; return true; }   // bin16
            case 0xC6: { uint32_t l; if (!read_be32(l)) return false; _pos += l; return true; }   // bin32
            case 0xCA: _pos += 4; return true;                              // float32
            case 0xCB: _pos += 8; return true;                              // float64
            case 0xCC: _pos += 1; return true;                              // uint8
            case 0xCD: _pos += 2; return true;                              // uint16
            case 0xCE: _pos += 4; return true;                              // uint32
            case 0xCF: _pos += 8; return true;                              // uint64
            case 0xD0: _pos += 1; return true;                              // int8
            case 0xD1: _pos += 2; return true;                              // int16
            case 0xD2: _pos += 4; return true;                              // int32
            case 0xD3: _pos += 8; return true;                              // int64
            case 0xD9: { uint8_t l; if (!read_byte(l)) return false; _pos += l; return true; }    // str8
            case 0xDA: { uint16_t l; if (!read_be16(l)) return false; _pos += l; return true; }   // str16
            case 0xDB: { uint32_t l; if (!read_be32(l)) return false; _pos += l; return true; }   // str32
            case 0xDC: {                                                    // array16
                uint16_t count;
                if (!read_be16(count)) return false;
                for (uint16_t i = 0; i < count; i++) if (!skip_object()) return false;
                return true;
            }
            case 0xDD: {                                                    // array32
                uint32_t count;
                if (!read_be32(count)) return false;
                for (uint32_t i = 0; i < count; i++) if (!skip_object()) return false;
                return true;
            }
            case 0xDE: {                                                    // map16
                uint16_t count;
                if (!read_be16(count)) return false;
                for (uint16_t i = 0; i < count * 2; i++) if (!skip_object()) return false;
                return true;
            }
            case 0xDF: {                                                    // map32
                uint32_t count;
                if (!read_be32(count)) return false;
                for (uint32_t i = 0; i < count * 2; i++) if (!skip_object()) return false;
                return true;
            }
            default:
                // fixext family (1, 2, 4, 8, 16 bytes payload)
                if (b >= 0xD4 && b <= 0xD8) {
                    uint8_t size = (uint8_t)(1u << (b - 0xD4));
                    uint8_t type;
                    if (!read_byte(type)) return false;
                    _pos += size;
                    return true;
                }
                if (b == 0xC7) {                                            // ext8
                    uint8_t len;
                    if (!read_byte(len)) return false;
                    uint8_t type;
                    if (!read_byte(type)) return false;
                    _pos += len;
                    return true;
                }
                if (b == 0xC8) {                                            // ext16
                    uint16_t len;
                    if (!read_be16(len)) return false;
                    uint8_t type;
                    if (!read_byte(type)) return false;
                    _pos += len;
                    return true;
                }
                if (b == 0xC9) {                                            // ext32
                    uint32_t len;
                    if (!read_be32(len)) return false;
                    uint8_t type;
                    if (!read_byte(type)) return false;
                    _pos += len;
                    return true;
                }
                return false;
        }
    }

    // Read an integer (signed or unsigned encodings).
    bool read_int(int64_t& value) {
        uint8_t b;
        if (!read_byte(b)) return false;
        if ((b & 0x80) == 0x00) { value = b; return true; }                 // positive fixint
        if ((b & 0xE0) == 0xE0) { value = (int8_t)b; return true; }         // negative fixint
        switch (b) {
            case 0xCC: { uint8_t v; if (!read_byte(v)) return false; value = v; return true; }
            case 0xCD: { uint16_t v; if (!read_be16(v)) return false; value = v; return true; }
            case 0xCE: { uint32_t v; if (!read_be32(v)) return false; value = v; return true; }
            case 0xCF: { uint64_t v; if (!read_be64(v)) return false; value = (int64_t)v; return true; }
            case 0xD0: { uint8_t v; if (!read_byte(v)) return false; value = (int8_t)v; return true; }
            case 0xD1: { uint16_t v; if (!read_be16(v)) return false; value = (int16_t)v; return true; }
            case 0xD2: { uint32_t v; if (!read_be32(v)) return false; value = (int32_t)v; return true; }
            case 0xD3: { uint64_t v; if (!read_be64(v)) return false; value = (int64_t)v; return true; }
            default: return false;
        }
    }

    size_t position() const { return _pos; }

private:
    bool read_byte(uint8_t& b) {
        if (_pos >= _size) return false;
        b = _data[_pos++];
        return true;
    }
    bool read_be16(uint16_t& v) {
        uint8_t hi, lo;
        if (!read_byte(hi) || !read_byte(lo)) return false;
        v = ((uint16_t)hi << 8) | lo;
        return true;
    }
    bool read_be32(uint32_t& v) {
        uint8_t a, b, c, d;
        if (!read_byte(a) || !read_byte(b) || !read_byte(c) || !read_byte(d)) return false;
        v = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
        return true;
    }
    bool read_be64(uint64_t& v) {
        uint32_t hi, lo;
        if (!read_be32(hi) || !read_be32(lo)) return false;
        v = ((uint64_t)hi << 32) | lo;
        return true;
    }

    const uint8_t* _data;
    size_t _size;
    size_t _pos;
};

// ---- LXMF message ---------------------------------------------------------

struct LXMessage {
    RNS::Bytes destination_hash;
    RNS::Bytes source_hash;
    RNS::Bytes signature;
    RNS::Bytes hash;                    // SHA-256(dest + source + payload)
    double      timestamp = 0.0;
    RNS::Bytes title;
    RNS::Bytes content;
    RNS::Bytes packed;
    bool        valid = false;

    // Pack an LXMF message with the given content (title and fields empty).
    //
    // Wire layout produced:
    //   dest_hash(16) || source_hash(16) || signature(64) ||
    //   msgpack([timestamp, title, content, fields])
    //
    // signature = Ed25519(signing_key,
    //     dest_hash || source_hash || msgpack_payload ||
    //     SHA-256(dest_hash || source_hash || msgpack_payload))
    static bool pack(RNS::Bytes& out,
                     const RNS::Bytes& destination_hash,
                     const RNS::Bytes& source_hash,
                     const RNS::Identity& source_identity,
                     const char* content,
                     double timestamp = -1.0) {
        if (destination_hash.size() != DESTINATION_LENGTH ||
            source_hash.size() != DESTINATION_LENGTH ||
            !source_identity) return false;
        if (!content) content = "";
        if (timestamp < 0) timestamp = RNS::Utilities::OS::time();

        // Build the msgpack payload: [timestamp, title, content, fields].
        // Title and fields are empty (mandatory parts of the structure).
        arduino::msgpack::Packer packer;
        packer.reserve_buffer(256);
        std::vector<uint8_t> title;                  // empty title bytes
        std::vector<uint8_t> content_vec(content, content + strlen(content));
        std::map<uint8_t, uint8_t> fields;           // empty fields map

        packer.to_array(timestamp, title, content_vec, fields);
        const uint8_t* payload = packer.data();
        size_t payload_len = packer.size();

        // hashed_part = dest_hash + source_hash + payload
        RNS::Bytes hashed_part;
        hashed_part.append(destination_hash);
        hashed_part.append(source_hash);
        hashed_part.append(payload, payload_len);

        // message_id / message_hash = SHA-256(hashed_part)
        RNS::Bytes message_hash = RNS::Identity::full_hash(hashed_part);

        // signed_part = hashed_part + message_hash
        RNS::Bytes signed_part(hashed_part);
        signed_part.append(message_hash);

        // signature = Ed25519(signing_key, signed_part)
        RNS::Bytes signature = source_identity.sign(signed_part);
        if (signature.size() != SIGNATURE_LENGTH) return false;

        // packed = dest_hash + source_hash + signature + payload
        RNS::Bytes packed;
        packed.append(destination_hash);
        packed.append(source_hash);
        packed.append(signature);
        packed.append(payload, payload_len);

        out = packed;
        return true;
    }

    // Unpack an LXMF message (extracts fields; does not verify signature).
    static bool unpack(LXMessage& msg, const RNS::Bytes& data) {
        if (data.size() < 2*DESTINATION_LENGTH + SIGNATURE_LENGTH + 2) return false;

        msg.destination_hash = data.left(DESTINATION_LENGTH);
        msg.source_hash      = data.mid(DESTINATION_LENGTH, DESTINATION_LENGTH);
        msg.signature        = data.mid(2*DESTINATION_LENGTH, SIGNATURE_LENGTH);
        size_t payload_offset = 2*DESTINATION_LENGTH + SIGNATURE_LENGTH;
        const uint8_t* payload = data.data() + payload_offset;
        size_t payload_len = data.size() - payload_offset;

        MsgPackReader reader(payload, payload_len);
        size_t count = 0;
        if (!reader.read_array_header(count) || count < 4) return false;

        if (!reader.read_double(msg.timestamp)) return false;

        std::vector<uint8_t> title_vec;
        if (!reader.read_bin(title_vec)) return false;
        if (!title_vec.empty()) msg.title.assign(title_vec.data(), title_vec.size());

        std::vector<uint8_t> content_vec;
        if (!reader.read_bin(content_vec)) return false;
        if (!content_vec.empty()) msg.content.assign(content_vec.data(), content_vec.size());

        // Fields (may be an empty map or a full dictionary) - skip.
        if (!reader.skip_object()) return false;

        // message_id = SHA-256(dest_hash + source_hash + packed_payload)
        RNS::Bytes hashed_part;
        hashed_part.append(msg.destination_hash);
        hashed_part.append(msg.source_hash);
        hashed_part.append(payload, payload_len);
        msg.hash = RNS::Identity::full_hash(hashed_part);

        msg.packed = data;
        msg.valid = true;
        return true;
    }

    // Verify the Ed25519 signature using the source identity.
    static bool verify(const LXMessage& msg, const RNS::Identity& source_identity) {
        if (!msg.valid || !source_identity) return false;

        size_t payload_offset = 2*DESTINATION_LENGTH + SIGNATURE_LENGTH;
        const uint8_t* payload = msg.packed.data() + payload_offset;
        size_t payload_len = msg.packed.size() - payload_offset;

        RNS::Bytes hashed_part;
        hashed_part.append(msg.destination_hash);
        hashed_part.append(msg.source_hash);
        hashed_part.append(payload, payload_len);

        RNS::Bytes signed_part(hashed_part);
        signed_part.append(msg.hash);

        return source_identity.validate(msg.signature, signed_part);
    }
};

// ---- LXMF announce app_data helpers ---------------------------------------

// Decode the display name from LXMF announce app_data:
//   msgpack [display_name(bin/str), stamp_cost(nil/int), [SF_*]]
// Returns true and fills `name_out` if the data is a valid LXMF announce
// array with a display-name first element (even an empty one).
inline bool announce_display_name(const uint8_t* data, size_t size,
                                  std::vector<uint8_t>& name_out) {
    if (!data || size < 2) return false;
    MsgPackReader reader(data, size);
    size_t count = 0;
    if (!reader.read_array_header(count) || count < 1) return false;

    std::vector<uint8_t> name;
    if (!reader.read_bin(name)) return false;
    name_out.swap(name);
    return true;
}

} // namespace LXMF

#endif // MICRORETICULUM_LXMF_H