#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // the Max SDK pulls in windows.h: keep its min/max macros off std::min/std::max
#endif
#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "tr808/core/voiceboard.h"
#include "tr808/core/parallel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace {
using namespace tr808;

struct Control {
    const char* name;
    double Controls::* knob;
    bool Controls::* selector;
};
constexpr Control controls[] = {
#include "controls.inc"
};
constexpr auto controlCount = sizeof(controls) / sizeof(*controls);

enum class CalibrationIndex {
#define CAL(key, label, unit, low, high, initial) key,
#include "calibration.inc"
#undef CAL
    count
};

struct CalibrationControl {
    const char* name;
    double minimum, maximum, initial;
};

constexpr CalibrationControl calibrationControls[] = {
#define CAL(key, label, unit, low, high, initial) {#key, low, high, initial},
#include "calibration.inc"
#undef CAL
};

using CalibrationValues = std::array<double, static_cast<size_t>(CalibrationIndex::count)>;

Calibration makeCalibration(const CalibrationValues& values) {
    auto value = [&](CalibrationIndex index) { return values[static_cast<size_t>(index)]; };
    auto cal = unitCalibration();
    using C = CalibrationIndex;
    cal.cbHz1 = value(C::cbHz1);
    cal.cbHz2 = value(C::cbHz2);
    cal.metalHz = {value(C::metalHz1), value(C::metalHz2), value(C::metalHz3), value(C::metalHz4)};
    cal.metalDuty = value(C::metalDuty) / 100;
    cal.noiseRms = value(C::noiseRms) / 1000;
    cal.cpOffset = value(C::cpOffset) / 100;
    cal.lateClap = value(C::lateClap) >= 0.5;
    cal.lateSnare = value(C::lateSnare) >= 0.5;
    cal.claveR313 = value(C::claveR313) * 1000;
    cal.parts.erase("rs.r313");
    cal.monoMs = value(C::monoMs);
    const double cap = 15e-9 * std::exp2(-value(C::kickTune) / 12);
    cal.parts["bd.c41"] = cap;
    cal.parts["bd.c42"] = cap;
    return cal;
}

// Denormals: a decaying circuit's tiny values slow the CPU down. Flush them to zero on the audio
// thread and on the worker threads (the bit is per thread).
struct NoDenormals {
#if defined(__aarch64__)
    uint64_t saved;
    NoDenormals() {
        asm volatile("mrs %0, fpcr" : "=r"(saved));
        asm volatile("msr fpcr, %0" : : "r"(saved | (uint64_t(1) << 24)));
    }
    ~NoDenormals() { asm volatile("msr fpcr, %0" : : "r"(saved)); }
#elif defined(__x86_64__) || defined(_M_X64)
    unsigned saved;
    NoDenormals() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040); }
    ~NoDenormals() { _mm_setcsr(saved); }
#endif
};

void flushDenormalsForever() {
#if defined(__aarch64__)
    uint64_t r;
    asm volatile("mrs %0, fpcr" : "=r"(r));
    asm volatile("msr fpcr, %0" : : "r"(r | (uint64_t(1) << 24)));
#elif defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
}

// The voice workers render inside the audio callback, so they need audio-thread priority: on macOS
// a real-time (time-constraint) thread, which also keeps them on the M1's performance cores.
void makeWorkerRealtime(int) {
    flushDenormalsForever();
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__APPLE__)
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double msToAbs = 1e6 * double(tb.denom) / double(tb.numer);
    thread_time_constraint_policy_data_t policy;
    policy.period = uint32_t(2.9 * msToAbs);       // about 128 samples at 44.1 kHz
    policy.computation = uint32_t(1.0 * msToAbs);
    policy.constraint = uint32_t(2.9 * msToAbs);
    policy.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#elif defined(__linux__)
    sched_param sp{};
    sp.sched_priority = 70;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); // ignored if not permitted
#endif
}

int workerCount() {
    // The calling audio thread takes jobs too; two helpers suit an M1's four performance cores and
    // leave room for other devices and instances. Push 3 standalone gets one.
    const unsigned cores = std::thread::hardware_concurrency();
#if defined(__linux__)
    return cores >= 4 ? 1 : 0;
#else
    return cores >= 4 ? 2 : (cores >= 2 ? 1 : 0);
#endif
}

struct Engine {
    // Declared before the board: the board holds a pointer to it, so it must outlive the board.
    std::unique_ptr<Parallel> parallel = std::make_unique<Parallel>(workerCount(), makeWorkerRealtime);
    std::unique_ptr<VoiceBoard> board = std::make_unique<VoiceBoard>();
    std::mutex boardMutex;
    // Calibration, live: the Max thread stores each value as it moves, the audio thread applies
    // what changed at the start of its next block (VoiceBoard::setLive*), no rebuild.
    std::array<std::atomic<double>, static_cast<size_t>(CalibrationIndex::count)> calibration;
    CalibrationValues built{};   // the values the board was prepared with
    CalibrationValues calApplied{}; // the values the board plays now
    double sampleRate = 0;
    bool syncControls = true;
    std::array<std::atomic<float>, controlCount> values;
    std::atomic<uint16_t> pending{0};
    std::atomic<bool> prepared{false};
    std::vector<float> buffer;
    Controls applied{};
    int hold = 0;
    int pulseSamples = 0;

    Engine() {
        for (size_t i = 0; i < built.size(); ++i) {
            built[i] = calibrationControls[i].initial;
            calibration[i].store(built[i], std::memory_order_relaxed);
        }
        calApplied = built;
        board->setCalibration(makeCalibration(built));
        board->setParallel(parallel->workers() > 0 ? parallel.get() : nullptr);
        for (size_t i = 0; i < controlCount; ++i) {
            const auto& control = controls[i];
            values[i].store(control.knob ? applied.*control.knob : applied.*control.selector);
        }
    }
};

struct External {
    t_pxobject object;
    Engine* engine;
    void* statusOutlet;
};
t_class* engineClass = nullptr;

uint16_t voiceForNote(long note) {
    switch (note) {
        case 35: case 36: return kBitBD;
        case 38: case 40: return kBitSD;
        case 41: case 43: case 64: return kBitLT;
        case 45: case 47: case 63: return kBitMT;
        case 48: case 50: case 62: return kBitHT;
        case 37: case 75: return kBitRS;
        case 39: case 70: return kBitCP;
        case 56: return kBitCB;
        case 49: case 57: return kBitCY;
        case 46: return kBitOH;
        case 42: case 44: return kBitCH;
        default: return 0;
    }
}

void hit(External* x, t_symbol*, long count, t_atom* atoms) {
    if (count != 2 || atom_getlong(atoms + 1) <= 0) return;
    const auto voice = voiceForNote(atom_getlong(atoms));
    if (voice) {
        const auto accent = atom_getlong(atoms + 1) >= 100 ? kBitAC : 0;
        x->engine->pending.fetch_or(voice | accent, std::memory_order_relaxed);
    }
}

void control(External* x, t_symbol*, long count, t_atom* atoms) {
    if (count != 2 || atom_gettype(atoms) != A_SYM) return;
    const auto* name = atom_getsym(atoms);
    const double value = atom_getfloat(atoms + 1);
    if (!std::isfinite(value)) return;
    for (size_t i = 0; i < controlCount; ++i) {
        if (name == gensym(controls[i].name)) {
            x->engine->values[i].store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
            return;
        }
    }
}

double kickCap(double semitones) { return 15e-9 * std::exp2(-semitones / 12); }

// Audio thread: apply the calibration values that moved since the last block.
void applyLiveCalibration(Engine& engine) {
    using C = CalibrationIndex;
    CalibrationValues now;
    bool any = false;
    for (size_t i = 0; i < now.size(); ++i) {
        now[i] = engine.calibration[i].load(std::memory_order_relaxed);
        any |= now[i] != engine.calApplied[i];
    }
    if (!any) return;
    auto moved = [&](C c) { return now[static_cast<size_t>(c)] != engine.calApplied[static_cast<size_t>(c)]; };
    auto value = [&](C c) { return now[static_cast<size_t>(c)]; };
    auto& board = *engine.board;
    if (moved(C::cbHz1) || moved(C::cbHz2) || moved(C::metalHz1) || moved(C::metalHz2) ||
        moved(C::metalHz3) || moved(C::metalHz4) || moved(C::metalDuty)) {
        const double hz[6] = {value(C::cbHz1), value(C::cbHz2), value(C::metalHz1),
                              value(C::metalHz2), value(C::metalHz3), value(C::metalHz4)};
        board.setLiveMetal(hz, value(C::metalDuty) / 100);
    }
    if (moved(C::noiseRms)) board.setLiveNoise(value(C::noiseRms) / 1000);
    if (moved(C::cpOffset)) board.setLiveClapOffset(value(C::cpOffset) / 100);
    if (moved(C::lateClap)) // R200 (10k, Q70's emitter to -15 V) fitted in later units, open in early ones
        board.setLivePart(Voice::CP, "r200cp", value(C::lateClap) >= 0.5 ? 10e3 : 1e12);
    if (moved(C::lateSnare)) { // the "PORTION CHANGED" values, or the schematic's (sd.cir)
        const bool late = value(C::lateSnare) >= 0.5;
        board.setLivePart(Voice::SD, "c58", late ? 0.056e-6 : 0.027e-6);
        board.setLivePart(Voice::SD, "c61", late ? 0.015e-6 : 0.0068e-6);
        board.setLivePart(Voice::SD, "r191", late ? 10e3 : 8.2e3);
        board.setLivePart(Voice::SD, "r200", late ? 0.1 : 47e3);
    }
    if (moved(C::claveR313)) board.setLivePart(Voice::RS, "r313", value(C::claveR313) * 1000);
    if (moved(C::monoMs)) {
        board.setLiveTriggerWidth(value(C::monoMs));
        engine.pulseSamples = std::max(1, static_cast<int>(std::ceil(engine.sampleRate * (value(C::monoMs) + 0.2) * 0.001)));
    }
    if (moved(C::kickTune)) { // C41 and C42, the kick's resonator: retunes a ringing kick too
        const double cap = kickCap(value(C::kickTune));
        board.setLivePart(Voice::BD, "c41", cap);
        board.setLivePart(Voice::BD, "c42", cap);
    }
    engine.calApplied = now;
}

void perform(External* x, t_object*, double**, long, double** outs, long,
             long frames, long, void*) {
    auto& engine = *x->engine;
    const NoDenormals noDenormals;
    std::unique_lock lock(engine.boardMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::fill_n(outs[0], frames, 0.0);
        return;
    }
    auto next = engine.applied;
    bool changed = engine.syncControls;
    for (size_t i = 0; i < controlCount; ++i) {
        const auto& control = controls[i];
        const float value = engine.values[i].load(std::memory_order_relaxed);
        if (control.knob) {
            changed |= next.*control.knob != value;
            next.*control.knob = value;
        } else {
            changed |= next.*control.selector != (value >= 0.5f);
            next.*control.selector = value >= 0.5f;
        }
    }
    applyLiveCalibration(engine);
    if (changed) {
        engine.board->setControls(next);
        engine.applied = next;
        engine.syncControls = false;
    }

    long position = 0;
    while (position < frames) {
        if (!engine.hold) {
            const auto bits = engine.pending.exchange(0, std::memory_order_relaxed);
            if (bits) {
                engine.board->setLines(true, bits);
                engine.board->render(engine.buffer.data() + position, 1);
                ++position;
                engine.board->setLines(false, bits);
                engine.hold = engine.pulseSamples;
                continue;
            }
            engine.board->setLines(false, 0);
        }
        const auto available = frames - position;
        const int block = static_cast<int>(engine.hold ? std::min<long>(available, engine.hold) : available);
        engine.board->render(engine.buffer.data() + position, block);
        position += block;
        if (engine.hold) engine.hold -= block;
    }
    std::copy_n(engine.buffer.data(), frames, outs[0]);
}

void calibration(External* x, t_symbol*, long count, t_atom* atoms) {
    if (count != 2 || atom_gettype(atoms) != A_SYM) return;
    const auto* name = atom_getsym(atoms);
    const double value = atom_getfloat(atoms + 1);
    if (!std::isfinite(value)) return;
    auto& engine = *x->engine;
    for (size_t i = 0; i < engine.calibration.size(); ++i) {
        const auto& item = calibrationControls[i];
        if (name == gensym(item.name)) {
            engine.calibration[i].store(std::clamp(value, item.minimum, item.maximum), std::memory_order_relaxed);
            return;
        }
    }
}

// Calibration is live now (applyLiveCalibration); kept so the patch's status still answers.
void applyCalibration(External* x) {
    outlet_anything(x->statusOutlet, gensym("calibrated"), 0, nullptr);
}

void status(External* x) {
    outlet_anything(x->statusOutlet,
        gensym(x->engine->prepared.load(std::memory_order_relaxed) ? "armed" : "loaded"),
        0, nullptr);
}

void dsp64(External* x, t_object* chain, short*, double rate, long maxFrames, long) {
    auto& engine = *x->engine;
    engine.prepared.store(false, std::memory_order_relaxed);
    if (rate <= 0 || maxFrames <= 0) return;
    try {
        const std::lock_guard lock(engine.boardMutex);
        engine.buffer.resize(maxFrames);
        engine.board->prepare(rate);
        engine.sampleRate = rate;
        engine.calApplied = engine.built; // prepare() rebuilt the board from these: the next block re-applies the live values
        const auto width = engine.board->calibration().monoMs;
        engine.pulseSamples = std::max(1, static_cast<int>(std::ceil(rate * (width + 0.2) * 0.001)));
        engine.syncControls = true;
        engine.hold = 0;
        engine.pending.store(0, std::memory_order_relaxed);
        dsp_add64(chain, reinterpret_cast<t_object*>(x), reinterpret_cast<t_perfroutine64>(perform), 0, nullptr);
        engine.prepared.store(true, std::memory_order_relaxed);
    } catch (const std::exception& error) {
        object_error(reinterpret_cast<t_object*>(x), "Could not prepare voice board: %s", error.what());
    }
}

void destroy(External* x) {
    dsp_free(&x->object);
    delete x->engine;
}

void* create() {
    auto* x = reinterpret_cast<External*>(object_alloc(engineClass));
    if (!x) return nullptr;
    x->engine = nullptr;
    dsp_setup(&x->object, 1);
    x->object.z_misc |= Z_NO_INPLACE;
    try {
        x->engine = new Engine();
    } catch (const std::exception& error) {
        object_error(reinterpret_cast<t_object*>(x), "Could not create voice board: %s", error.what());
        object_free(x);
        return nullptr;
    }
    x->statusOutlet = outlet_new(reinterpret_cast<t_object*>(x), nullptr);
    outlet_new(reinterpret_cast<t_object*>(x), "signal");
    return x;
}
}

extern "C" C74_EXPORT void ext_main(void*) {
    auto* cls = class_new("ticco808cal~", reinterpret_cast<method>(create),
                          reinterpret_cast<method>(destroy), sizeof(External), nullptr, 0);
    class_addmethod(cls, reinterpret_cast<method>(dsp64), "dsp64", A_CANT, 0);
    class_addmethod(cls, reinterpret_cast<method>(hit), "hit", A_GIMME, 0);
    class_addmethod(cls, reinterpret_cast<method>(control), "control", A_GIMME, 0);
    class_addmethod(cls, reinterpret_cast<method>(status), "status", 0);
    class_addmethod(cls, reinterpret_cast<method>(calibration), "cal", A_GIMME, 0);
    class_addmethod(cls, reinterpret_cast<method>(applyCalibration), "applycal", 0);
    class_dspinit(cls);
    class_register(CLASS_BOX, cls);
    engineClass = cls;
}
