// test_protocol.cpp -- the codec must survive anything that arrives at the
// antenna. A frame arriving proves only that something transmitted it, so every
// one of these cases is a real frame someone will send us on tier 3.
//
// No test framework: a main() that returns non-zero is enough and vendoring one
// costs time we do not have.

#include "protocol.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace sw;

static void TestHeaderRoundTrip() {
    std::printf("header round-trip\n");
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));

    Header out;
    out.type = MsgType::TrackReport;
    out.origin = 7;
    out.hops = 2;
    out.seq = 40000;
    out.sent_time = 123.456f;
    out.Write(w);

    CHECK(w.ok());
    CHECK(w.size() == Header::kBytes);

    Reader r(buf, w.size());
    Header in;
    CHECK(in.Read(r));
    CHECK(in.version == kProtocolVersion);
    CHECK(in.type == MsgType::TrackReport);
    CHECK(in.origin == 7);
    CHECK(in.hops == 2);
    CHECK(in.seq == 40000);
    CHECK(in.sent_time > 123.4f && in.sent_time < 123.5f);
}

static void TestTruncated() {
    std::printf("truncated frame is rejected\n");
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    Header h;
    h.origin = 3;
    h.Write(w);

    // Every prefix shorter than a full header must be refused, not guessed at.
    for (uint32_t len = 0; len < Header::kBytes; ++len) {
        Reader r(buf, len);
        Header in;
        CHECK(!in.Read(r));
    }
}

static void TestWrongVersion() {
    std::printf("wrong version is rejected\n");
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    Header h;
    h.Write(w);
    buf[0] = kProtocolVersion + 1;

    Reader r(buf, w.size());
    Header in;
    CHECK(!in.Read(r));
}

static void TestUnknownType() {
    std::printf("unknown message type is rejected\n");
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    Header h;
    h.Write(w);
    buf[1] = 99;

    Reader r(buf, w.size());
    Header in;
    CHECK(!in.Read(r));
}

static void TestGarbage() {
    std::printf("garbage bytes never read out of bounds\n");
    // Tier 3 hostiles transmit noise. Nothing here may crash or read past len.
    uint8_t buf[32];
    for (uint32_t seed = 0; seed < 256; ++seed) {
        for (uint32_t i = 0; i < sizeof(buf); ++i)
            buf[i] = static_cast<uint8_t>((seed * 31 + i * 17) & 0xFF);

        for (uint32_t len = 0; len <= sizeof(buf); ++len) {
            Reader r(buf, len);
            Header h;
            if (!h.Read(r)) continue;
            TrackReportMsg m;
            m.Read(r);
            // Whatever came out, r.ok() is the only thing that decides whether
            // we may act on it.
            CHECK(m.belief <= Belief::Wreckage);
        }
    }
}

static void TestPayloadRoundTrip() {
    std::printf("payload round-trip within quantisation error\n");
    uint8_t buf[128];
    Writer w(buf, sizeof(buf));

    TrackReportMsg out;
    out.position = Vec3(123.5f, -87.25f, -42.0f);
    out.velocity = Vec3(12.0f, -3.5f, 0.25f);
    out.belief = Belief::Hostile;
    out.confidence = 200;
    out.Write(w);
    CHECK(w.ok());

    Reader r(buf, w.size());
    TrackReportMsg in;
    in.Read(r);
    CHECK(r.ok());

    // 0.125 m per-axis quantisation step, truncated not rounded, so the 3D
    // error is up to sqrt(3)*0.125 = 0.217 m. Measured fix_sigma is 0.35 m.
    CHECK(swarm::Distance(in.position, out.position) < 0.3f);
    CHECK(swarm::Distance(in.velocity, out.velocity) < 0.3f);
    CHECK(in.belief == Belief::Hostile);
    CHECK(in.confidence == 200);
}

static void TestWriterOverflow() {
    std::printf("writer refuses to overflow\n");
    uint8_t small[4];
    Writer w(small, sizeof(small));
    Header h;
    h.Write(w);
    CHECK(!w.ok());
    CHECK(w.size() <= sizeof(small));
}

static void TestSeenSet() {
    std::printf("seen-set does not confuse empty with (origin 0, seq 0)\n");
    SeenSet<8> seen;

    // The example brain's ring starts full of zeroes and therefore believes it
    // has already relayed (origin 0, seq 0). This must not.
    CHECK(!seen.Seen(0, 0));
    CHECK(!seen.SeenAndMark(0, 0));
    CHECK(seen.Seen(0, 0));

    CHECK(!seen.SeenAndMark(1, 5));
    CHECK(seen.Seen(1, 5));
    CHECK(!seen.Seen(1, 6));
}

static void TestOutboxPriority() {
    std::printf("outbox drops the least important, not the newest\n");
    Outbox<3> box;
    uint8_t payload[8]{};

    CHECK(box.Push(payload, sizeof(payload), 1, 0.0f));   // heartbeat
    CHECK(box.Push(payload, sizeof(payload), 1, 0.1f));   // heartbeat
    CHECK(box.Push(payload, sizeof(payload), 1, 0.2f));   // heartbeat
    CHECK(box.size() == 3);

    // A hostile detection arriving at a full queue must displace a heartbeat.
    CHECK(box.Push(payload, sizeof(payload), 5, 0.3f));
    CHECK(box.size() == 3);

    const int32_t best = box.Best();
    CHECK(best >= 0);
    CHECK(box.At(static_cast<uint32_t>(best)).priority == 5);

    // Another heartbeat must NOT displace anything now.
    Outbox<1> tiny;
    CHECK(tiny.Push(payload, sizeof(payload), 5, 0.0f));
    CHECK(!tiny.Push(payload, sizeof(payload), 1, 0.1f));
}

static void TestOutboxExpiry() {
    std::printf("stale frames are dropped rather than sent late\n");
    Outbox<4> box;
    uint8_t payload[8]{};
    box.Push(payload, sizeof(payload), 1, 0.0f);
    box.Push(payload, sizeof(payload), 1, 5.0f);
    box.Expire(6.0f, 2.0f);
    CHECK(box.size() == 1);
}

static void TestRelayCopy() {
    std::printf("relay copy stamps hops and keeps origin/seq\n");
    uint8_t src[64];
    Writer w(src, sizeof(src));
    Header out;
    out.type = MsgType::TrackReport;
    out.origin = 9;
    out.hops = 0;
    out.seq = 77;
    out.sent_time = 12.5f;
    out.Write(w);
    TrackReportMsg m;
    m.position = Vec3(100, 20, -40);
    m.velocity = Vec3(-13, 0, 0);
    m.belief = Belief::Hostile;
    m.confidence = 200;
    m.Write(w);
    CHECK(w.ok());

    uint8_t dst[64];
    CHECK(RelayCopy(src, w.size(), dst, sizeof(dst), 1));
    CHECK(dst[Header::kHopsOffset] == 1);

    Reader r(dst, w.size());
    Header in;
    CHECK(in.Read(r));
    CHECK(in.origin == 9);
    CHECK(in.seq == 77);
    CHECK(in.hops == 1);
    CHECK(in.type == MsgType::TrackReport);

    // Cap: a frame already at kMaxHops is not copied by the caller; the
    // helper still stamps whatever hop count it is given.
    CHECK(RelayCopy(src, w.size(), dst, sizeof(dst), kMaxHops));
    Reader r2(dst, w.size());
    Header in2;
    CHECK(in2.Read(r2));
    CHECK(in2.hops == kMaxHops);

    uint8_t tiny[4];
    CHECK(!RelayCopy(src, w.size(), tiny, sizeof(tiny), 1));
    CHECK(!RelayCopy(src, Header::kBytes - 1, dst, sizeof(dst), 1));
}

int main() {
    TestHeaderRoundTrip();
    TestTruncated();
    TestWrongVersion();
    TestUnknownType();
    TestGarbage();
    TestPayloadRoundTrip();
    TestWriterOverflow();
    TestSeenSet();
    TestOutboxPriority();
    TestOutboxExpiry();
    TestRelayCopy();

    if (g_failures == 0) {
        std::printf("protocol: all passed\n");
        return 0;
    }
    std::printf("protocol: %d failure(s)\n", g_failures);
    return 1;
}
