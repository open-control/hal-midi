#include "LibreMidiTransport.hpp"

#include <chrono>
#include <libremidi/libremidi.hpp>
#ifdef __EMSCRIPTEN__
#include <libremidi/configurations.hpp>
#endif
#include <oc/log/Log.hpp>

namespace oc::hal::midi {

namespace {

uint64_t nowSteadyUs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

LibreMidiTransport::LibreMidiTransport() : LibreMidiTransport(LibreMidiConfig{}) {}

LibreMidiTransport::LibreMidiTransport(const LibreMidiConfig& config)
    : config_(config) {}

LibreMidiTransport::~LibreMidiTransport() = default;

oc::type::Result<void> LibreMidiTransport::init() {
    if (initialized_) {
        return oc::type::Result<void>::ok();
    }

    // Initialize active notes tracking
    active_notes_.resize(config_.maxActiveNotes);
    for (auto& note : active_notes_) {
        note.active = false;
    }

    try {
#ifdef __EMSCRIPTEN__
        // ═══════════════════════════════════════════════════════════════
        // WebMIDI: Asynchronous port discovery via callbacks
        // Ports are opened when callbacks fire, not immediately
        // ═══════════════════════════════════════════════════════════════
        OC_LOG_INFO("MIDI: Initializing WebMIDI (async mode)");
        
        observer_ = std::make_unique<libremidi::observer>(
            libremidi::observer_configuration{
                .input_added = [this](const libremidi::input_port& port) {
                    onInputAdded(port);
                },
                .input_removed = [](const libremidi::input_port& port) {
                    OC_LOG_DEBUG("MIDI: Input removed: {}", port.display_name.c_str());
                },
                .output_added = [this](const libremidi::output_port& port) {
                    onOutputAdded(port);
                },
                .output_removed = [](const libremidi::output_port& port) {
                    OC_LOG_DEBUG("MIDI: Output removed: {}", port.display_name.c_str());
                }
            },
            libremidi::observer_configuration_for(libremidi::API::WEBMIDI)
        );
        
        initialized_ = true;
        OC_LOG_INFO("MIDI: WebMIDI observer started (waiting for ports)");
#else
        // ═══════════════════════════════════════════════════════════════
        // Desktop: Virtual ports (macOS) or existing port enumeration
        // ═══════════════════════════════════════════════════════════════

        // Create MIDI input with callback.
        // Note: callbacks may come from a background thread depending on backend.
        libremidi::input_configuration in_config;
        // libremidi ignores timing / realtime messages by default (ignore_timing=true).
        // We need realtime clock / transport for external sync.
        in_config.ignore_timing = false;
        in_config.on_message = [this](libremidi::message&& msg) {
            if (msg.bytes.empty()) return;

            PendingMessage pending{};
            pending.timestampUs = nowSteadyUs();
            pending.bytes = std::move(msg.bytes);

            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (pending_messages_.size() >= max_pending_messages_) {
                // Drop newest to keep bounded memory.
                return;
            }
            pending_messages_.push_back(std::move(pending));
        };

#if defined(__APPLE__)
        if (config_.useVirtualPorts) {
            // macOS: CoreMIDI virtual ports (native support)
            midi_in_ = std::make_unique<libremidi::midi_in>(in_config);
            midi_out_ = std::make_unique<libremidi::midi_out>();

            if (!config_.inputPortName.empty()) {
                midi_in_->open_virtual_port(config_.inputPortName);
                OC_LOG_INFO("MIDI: Created virtual input port: {}", config_.inputPortName.c_str());
            }
            if (!config_.outputPortName.empty()) {
                midi_out_->open_virtual_port(config_.outputPortName);
                OC_LOG_INFO("MIDI: Created virtual output port: {}", config_.outputPortName.c_str());
            }
            initialized_ = true;
            OC_LOG_INFO("MIDI: Virtual ports ready");
        } else
#endif
        {
            // Connect to existing ports
            // Linux: VirMIDI kernel ports (via snd-virmidi module)
            // Windows: loopMIDI virtual ports
            midi_in_ = std::make_unique<libremidi::midi_in>(in_config);
            midi_out_ = std::make_unique<libremidi::midi_out>();

            // Configure observer to see both hardware and virtual ports
            libremidi::observer_configuration obs_config;
            obs_config.track_hardware = true;
            obs_config.track_virtual = true;  // Needed for VirMIDI/loopMIDI
            libremidi::observer obs{obs_config};

            auto in_ports = obs.get_input_ports();
            auto out_ports = obs.get_output_ports();

            OC_LOG_INFO("MIDI: Found {} input ports, {} output ports", in_ports.size(), out_ports.size());

            // Find matching input port
            bool in_opened = false;
            for (size_t i = 0; i < in_ports.size(); ++i) {
                std::string name = in_ports[i].display_name;
                OC_LOG_DEBUG("MIDI IN [{}]: {}", i, name.c_str());

                if (config_.inputPortName.empty() ||
                    name.find(config_.inputPortName) != std::string::npos) {
                    midi_in_->open_port(in_ports[i]);
                    OC_LOG_INFO("MIDI: Opened input port: {}", name.c_str());
                    in_opened = true;
                    break;
                }
            }

            // Find matching output port
            bool out_opened = false;
            for (size_t i = 0; i < out_ports.size(); ++i) {
                std::string name = out_ports[i].display_name;
                OC_LOG_DEBUG("MIDI OUT [{}]: {}", i, name.c_str());

                if (config_.outputPortName.empty() ||
                    name.find(config_.outputPortName) != std::string::npos) {
                    midi_out_->open_port(out_ports[i]);
                    OC_LOG_INFO("MIDI: Opened output port: {}", name.c_str());
                    out_opened = true;
                    break;
                }
            }

            if (!in_opened) {
                OC_LOG_WARN("MIDI: No input port opened (pattern: {})", config_.inputPortName.c_str());
            }
            if (!out_opened) {
                OC_LOG_WARN("MIDI: No output port opened (pattern: {})", config_.outputPortName.c_str());
            }

            initialized_ = true;
            OC_LOG_INFO("MIDI: Initialized successfully");
        }
#endif
    } catch (const std::exception& e) {
        OC_LOG_ERROR("MIDI: Init failed: {}", e.what());
        return oc::type::Result<void>::err(oc::type::ErrorCode::HARDWARE_INIT_FAILED);
    }

    return oc::type::Result<void>::ok();
}

void LibreMidiTransport::update() {
    // Process buffered MIDI messages on the main thread.
    std::vector<PendingMessage> local;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        local.swap(pending_messages_);
    }

    for (auto& pending : local) {
        processMessage(pending.bytes.data(), pending.bytes.size(), pending.timestampUs);
    }
}

void LibreMidiTransport::processMessage(const uint8_t* data, size_t length, uint64_t timestampUs) {
    if (length == 0) return;

    // Debug: log incoming MIDI (can be very chatty)
    OC_LOG_DEBUG("MIDI RX: status={} len={}", data[0], length);

    uint8_t status = data[0];

    // Realtime single-byte messages (may appear interleaved at any time)
    switch (status) {
        case 0xF8:
            if (on_clock_) on_clock_(timestampUs);
            return;
        case 0xFA:
            if (on_start_) on_start_();
            return;
        case 0xFB:
            if (on_continue_) on_continue_();
            return;
        case 0xFC:
            if (on_stop_) on_stop_();
            return;
        default:
            break;
    }

    uint8_t type = status & 0xF0;
    uint8_t channel = status & 0x0F;

    switch (type) {
        case 0x80: // Note Off
            if (length >= 3 && on_note_off_) {
                on_note_off_(channel, data[1], data[2]);
            }
            break;

        case 0x90: // Note On
            if (length >= 3) {
                if (data[2] == 0 && on_note_off_) {
                    on_note_off_(channel, data[1], 0);
                } else if (on_note_on_) {
                    on_note_on_(channel, data[1], data[2]);
                }
            }
            break;

        case 0xB0: // Control Change
            if (length >= 3 && on_cc_) {
                on_cc_(channel, data[1], data[2]);
            }
            break;

        case 0xF0: // System Exclusive
            if (status == 0xF0 && on_sysex_) {
                on_sysex_(data, length);
            }
            break;

        default:
            break;
    }
}

void LibreMidiTransport::markNoteActive(uint8_t channel, uint8_t note) {
    for (auto& slot : active_notes_) {
        if (!slot.active) {
            slot = {channel, note, true};
            return;
        }
    }
    if (!active_notes_.empty()) {
        active_notes_[0] = {channel, note, true};
    }
}

void LibreMidiTransport::markNoteInactive(uint8_t channel, uint8_t note) {
    for (auto& slot : active_notes_) {
        if (slot.active && slot.channel == channel && slot.note == note) {
            slot.active = false;
            return;
        }
    }
}

void LibreMidiTransport::sendCC(uint8_t channel, uint8_t cc, uint8_t value) {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    libremidi::message msg;
    msg.bytes = {
        static_cast<uint8_t>(0xB0 | (channel & 0x0F)),
        static_cast<uint8_t>(cc & 0x7F),
        static_cast<uint8_t>(value & 0x7F)
    };
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    markNoteActive(channel, note);
    libremidi::message msg;
    msg.bytes = {
        static_cast<uint8_t>(0x90 | (channel & 0x0F)),
        static_cast<uint8_t>(note & 0x7F),
        static_cast<uint8_t>(velocity & 0x7F)
    };
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    markNoteInactive(channel, note);
    libremidi::message msg;
    msg.bytes = {
        static_cast<uint8_t>(0x80 | (channel & 0x0F)),
        static_cast<uint8_t>(note & 0x7F),
        static_cast<uint8_t>(velocity & 0x7F)
    };
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendSysEx(const uint8_t* data, size_t length) {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    libremidi::message msg;
    msg.bytes.assign(data, data + length);
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendProgramChange(uint8_t channel, uint8_t program) {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    libremidi::message msg;
    msg.bytes = {
        static_cast<uint8_t>(0xC0 | (channel & 0x0F)),
        static_cast<uint8_t>(program & 0x7F)
    };
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendPitchBend(uint8_t channel, int16_t value) {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    uint16_t bend = static_cast<uint16_t>(value + 8192);
    libremidi::message msg;
    msg.bytes = {
        static_cast<uint8_t>(0xE0 | (channel & 0x0F)),
        static_cast<uint8_t>(bend & 0x7F),
        static_cast<uint8_t>((bend >> 7) & 0x7F)
    };
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendChannelPressure(uint8_t channel, uint8_t pressure) {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    libremidi::message msg;
    msg.bytes = {
        static_cast<uint8_t>(0xD0 | (channel & 0x0F)),
        static_cast<uint8_t>(pressure & 0x7F)
    };
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendClock() {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    libremidi::message msg;
    msg.bytes = {0xF8};
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendStart() {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    libremidi::message msg;
    msg.bytes = {0xFA};
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendStop() {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    libremidi::message msg;
    msg.bytes = {0xFC};
    midi_out_->send_message(msg);
}

void LibreMidiTransport::sendContinue() {
    if (!midi_out_ || !midi_out_->is_port_connected()) return;

    libremidi::message msg;
    msg.bytes = {0xFB};
    midi_out_->send_message(msg);
}

void LibreMidiTransport::allNotesOff() {
    for (auto& slot : active_notes_) {
        if (slot.active) {
            sendNoteOff(slot.channel, slot.note, 0);
            slot.active = false;
        }
    }
}

void LibreMidiTransport::setOnCC(CCCallback cb) { on_cc_ = std::move(cb); }
void LibreMidiTransport::setOnNoteOn(NoteCallback cb) { on_note_on_ = std::move(cb); }
void LibreMidiTransport::setOnNoteOff(NoteCallback cb) { on_note_off_ = std::move(cb); }
void LibreMidiTransport::setOnSysEx(SysExCallback cb) { on_sysex_ = std::move(cb); }
void LibreMidiTransport::setOnClock(ClockCallback cb) { on_clock_ = std::move(cb); }
void LibreMidiTransport::setOnStart(RealtimeCallback cb) { on_start_ = std::move(cb); }
void LibreMidiTransport::setOnStop(RealtimeCallback cb) { on_stop_ = std::move(cb); }
void LibreMidiTransport::setOnContinue(RealtimeCallback cb) { on_continue_ = std::move(cb); }

// =============================================================================
// WebMIDI async port handling
// =============================================================================

void LibreMidiTransport::onInputAdded(const libremidi::input_port& port) {
    std::string name = port.display_name;
    OC_LOG_DEBUG("MIDI: Input port available: {}", name.c_str());
    
    // Skip if already have an input
    if (midi_in_ && midi_in_->is_port_connected()) {
        return;
    }
    
    // Check pattern match
    if (!config_.inputPortName.empty() &&
        name.find(config_.inputPortName) == std::string::npos) {
        return;
    }
    
    // Create and open input
    libremidi::input_configuration in_config;
    // Keep timing messages for external clock / transport sync.
    in_config.ignore_timing = false;
    in_config.on_message = [this](libremidi::message&& msg) {
        if (msg.bytes.empty()) return;

        PendingMessage pending{};
        pending.timestampUs = nowSteadyUs();
        pending.bytes = std::move(msg.bytes);

        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_messages_.size() >= max_pending_messages_) {
            return;
        }
        pending_messages_.push_back(std::move(pending));
    };
    
#ifdef __EMSCRIPTEN__
    midi_in_ = std::make_unique<libremidi::midi_in>(
        in_config, 
        libremidi::midi_in_configuration_for(libremidi::API::WEBMIDI)
    );
#else
    midi_in_ = std::make_unique<libremidi::midi_in>(in_config);
#endif
    midi_in_->open_port(port);
    OC_LOG_INFO("MIDI: Opened input port: {}", name.c_str());
}

void LibreMidiTransport::onOutputAdded(const libremidi::output_port& port) {
    std::string name = port.display_name;
    OC_LOG_DEBUG("MIDI: Output port available: {}", name.c_str());
    
    // Skip if already have an output
    if (midi_out_ && midi_out_->is_port_connected()) {
        return;
    }
    
    // Check pattern match
    if (!config_.outputPortName.empty() &&
        name.find(config_.outputPortName) == std::string::npos) {
        return;
    }
    
    // Create and open output
#ifdef __EMSCRIPTEN__
    midi_out_ = std::make_unique<libremidi::midi_out>(
        libremidi::output_configuration{},
        libremidi::midi_out_configuration_for(libremidi::API::WEBMIDI)
    );
#else
    midi_out_ = std::make_unique<libremidi::midi_out>();
#endif
    midi_out_->open_port(port);
    OC_LOG_INFO("MIDI: Opened output port: {}", name.c_str());
}

}  // namespace oc::hal::midi
