// protocol.h -- the wire format. Knows nothing about tactics or flight.
//
// Versioned from the first byte, explicitly little-endian, fixed width. The
// example brain's packed-struct memcpy is called out in its own comments as the
// crudest thing that works; this is the shape of the thing that does not break
// when you add a field.
#ifndef SWARM_PROTOCOL_H
#define SWARM_PROTOCOL_H

#include "world.h"

namespace sw {

constexpr uint8_t kProtocolVersion = 1;

enum class MsgType : uint8_t {
    Heartbeat = 1,   // I am alive, here, at this time
    TrackReport = 2, // I see something, here, and I think it is this
    Claim = 3,       // I am committing to that thing
    Accuse = 4,      // I believe this key belongs to an insider (tier 5)
};

// ---------------------------------------------------------------------------
// Bounds-checked, endian-defined serialisation.
//
// Explicit shifts rather than memcpy of a struct: the byte order is then a
// property of this code and not of the machine that compiled it.
// ---------------------------------------------------------------------------

class Writer {
public:
    Writer(uint8_t* buf, uint32_t cap) : buf_(buf), cap_(cap) {}

    bool ok() const { return ok_; }
    uint32_t size() const { return pos_; }

    void U8(uint8_t v) {
        if (!Room(1)) return;
        buf_[pos_++] = v;
    }
    void U16(uint16_t v) {
        if (!Room(2)) return;
        buf_[pos_++] = static_cast<uint8_t>(v);
        buf_[pos_++] = static_cast<uint8_t>(v >> 8);
    }
    void U32(uint32_t v) {
        if (!Room(4)) return;
        for (int i = 0; i < 4; ++i) buf_[pos_++] = static_cast<uint8_t>(v >> (8 * i));
    }
    void F32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        U32(bits);
    }
    void Vec(const Vec3& v) { F32(v.x); F32(v.y); F32(v.z); }

    /// Positions quantised to 0.125 m over a +/-4095 m range: 2 bytes per axis
    /// instead of 4. Bandwidth is the binding constraint from s2 onward.
    /// This truncates rather than rounds, so the per-axis error is up to the
    /// full 0.125 m and one-signed. Measured fix_sigma on s1 is 0.35 m
    /// (--dump-params sense.fix_sigma), so the error is a third of the fix
    /// noise -- below it, but not negligibly so. The +/-4095 m range is 20x
    /// wider than the +/-200 m arena, which is two wasted bits per axis.
    void PosQ(const Vec3& v) {
        Q16(v.x); Q16(v.y); Q16(v.z);
    }

private:
    void Q16(float v) {
        float clamped = v < -4095.0f ? -4095.0f : (v > 4095.0f ? 4095.0f : v);
        U16(static_cast<uint16_t>(static_cast<int32_t>(clamped * 8.0f) + 32768));
    }
    bool Room(uint32_t n) {
        if (pos_ + n > cap_) { ok_ = false; return false; }
        return true;
    }

    uint8_t* buf_;
    uint32_t cap_;
    uint32_t pos_ = 0;
    bool ok_ = true;
};

class Reader {
public:
    Reader(const uint8_t* buf, uint32_t len) : buf_(buf), len_(len) {}

    /// False once anything has run off the end. Check it before trusting any
    /// value you read -- a truncated or hostile frame must never be acted on.
    bool ok() const { return ok_; }
    uint32_t remaining() const { return ok_ ? len_ - pos_ : 0; }

    uint8_t U8() {
        if (!Room(1)) return 0;
        return buf_[pos_++];
    }
    uint16_t U16() {
        if (!Room(2)) return 0;
        uint16_t v = static_cast<uint16_t>(buf_[pos_] | (buf_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    uint32_t U32() {
        if (!Room(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(buf_[pos_ + i]) << (8 * i);
        pos_ += 4;
        return v;
    }
    float F32() {
        uint32_t bits = U32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    Vec3 Vec() { float x = F32(), y = F32(), z = F32(); return Vec3(x, y, z); }
    Vec3 PosQ() { float x = DQ16(), y = DQ16(), z = DQ16(); return Vec3(x, y, z); }

private:
    float DQ16() {
        return (static_cast<float>(static_cast<int32_t>(U16()) - 32768)) * 0.125f;
    }
    bool Room(uint32_t n) {
        if (!ok_ || pos_ + n > len_) { ok_ = false; return false; }
        return true;
    }

    const uint8_t* buf_;
    uint32_t len_;
    uint32_t pos_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------
// Common header on every frame.
// ---------------------------------------------------------------------------

struct Header {
    uint8_t version = kProtocolVersion;
    MsgType type = MsgType::Heartbeat;
    uint8_t origin = 0;    // drone_id that authored this, not who relayed it
    uint8_t hops = 0;
    uint16_t seq = 0;      // per origin
    float sent_time = 0.0f;

    static constexpr uint32_t kBytes = 10;

    void Write(Writer& w) const {
        w.U8(version);
        w.U8(static_cast<uint8_t>(type));
        w.U8(origin);
        w.U8(hops);
        w.U16(seq);
        w.F32(sent_time);
    }

    /// False for anything we will not act on: wrong version, unknown type, or a
    /// frame too short to contain a header at all. A frame arriving at your
    /// antenna proves only that something transmitted it.
    bool Read(Reader& r) {
        version = r.U8();
        uint8_t t = r.U8();
        origin = r.U8();
        hops = r.U8();
        seq = r.U16();
        sent_time = r.F32();
        if (!r.ok()) return false;
        if (version != kProtocolVersion) return false;
        if (t < 1 || t > 4) return false;
        type = static_cast<MsgType>(t);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Payloads
// ---------------------------------------------------------------------------

struct HeartbeatMsg {
    Vec3 position{};
    Vec3 velocity{};

    void Write(Writer& w) const { w.PosQ(position); w.PosQ(velocity); }
    void Read(Reader& r) { position = r.PosQ(); velocity = r.PosQ(); }
};

struct TrackReportMsg {
    Vec3 position{};
    Vec3 velocity{};
    Belief belief = Belief::Unknown;
    uint8_t confidence = 0;   // 0..255

    /// No track_id. It is observer-local and means nothing to the receiver --
    /// the whole point. The receiver associates by geometry and time.
    void Write(Writer& w) const {
        w.PosQ(position);
        w.PosQ(velocity);
        w.U8(static_cast<uint8_t>(belief));
        w.U8(confidence);
    }
    void Read(Reader& r) {
        position = r.PosQ();
        velocity = r.PosQ();
        uint8_t b = r.U8();
        belief = (b <= 4) ? static_cast<Belief>(b) : Belief::Unknown;
        confidence = r.U8();
    }
};

struct ClaimMsg {
    Vec3 target_position{};
    float expires_at = 0.0f;  // absolute sim time; claims TIME OUT, never ack

    void Write(Writer& w) const { w.PosQ(target_position); w.F32(expires_at); }
    void Read(Reader& r) { target_position = r.PosQ(); expires_at = r.F32(); }
};

// ---------------------------------------------------------------------------
// Duplicate suppression.
//
// The example's ring buffer starts full of zeroes, which collides with
// (origin 0, seq 0) -- its own comments admit it. This one carries an explicit
// occupied flag, so an empty slot is never mistaken for a real message.
// ---------------------------------------------------------------------------

template <uint32_t N>
class SeenSet {
public:
    bool Seen(uint8_t origin, uint16_t seq) const {
        const uint32_t key = Key(origin, seq);
        for (uint32_t i = 0; i < N; ++i)
            if (occupied_[i] && keys_[i] == key) return true;
        return false;
    }

    void Mark(uint8_t origin, uint16_t seq) {
        keys_[head_] = Key(origin, seq);
        occupied_[head_] = true;
        head_ = (head_ + 1) % N;
    }

    /// Test then mark, since that is the only way it is ever used.
    bool SeenAndMark(uint8_t origin, uint16_t seq) {
        if (Seen(origin, seq)) return true;
        Mark(origin, seq);
        return false;
    }

private:
    static uint32_t Key(uint8_t origin, uint16_t seq) {
        return (static_cast<uint32_t>(origin) << 16) | seq;
    }

    uint32_t keys_[N]{};
    bool occupied_[N]{};
    uint32_t head_ = 0;
};

// ---------------------------------------------------------------------------
// Outbound queue. One broadcast per tick, so almost everything queues.
// ---------------------------------------------------------------------------

struct OutFrame {
    uint8_t bytes[SW_MTU]{};
    uint32_t len = 0;
    uint8_t priority = 0;    // higher goes first
    float queued_at = 0.0f;
};

/// Priority queue, not FIFO: a new hostile detection outranks a routine
/// heartbeat, and under budget pressure the heartbeat is what should be lost.
template <uint32_t N>
class Outbox {
public:
    bool Push(const uint8_t* data, uint32_t len, uint8_t priority, float now) {
        if (len > SW_MTU) return false;
        if (queue_.full()) {
            // Drop the lowest-priority resident rather than the newest arrival.
            uint32_t worst = 0;
            for (uint32_t i = 1; i < queue_.size() && i < N; ++i)
                if (queue_[i].priority < queue_[worst].priority) worst = i;
            if (queue_[worst].priority >= priority) return false;
            queue_.erase(worst);
        }
        OutFrame* f = queue_.emplace();
        if (!f) return false;
        std::memcpy(f->bytes, data, len);
        f->len = len;
        f->priority = priority;
        f->queued_at = now;
        return true;
    }

    /// Highest priority, oldest first within a priority. A total order, so the
    /// choice never depends on iteration accident.
    int32_t Best() const {
        if (queue_.empty()) return -1;
        uint32_t best = 0;
        for (uint32_t i = 1; i < queue_.size() && i < N; ++i) {
            if (queue_[i].priority > queue_[best].priority ||
                (queue_[i].priority == queue_[best].priority &&
                 queue_[i].queued_at < queue_[best].queued_at)) {
                best = i;
            }
        }
        return static_cast<int32_t>(best);
    }

    const OutFrame& At(uint32_t i) const { return queue_[i]; }
    void Remove(uint32_t i) { queue_.erase(i); }
    bool empty() const { return queue_.empty(); }
    uint32_t size() const { return queue_.size(); }

    /// Anything that has waited too long is no longer worth saying.
    void Expire(float now, float max_age) {
        for (uint32_t i = queue_.size(); i > 0; --i) {
            if (now - queue_[i - 1].queued_at > max_age) queue_.erase(i - 1);
        }
    }

private:
    FixedVec<OutFrame, N> queue_;
};

}  // namespace sw

#endif  // SWARM_PROTOCOL_H
