/* Minimal queued-audio bridge for the pthread web runtime.
 *
 * SDL2 2.32's Emscripten backend creates its AudioContext with
 * MAIN_THREAD_EM_ASM, then reads Module.SDL2.audioContext from the worker with
 * plain EM_ASM. JavaScript globals are per-thread, so PROXY_TO_PTHREAD builds
 * fault before the device opens. This bridge keeps every Web Audio operation
 * on the browser thread and exposes the small SDL queued-audio surface used by
 * the runtime. Guest samples remain 44.1 kHz; Web Audio performs any host-rate
 * conversion.
 */

#include "web_audio_bridge.h"

#include <emscripten.h>
#include <string.h>

/* Two ordinary vblanks per browser submission. Audio used to make one
 * synchronous worker->browser query every vblank plus another synchronous hop
 * for each submission. Windows Chromium can park the browser thread between
 * those calls, stalling the entire guest and then starving the very audio queue
 * the call was trying to fill. Keep the two-vblank chunk, but hand it through a
 * shared-memory slot to MAIN_THREAD_ASYNC_EM_ASM instead. */
#define WEB_AUDIO_STAGE_CAP_BYTES   (4096u * 4u)
#define WEB_AUDIO_FLUSH_BYTES       (1470u * 4u)
#define WEB_AUDIO_ASYNC_SLOTS       8u
static Uint8 s_web_audio_stage[WEB_AUDIO_STAGE_CAP_BYTES];
static Uint32 s_web_audio_stage_len;
static Uint8 s_web_audio_slots[WEB_AUDIO_ASYNC_SLOTS][WEB_AUDIO_STAGE_CAP_BYTES];
/* Zero = free; non-zero = byte length ready for / owned by the browser. The
 * browser clears a slot only after it has copied the PCM into an AudioBuffer. */
static Uint32 s_web_audio_slot_len[WEB_AUDIO_ASYNC_SLOTS];
static Uint32 s_web_audio_next_slot;
static Uint32 s_web_audio_generation = 1;
static Uint32 s_web_audio_async_drops;

int psx_web_audio_init_subsystem(Uint32 flags) {
    (void)flags;
    return MAIN_THREAD_EM_ASM_INT({
        var platform = (navigator.userAgentData && navigator.userAgentData.platform) ||
                       navigator.platform || "";
        var isWindows = /^Win/.test(platform) ||
                        /Windows/.test(navigator.userAgent || "");
        if (!Module.psxWebAudio) {
            var Context = globalThis.AudioContext || globalThis.webkitAudioContext;
            if (!Context) return -1;
            var context;
            try {
                /* Windows browser/device stacks are much more reliable with a
                 * modest hardware buffer. This remains well below the latency
                 * of the old 200 ms queue cap while avoiding the tiny default
                 * quantum selected by some Chrome + WASAPI combinations. */
                context = new Context({ latencyHint: isWindows ? 0.04 : "interactive" });
            } catch (_) {
                context = new Context();
            }
            Module.psxWebAudio = {};
            Module.psxWebAudio.context = context;
            Module.psxWebAudio.nextTime = context.currentTime;
            Module.psxWebAudio.sources = new Set();
        }

        var state = Module.psxWebAudio;
        var baseLatency = Number(state.context.baseLatency) || 0;
        var outputLatency = Number(state.context.outputLatency) || 0;
        var minimumLead = isWindows ? 0.160 : 0.030;
        var safetyMargin = isWindows ? 0.050 : 0.015;
        state.targetLead = Math.min(0.200,
            Math.max(minimumLead, baseLatency + outputLatency + safetyMargin));
        state.started = !!state.started;
        state.underruns = state.underruns || 0;
        state.bridgeDrops = state.bridgeDrops || 0;
        state.sources = state.sources || new Set();
        state.output = state.output || state.context.destination;
        state.profile = isWindows ? "windows-stable" : "interactive";
        /* Runs only on the browser thread. The worker publishes PCM through a
         * SharedArrayBuffer slot and posts an asynchronous callback below, so
         * neither sample conversion nor Web Audio scheduling can stall guest
         * simulation while Chromium is busy servicing its UI thread. */
        state.submitPcm = function(ptr, len) {
            var frames = (len / 4) | 0;
            if (!frames) return;
            var now = state.context.currentTime;
            var lead = state.targetLead || 0.030;

            /* After an unusually long browser-thread stall, do not turn the
             * recovered burst into permanently growing A/V latency. Already
             * queued audio remains continuous; excess newest chunks are
             * discarded until wall time catches the queue again. */
            if (state.started && state.nextTime - now > 0.240) {
                state.bridgeDrops++;
                return;
            }

            var buffer = state.context.createBuffer(2, frames, 44100);
            var left = buffer.getChannelData(0);
            var right = buffer.getChannelData(1);
            growMemViews();
            var base = ptr >> 1;
            for (var i = 0; i < frames; ++i) {
                left[i] = HEAP16[base + i * 2] / 32768.0;
                right[i] = HEAP16[base + i * 2 + 1] / 32768.0;
            }
            var source = state.context.createBufferSource();
            source.buffer = buffer;
            source.connect(state.output || state.context.destination);
            var start;
            if (!state.started) {
                start = now + lead;
                state.started = true;
            } else if (!state.nextTime || state.nextTime <= now) {
                state.underruns = (state.underruns || 0) + 1;
                start = now + lead;
                if (state.underruns <= 3 || (state.underruns % 10) == 0) {
                    console.warn("psxrecomp: web audio underrun #" + state.underruns +
                        "; restoring " + Math.round(lead * 1000) + "ms reserve");
                }
            } else {
                start = state.nextTime;
            }
            source.start(start);
            state.nextTime = start + frames / 44100.0;
            state.sources.add(source);
            source.onended = function() { state.sources.delete(source); };
        };
        if (!state.profileLogged) {
            console.info("psxrecomp: web audio profile=" + state.profile +
                " lead=" + Math.round(state.targetLead * 1000) + "ms" +
                " base=" + Math.round(baseLatency * 1000) + "ms" +
                " output=" + Math.round(outputLatency * 1000) + "ms");
            state.profileLogged = true;
        }
        return 0;
    });
}

SDL_AudioDeviceID psx_web_audio_open_device(
    const char *device, int iscapture, const SDL_AudioSpec *desired,
    SDL_AudioSpec *obtained, int allowed_changes) {
    (void)device;
    (void)allowed_changes;
    if (iscapture || !desired || !obtained || desired->channels != 2 ||
        desired->format != AUDIO_S16SYS) {
        return 0;
    }
    *obtained = *desired;
    obtained->callback = NULL;
    return 1;
}

void psx_web_audio_pause_device(SDL_AudioDeviceID device, int pause_on) {
    (void)device;
    MAIN_THREAD_EM_ASM({
        var state = Module.psxWebAudio;
        if (!state) return;
        if ($0 || Module.psxWebAudioLocked) state.context.suspend();
        else state.context.resume();
    }, pause_on);
}

static int psx_web_audio_submit(const void *data, Uint32 len) {
    if (!data || len < 4) return 0;

    const Uint8 *source = (const Uint8 *)data;
    while (len >= 4) {
        Uint32 chunk = len > WEB_AUDIO_STAGE_CAP_BYTES
            ? WEB_AUDIO_STAGE_CAP_BYTES : len;
        chunk &= ~3u;

        Uint32 slot = WEB_AUDIO_ASYNC_SLOTS;
        for (Uint32 offset = 0; offset < WEB_AUDIO_ASYNC_SLOTS; ++offset) {
            Uint32 candidate = (s_web_audio_next_slot + offset) % WEB_AUDIO_ASYNC_SLOTS;
            if (__atomic_load_n(&s_web_audio_slot_len[candidate], __ATOMIC_ACQUIRE) == 0) {
                slot = candidate;
                break;
            }
        }
        if (slot == WEB_AUDIO_ASYNC_SLOTS) {
            /* The browser thread is more than eight submissions behind. Never
             * block the guest waiting for it: losing the newest chunk is less
             * disruptive than freezing simulation and draining all audio. */
            __atomic_add_fetch(&s_web_audio_async_drops, 1u, __ATOMIC_RELAXED);
            return 0;
        }

        memcpy(s_web_audio_slots[slot], source, chunk);
        Uint32 generation = __atomic_load_n(&s_web_audio_generation, __ATOMIC_ACQUIRE);
        __atomic_store_n(&s_web_audio_slot_len[slot], chunk, __ATOMIC_RELEASE);
        MAIN_THREAD_ASYNC_EM_ASM({
            growMemViews();
            var lengthWord = $1 >> 2;
            var generationWord = $3 >> 2;
            var bytes = Atomics.load(HEAPU32, lengthWord);
            try {
                if (bytes &&
                    Atomics.load(HEAPU32, generationWord) === ($2 >>> 0)) {
                    var state = Module.psxWebAudio;
                    if (state && state.submitPcm) state.submitPcm($0, bytes);
                }
            } catch (error) {
                console.error("psxrecomp: asynchronous web audio submission failed", error);
            } finally {
                Atomics.store(HEAPU32, lengthWord, 0);
            }
        }, s_web_audio_slots[slot], &s_web_audio_slot_len[slot], generation,
           &s_web_audio_generation);

        s_web_audio_next_slot = (slot + 1u) % WEB_AUDIO_ASYNC_SLOTS;
        source += chunk;
        len -= chunk;
    }
    return 0;
}

int psx_web_audio_queue(SDL_AudioDeviceID device, const void *data, Uint32 len) {
    (void)device;
    if (!data || len < 4) return 0;

    if (len > WEB_AUDIO_STAGE_CAP_BYTES) {
        if (s_web_audio_stage_len) {
            psx_web_audio_submit(s_web_audio_stage, s_web_audio_stage_len);
            s_web_audio_stage_len = 0;
        }
        return psx_web_audio_submit(data, len);
    }
    if (s_web_audio_stage_len + len > WEB_AUDIO_STAGE_CAP_BYTES) {
        psx_web_audio_submit(s_web_audio_stage, s_web_audio_stage_len);
        s_web_audio_stage_len = 0;
    }
    memcpy(s_web_audio_stage + s_web_audio_stage_len, data, len);
    s_web_audio_stage_len += len;
    if (s_web_audio_stage_len >= WEB_AUDIO_FLUSH_BYTES) {
        psx_web_audio_submit(s_web_audio_stage, s_web_audio_stage_len);
        s_web_audio_stage_len = 0;
    }
    return 0;
}

Uint32 psx_web_audio_queued_size(SDL_AudioDeviceID device) {
    (void)device;
    Uint32 queued = s_web_audio_stage_len;
    for (Uint32 slot = 0; slot < WEB_AUDIO_ASYNC_SLOTS; ++slot) {
        Uint32 bytes = __atomic_load_n(&s_web_audio_slot_len[slot], __ATOMIC_ACQUIRE);
        if (queued > 0x7fffffffu - bytes) return 0x7fffffffu;
        queued += bytes;
    }
    return queued;
}

EMSCRIPTEN_KEEPALIVE Uint32 psx_web_audio_async_drop_count(void) {
    return __atomic_load_n(&s_web_audio_async_drops, __ATOMIC_RELAXED);
}

void psx_web_audio_clear(SDL_AudioDeviceID device) {
    (void)device;
    s_web_audio_stage_len = 0;
    Uint32 generation = __atomic_add_fetch(
        &s_web_audio_generation, 1u, __ATOMIC_ACQ_REL);
    if (generation == 0)
        __atomic_add_fetch(&s_web_audio_generation, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_web_audio_async_drops, 0u, __ATOMIC_RELAXED);
    MAIN_THREAD_EM_ASM({
        var state = Module.psxWebAudio;
        if (!state) return;
        state.sources.forEach(function(source) {
            try { source.stop(); } catch (_) {}
        });
        state.sources.clear();
        state.nextTime = state.context.currentTime;
        state.started = false;
        state.bridgeDrops = 0;
    });
}

void psx_web_audio_close(SDL_AudioDeviceID device) {
    psx_web_audio_clear(device);
}

void psx_web_audio_lock(SDL_AudioDeviceID device) { (void)device; }
void psx_web_audio_unlock(SDL_AudioDeviceID device) { (void)device; }
