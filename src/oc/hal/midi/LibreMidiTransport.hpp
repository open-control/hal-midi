#pragma once

/**
 * @file LibreMidiTransport.hpp
 * @brief MIDI transport using libremidi (Desktop + WebMIDI)
 *
 * Provides real MIDI I/O for desktop and browser platforms.
 * - Desktop: Synchronous port enumeration (WinMM, ALSA, CoreMIDI)
 * - WebMIDI: Asynchronous port discovery via callbacks (Emscripten)
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <oc/type/Result.hpp>
#include <oc/interface/IMidi.hpp>

namespace libremidi {
class midi_in;
class midi_out;
class observer;
struct input_port;
struct output_port;
}

namespace oc::hal::midi {

/**
 * @brief Configuration for LibreMidiTransport
 */
struct LibreMidiConfig {
    /// Application name used for MIDI port naming (default: "OpenControl")
    std::string appName = "OpenControl";
    
    /// Maximum number of active notes to track for allNotesOff()
    size_t maxActiveNotes = 32;
    
    /// Input port name pattern to look for (empty = first available)
    std::string inputPortPattern = "";
    
    /// Output port name pattern to look for (empty = first available)
    std::string outputPortPattern = "";
};

/**
 * @brief Desktop MIDI transport using libremidi (WinMM backend)
 *
 * Implements IMidiTransport for desktop platforms.
 * Uses libremidi with the WinMM backend for Windows MIDI support.
 *
 * ## Usage with loopMIDI (Windows)
 *
 * 1. Install loopMIDI from https://www.tobias-erichsen.de/software/loopmidi.html
 * 2. Create a virtual MIDI port (e.g., "OpenControl")
 * 3. Configure your DAW to use this port
 * 4. LibreMidiTransport will connect to matching ports automatically
 */
class LibreMidiTransport : public interface::IMidi {
public:
    static constexpr size_t DEFAULT_MAX_ACTIVE_NOTES = 32;

    LibreMidiTransport();
    explicit LibreMidiTransport(const LibreMidiConfig& config);
    ~LibreMidiTransport() override;

    // Non-copyable
    LibreMidiTransport(const LibreMidiTransport&) = delete;
    LibreMidiTransport& operator=(const LibreMidiTransport&) = delete;

    // Moveable
    LibreMidiTransport(LibreMidiTransport&&) noexcept;
    LibreMidiTransport& operator=(LibreMidiTransport&&) noexcept;

    oc::type::Result<void> init() override;
    void update() override;

    void sendCC(uint8_t channel, uint8_t cc, uint8_t value) override;
    void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override;
    void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override;
    void sendSysEx(const uint8_t* data, size_t length) override;
    void sendProgramChange(uint8_t channel, uint8_t program) override;
    void sendPitchBend(uint8_t channel, int16_t value) override;
    void sendChannelPressure(uint8_t channel, uint8_t pressure) override;
    void allNotesOff() override;

    void setOnCC(CCCallback cb) override;
    void setOnNoteOn(NoteCallback cb) override;
    void setOnNoteOff(NoteCallback cb) override;
    void setOnSysEx(SysExCallback cb) override;

private:
    struct ActiveNote {
        uint8_t channel;
        uint8_t note;
        bool active;
    };

    void markNoteActive(uint8_t channel, uint8_t note);
    void markNoteInactive(uint8_t channel, uint8_t note);
    void processMessage(const uint8_t* data, size_t length);
    
    // WebMIDI async port handling
    void onInputAdded(const libremidi::input_port& port);
    void onOutputAdded(const libremidi::output_port& port);

    LibreMidiConfig config_;
    std::unique_ptr<libremidi::observer> observer_;  // Keep alive for WebMIDI callbacks
    std::unique_ptr<libremidi::midi_in> midi_in_;
    std::unique_ptr<libremidi::midi_out> midi_out_;

    CCCallback on_cc_;
    NoteCallback on_note_on_;
    NoteCallback on_note_off_;
    SysExCallback on_sysex_;

    std::vector<ActiveNote> active_notes_;
    bool initialized_ = false;
};

}  // namespace oc::hal::midi
