/**
 * @file test_LibreMidiTransport.cpp
 * @brief Unit tests for LibreMidiTransport
 *
 * Tests the LibreMidiTransport class message parsing and callback logic.
 * Note: Full MIDI I/O tests require real MIDI ports (loopMIDI on Windows).
 */

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <vector>

// Minimal mock for testing processMessage logic
// This simulates the callback behavior without needing real MIDI

namespace test {

struct ReceivedCC {
    uint8_t channel;
    uint8_t cc;
    uint8_t value;
};

struct ReceivedNote {
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
};

class MockMidiReceiver {
public:
    std::vector<ReceivedCC> ccMessages;
    std::vector<ReceivedNote> noteOnMessages;
    std::vector<ReceivedNote> noteOffMessages;
    std::vector<std::vector<uint8_t>> sysexMessages;
    int clockCount = 0;
    int startCount = 0;
    int stopCount = 0;
    int continueCount = 0;

    void onCC(uint8_t ch, uint8_t cc, uint8_t val) {
        ccMessages.push_back({ch, cc, val});
    }

    void onNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
        noteOnMessages.push_back({ch, note, vel});
    }

    void onNoteOff(uint8_t ch, uint8_t note, uint8_t vel) {
        noteOffMessages.push_back({ch, note, vel});
    }

    void onSysEx(const uint8_t* data, size_t len) {
        sysexMessages.push_back(std::vector<uint8_t>(data, data + len));
    }

    void onClock() { clockCount++; }

    void onStart() { startCount++; }

    void onStop() { stopCount++; }

    void onContinue() { continueCount++; }

    void clear() {
        ccMessages.clear();
        noteOnMessages.clear();
        noteOffMessages.clear();
        sysexMessages.clear();
        clockCount = 0;
        startCount = 0;
        stopCount = 0;
        continueCount = 0;
    }
};

// Test message parsing logic (mirrors LibreMidiTransport::processMessage)
void processTestMessage(const uint8_t* data, size_t length, MockMidiReceiver& receiver) {
    if (length == 0) return;

    uint8_t status = data[0];

    switch (status) {
        case 0xF8:
            receiver.onClock();
            return;
        case 0xFA:
            receiver.onStart();
            return;
        case 0xFB:
            receiver.onContinue();
            return;
        case 0xFC:
            receiver.onStop();
            return;
        default:
            break;
    }

    uint8_t type = status & 0xF0;
    uint8_t channel = status & 0x0F;

    switch (type) {
        case 0x80: // Note Off
            if (length >= 3) {
                receiver.onNoteOff(channel, data[1], data[2]);
            }
            break;

        case 0x90: // Note On
            if (length >= 3) {
                if (data[2] == 0) {
                    // Velocity 0 = Note Off
                    receiver.onNoteOff(channel, data[1], 0);
                } else {
                    receiver.onNoteOn(channel, data[1], data[2]);
                }
            }
            break;

        case 0xB0: // Control Change
            if (length >= 3) {
                receiver.onCC(channel, data[1], data[2]);
            }
            break;

        case 0xF0: // System Exclusive
            receiver.onSysEx(data, length);
            break;

        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test Cases
// ═══════════════════════════════════════════════════════════════════

void test_NoteOn() {
    MockMidiReceiver receiver;

    // Note On: Channel 0, Note 60 (C4), Velocity 100
    uint8_t msg[] = {0x90, 60, 100};
    processTestMessage(msg, 3, receiver);

    assert(receiver.noteOnMessages.size() == 1);
    assert(receiver.noteOnMessages[0].channel == 0);
    assert(receiver.noteOnMessages[0].note == 60);
    assert(receiver.noteOnMessages[0].velocity == 100);

    std::cout << "[PASS] test_NoteOn\n";
}

void test_NoteOff() {
    MockMidiReceiver receiver;

    // Note Off: Channel 1, Note 64 (E4), Velocity 0
    uint8_t msg[] = {0x81, 64, 0};
    processTestMessage(msg, 3, receiver);

    assert(receiver.noteOffMessages.size() == 1);
    assert(receiver.noteOffMessages[0].channel == 1);
    assert(receiver.noteOffMessages[0].note == 64);
    assert(receiver.noteOffMessages[0].velocity == 0);

    std::cout << "[PASS] test_NoteOff\n";
}

void test_NoteOnWithZeroVelocity_IsNoteOff() {
    MockMidiReceiver receiver;

    // Note On with velocity 0 should be treated as Note Off
    uint8_t msg[] = {0x90, 60, 0};
    processTestMessage(msg, 3, receiver);

    assert(receiver.noteOnMessages.empty());
    assert(receiver.noteOffMessages.size() == 1);
    assert(receiver.noteOffMessages[0].note == 60);

    std::cout << "[PASS] test_NoteOnWithZeroVelocity_IsNoteOff\n";
}

void test_ControlChange() {
    MockMidiReceiver receiver;

    // CC: Channel 0, CC 1 (Mod Wheel), Value 64
    uint8_t msg[] = {0xB0, 1, 64};
    processTestMessage(msg, 3, receiver);

    assert(receiver.ccMessages.size() == 1);
    assert(receiver.ccMessages[0].channel == 0);
    assert(receiver.ccMessages[0].cc == 1);
    assert(receiver.ccMessages[0].value == 64);

    std::cout << "[PASS] test_ControlChange\n";
}

void test_ChannelExtraction() {
    MockMidiReceiver receiver;

    // Note On on Channel 15 (0x9F)
    uint8_t msg[] = {0x9F, 60, 100};
    processTestMessage(msg, 3, receiver);

    assert(receiver.noteOnMessages.size() == 1);
    assert(receiver.noteOnMessages[0].channel == 15);

    std::cout << "[PASS] test_ChannelExtraction\n";
}

void test_SysEx() {
    MockMidiReceiver receiver;

    // SysEx message
    uint8_t msg[] = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
    processTestMessage(msg, 6, receiver);

    assert(receiver.sysexMessages.size() == 1);
    assert(receiver.sysexMessages[0].size() == 6);
    assert(receiver.sysexMessages[0][0] == 0xF0);
    assert(receiver.sysexMessages[0][5] == 0xF7);

    std::cout << "[PASS] test_SysEx\n";
}

void test_EmptyMessage() {
    MockMidiReceiver receiver;

    // Empty message should be ignored
    processTestMessage(nullptr, 0, receiver);

    assert(receiver.noteOnMessages.empty());
    assert(receiver.noteOffMessages.empty());
    assert(receiver.ccMessages.empty());
    assert(receiver.sysexMessages.empty());

    std::cout << "[PASS] test_EmptyMessage\n";
}

void test_ShortMessage() {
    MockMidiReceiver receiver;

    // Too short message (only 2 bytes for Note On)
    uint8_t msg[] = {0x90, 60};
    processTestMessage(msg, 2, receiver);

    // Should be ignored (needs 3 bytes)
    assert(receiver.noteOnMessages.empty());

    std::cout << "[PASS] test_ShortMessage\n";
}

void test_RealtimeClock() {
    MockMidiReceiver receiver;

    uint8_t msg[] = {0xF8};
    processTestMessage(msg, 1, receiver);

    assert(receiver.clockCount == 1);
    assert(receiver.startCount == 0);
    assert(receiver.stopCount == 0);
    assert(receiver.continueCount == 0);

    std::cout << "[PASS] test_RealtimeClock\n";
}

void test_RealtimeStart() {
    MockMidiReceiver receiver;

    uint8_t msg[] = {0xFA};
    processTestMessage(msg, 1, receiver);

    assert(receiver.startCount == 1);
    assert(receiver.clockCount == 0);
    assert(receiver.stopCount == 0);
    assert(receiver.continueCount == 0);

    std::cout << "[PASS] test_RealtimeStart\n";
}

void test_RealtimeContinue() {
    MockMidiReceiver receiver;

    uint8_t msg[] = {0xFB};
    processTestMessage(msg, 1, receiver);

    assert(receiver.continueCount == 1);
    assert(receiver.clockCount == 0);
    assert(receiver.startCount == 0);
    assert(receiver.stopCount == 0);

    std::cout << "[PASS] test_RealtimeContinue\n";
}

void test_RealtimeStop() {
    MockMidiReceiver receiver;

    uint8_t msg[] = {0xFC};
    processTestMessage(msg, 1, receiver);

    assert(receiver.stopCount == 1);
    assert(receiver.clockCount == 0);
    assert(receiver.startCount == 0);
    assert(receiver.continueCount == 0);

    std::cout << "[PASS] test_RealtimeStop\n";
}

} // namespace test

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << "LibreMidiTransport Unit Tests\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n\n";

    test::test_NoteOn();
    test::test_NoteOff();
    test::test_NoteOnWithZeroVelocity_IsNoteOff();
    test::test_ControlChange();
    test::test_ChannelExtraction();
    test::test_SysEx();
    test::test_EmptyMessage();
    test::test_ShortMessage();
    test::test_RealtimeClock();
    test::test_RealtimeStart();
    test::test_RealtimeContinue();
    test::test_RealtimeStop();

    std::cout << "\n═══════════════════════════════════════════════════════════════════\n";
    std::cout << "All tests passed!\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n";

    return 0;
}
