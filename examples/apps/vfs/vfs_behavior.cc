// =============================================================================
// vfs_behavior.cc  —  [vfs] behaviour for the vfs app
// =============================================================================
//
// VFS Explorer + Audio Player
// ───────────────────────────
// Demonstrates pce::vfs::Vfs with MemMount, LocalMount, and audio playback.
//
// Two schemes are mounted at startup:
//   asset://  →  <binary-dir>/data/       (LocalMount, read-only in practice)
//   mem://    →  in-memory scratch space  (MemMount, pre-seeded with demo files)
//
// Audio via VFS src=
// ──────────────────
//   The jade scene declares:  audio(id="vfs-audio" src="")
//   When a .wav is selected in the file browser, the behavior writes the VFS
//   URI into that node's src= attribute:
//
//     n->setStyle("src", "asset://audio/click.wav");
//
//   Pressing ▶ reads src= back, calls vfs.read() to get the raw bytes, then
//   feeds them through SDL_LoadWAV_IO → SDL_OpenAudioDeviceStream → play.
//
//   You can also hardcode a URI directly in jade:
//
//     audio(id="vfs-audio" src="asset://audio/ambient.wav")
//
//   jade_host preserves scheme:// URIs unchanged (resolveAssetPaths skips any
//   src= that contains "://"), so the behavior auto-plays it at startup.
//
// Adding more mounts
// ──────────────────
//   Put extra calls inside the "enter the forrest" user region:
//     state->vfs.mount_local("scene", std::string(base) + "scenes");
//     state->vfs.mount("cache", std::make_unique<pce::vfs::MemMount>());
//   The scheme selector rebuilds itself from vfs.schemes() so new chips
//   appear automatically.
//
// Architecture
// ────────────
//   This file is #include-d into jade_host.cc at compile time.
//   All jade_host declarations (jade::parse, bindDrawCallbacks, …) are
//   visible here without extra #includes.
//
// Regeneration
// ────────────
//   sdlos create vfs --overwrite
//   Code between "enter the forrest" / "back to the sea" is preserved.
// =============================================================================

#include "vfs/vfs.h"
#include "vfs/mem_mount.h"
#include "vfs/local_mount.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ── SfxPlayer ─────────────────────────────────────────────────────────────────
//
// Fire-and-forget UI sound effects.  Pre-loads WAV files into memory;
// play() opens a short-lived SDL3 audio stream per invocation so multiple
// overlapping sounds work without a full mixing library.

struct SfxClip {
    SDL_AudioSpec  spec{};
    std::vector<Uint8> pcm;
};

struct SfxPlayer {
    std::unordered_map<std::string, SfxClip> clips;

    bool load(const std::string& name, const std::string& path) {
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (!io) {
            sdlos_log("[sfx] cannot open: " + path + " — " + SDL_GetError());
            return false;
        }

        SDL_AudioSpec spec{};
        Uint8*  buf = nullptr;
        Uint32  len = 0;
        if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len)) {
            sdlos_log("[sfx] load failed: " + path + " — " + SDL_GetError());
            return false;
        }

        SfxClip clip;
        clip.spec = spec;
        clip.pcm.assign(buf, buf + len);
        SDL_free(buf);

        clips[name] = std::move(clip);
        sdlos_log("[sfx] loaded '" + name + "' ← " + path
                  + "  (" + std::to_string(len) + " bytes)");
        return true;
    }

    void play(const std::string& name) {
        auto it = clips.find(name);
        if (it == clips.end()) return;

        const SfxClip& clip = it->second;
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &clip.spec, nullptr, nullptr);
        if (!stream) return;

        SDL_PutAudioStreamData(stream,
            static_cast<const void*>(clip.pcm.data()),
            static_cast<int>(clip.pcm.size()));
        SDL_FlushAudioStream(stream);
        SDL_ResumeAudioStreamDevice(stream);
    }
};

// ── AudioPlayer ───────────────────────────────────────────────────────────────
//
// Owns one SDL3 audio stream.  VFS delivers the raw bytes; SDL3 decodes the
// WAV headers and feeds the PCM data to the default output device.
//
// Usage:
//   auto bytes = state->vfs.read("asset://audio/click.wav");
//   if (bytes) state->audio.play(*bytes);

struct AudioPlayer {
    SDL_AudioStream* stream  = nullptr;
    bool             playing = false;

    /// Load `wav_bytes` (full WAV file) and start playback.
    /// Stops and replaces any currently active stream.
    bool play(const std::vector<std::byte>& wav_bytes) {
        stop();

        SDL_IOStream* io = SDL_IOFromConstMem(
            static_cast<const void*>(wav_bytes.data()), wav_bytes.size());
        if (!io) {
            sdlos_log("[vfs] audio: SDL_IOFromConstMem — " + std::string(SDL_GetError()));
            return false;
        }

        SDL_AudioSpec spec{};
        Uint8*  buf = nullptr;
        Uint32  len = 0;
        // SDL_TRUE in some SDL3 headers expands to a renamed symbol that isn't
        // available in our build configuration; use a plain `true` boolean to
        // avoid depending on the oldnames macro indirection.
        if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len)) {
            sdlos_log("[vfs] audio: SDL_LoadWAV_IO — " + std::string(SDL_GetError()));
            return false;
        }

        stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            SDL_free(buf);
            sdlos_log("[vfs] audio: SDL_OpenAudioDeviceStream — " + std::string(SDL_GetError()));
            return false;
        }

        SDL_PutAudioStreamData(stream,
            static_cast<const void*>(buf), static_cast<int>(len));
        SDL_free(buf);
        SDL_ResumeAudioStreamDevice(stream);
        playing = true;
        return true;
    }

    void stop() {
        if (stream) {
            SDL_PauseAudioStreamDevice(stream);
            SDL_DestroyAudioStream(stream);
            stream  = nullptr;
        }
        playing = false;
    }

    ~AudioPlayer() { stop(); }
};


// State
//
// pce::vfs::Vfs is non-copyable / non-movable; it lives directly in this
// struct which is heap-allocated via std::make_shared, so no copy or move
// ever occurs.

struct VfsState {
    pce::vfs::Vfs vfs;

    std::string active_scheme;   ///< currently selected scheme, e.g. "mem"
    std::string active_path;     ///< sub-directory within the scheme (empty = root)

    // Node handles resolved once in jade_app_init.
    pce::sdlos::NodeHandle schemes_col_h   = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle path_h          = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle file_list_h     = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle content_title_h = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle content_h       = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle status_h        = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle vfs_info_h      = pce::sdlos::k_null_handle;

    // Audio player nodes.
    pce::sdlos::NodeHandle audio_node_h    = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle audio_title_h   = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle audio_badge_h   = pce::sdlos::k_null_handle;

    // Handles to dynamically created scheme chips (used for active highlighting).
    std::vector<pce::sdlos::NodeHandle> scheme_chips;

    AudioPlayer audio;
    SfxPlayer   sfx;
};


// ── Small UI helpers ──────────────────────────────────────────────────────────

static void setLabel(pce::sdlos::RenderTree& tree,
                     pce::sdlos::NodeHandle   h,
                     const std::string&       text)
{
    if (pce::sdlos::RenderNode* n = tree.node(h)) {
        n->setStyle("text", text);
        n->dirty_render = true;
    }
}

static void setStyle(pce::sdlos::RenderTree& tree,
                     pce::sdlos::NodeHandle   h,
                     const std::string&       key,
                     const std::string&       val)
{
    if (pce::sdlos::RenderNode* n = tree.node(h)) {
        n->setStyle(key, val);
        n->dirty_render = true;
    }
}

static void setStatus(pce::sdlos::RenderTree& tree,
                      pce::sdlos::NodeHandle   status_h,
                      const std::string&       msg,
                      bool                     ok)
{
    setLabel(tree, status_h, msg);
    setStyle(tree, status_h, "color", ok ? "#4ade80" : "#f87171");
}

/// Detach and free all direct children of `container`.
static void clearChildren(pce::sdlos::RenderTree& tree,
                          pce::sdlos::NodeHandle   container)
{
    const pce::sdlos::RenderNode* p = tree.node(container);
    if (!p) return;

    // Snapshot the child list before any structural changes.
    std::vector<pce::sdlos::NodeHandle> kids;
    for (pce::sdlos::NodeHandle c = p->child; c.valid(); ) {
        const pce::sdlos::RenderNode* cn = tree.node(c);
        if (!cn) break;
        kids.push_back(c);
        c = cn->sibling;
    }

    for (auto h : kids) {
        tree.detach(h);
        tree.free(h);
    }
}


// Scheme chip utilities

/// Update background/colour of every chip based on `active` scheme.
static void highlightChips(pce::sdlos::RenderTree& tree,
                            const std::vector<pce::sdlos::NodeHandle>& chips,
                            const std::string& active)
{
    for (auto h : chips) {
        if (pce::sdlos::RenderNode* n = tree.node(h)) {
            const bool on = (std::string(n->style("data-value")) == active);
            n->setStyle("backgroundColor", on ? "#6366f133" : "transparent");
            n->setStyle("color",           on ? "#a5b4fc"   : "#94a3b8");
            n->dirty_render = true;
        }
    }
}

/// Rebuild the scheme selector column from the live vfs.schemes() listing.
/// Called once at startup, and again whenever the mount table changes.
static void rebuildSchemeChips(pce::sdlos::RenderTree& tree,
                                pce::sdlos::IEventBus&   bus,
                                VfsState&    state)
{
    clearChildren(tree, state.schemes_col_h);
    state.scheme_chips.clear();

    auto schemes = state.vfs.schemes();
    std::sort(schemes.begin(), schemes.end());

    for (const auto& s : schemes) {
        // Build a minimal jade snippet for one chip.
        const std::string jade_src =
            "div.scheme-chip("
            "data-value=\"" + s + "\" "
            "onclick=\"vfs:scheme\" "
            "text=\"" + s + "://\")";

        const pce::sdlos::NodeHandle chip = pce::sdlos::jade::parse(jade_src, tree);
        if (!chip.valid()) continue;

        pce::sdlos::bindDrawCallbacks(tree, chip);
        pce::sdlos::bindNodeEvents(tree, chip, bus);
        tree.appendChild(state.schemes_col_h, chip);
        state.scheme_chips.push_back(chip);
    }

    // Refresh the mount-count label.
    setLabel(tree, state.vfs_info_h,
             std::to_string(schemes.size()) + " mount" + (schemes.size() == 1 ? "" : "s"));
}


// File list
/// Repopulate the file-entry column for state.active_scheme + state.active_path.
static void populateFileList(pce::sdlos::RenderTree& tree,
                              pce::sdlos::IEventBus&   bus,
                              VfsState&    state)
{
    clearChildren(tree, state.file_list_h);
    if (state.active_scheme.empty()) return;

    const std::string uri = state.active_scheme + "://" + state.active_path;
    auto entries = state.vfs.list(uri);

    if (entries.empty()) {
        const pce::sdlos::NodeHandle h =
            pce::sdlos::jade::parse("div.empty-hint(text=\"(empty)\")", tree);
        if (h.valid()) {
            pce::sdlos::bindDrawCallbacks(tree, h);
            tree.appendChild(state.file_list_h, h);
        }
        return;
    }

    // Directories first, then files — stable so alphabetical order is preserved.
    std::stable_partition(entries.begin(), entries.end(),
        [](const std::string& e) { return !e.empty() && e.back() == '/'; });

    for (const auto& entry : entries) {
        const bool is_dir = !entry.empty() && entry.back() == '/';
        const std::string cls = is_dir ? "file-entry is-dir" : "file-entry";

        // Escape any double-quotes inside the entry name so the jade snippet
        // remains syntactically valid.
        std::string safe = entry;
        for (char& c : safe) if (c == '"') c = '\'';

        const std::string jade_src =
            "div." + cls + "("
            "data-value=\"" + safe + "\" "
            "onclick=\"vfs:open-file\" "
            "text=\"" + safe + "\")";

        const pce::sdlos::NodeHandle h = pce::sdlos::jade::parse(jade_src, tree);
        if (!h.valid()) continue;

        pce::sdlos::bindDrawCallbacks(tree, h);
        pce::sdlos::bindNodeEvents(tree, h, bus);
        tree.appendChild(state.file_list_h, h);
    }
}


// File open / navigation
/// Handle a click on a file-list entry.
///
/// Directories  → push sub-path and re-list.
/// .wav files   → write VFS URI into the audio node's src= and prime the UI.
/// Other files  → read as text and show in the content pane.
static void openEntry(pce::sdlos::RenderTree& tree,
                      pce::sdlos::IEventBus&   bus,
                      VfsState&    state,
                      const std::string&       entry)
{
    if (state.active_scheme.empty()) return;

    // Directory: navigate in.
    if (!entry.empty() && entry.back() == '/') {
        state.active_path += entry;
        setLabel(tree, state.path_h,
                 state.active_scheme + "://" + state.active_path);
        populateFileList(tree, bus, state);
        return;
    }

    const std::string uri =
        state.active_scheme + "://" + state.active_path + entry;

    // .wav → set the audio node's src= attribute to the VFS URI.
    // The ▶ button reads it back, calls vfs.read(), and plays via SDL3.
    const bool is_wav = entry.size() > 4 &&
        (entry.substr(entry.size() - 4) == ".wav" ||
         entry.substr(entry.size() - 4) == ".WAV");

    if (is_wav) {
        if (pce::sdlos::RenderNode* an = tree.node(state.audio_node_h)) {
            an->setStyle("src", uri);   // VFS URI stored on the node
        }
        setLabel(tree, state.audio_title_h, entry);
        setLabel(tree, state.audio_badge_h, "ready");
        setLabel(tree, state.content_title_h, entry + "  \xe2\x80\x94  audio");
        setLabel(tree, state.content_h,
                 "VFS uri: " + uri + "\n\nPress \xe2\x96\xb6 to play via SDL3.\n"
                 "The audio node carries src=\"" + uri + "\"\n"
                 "as a plain style attribute — no engine widget needed.");
        setStatus(tree, state.status_h, "audio ready  \xe2\x80\x94  " + uri, true);
        return;
    }

    // Text file: read and display (truncated if large).
    const auto result = state.vfs.read_text(uri);
    if (result) {
        std::string body = *result;
        if (body.size() > 2048)
            body = body.substr(0, 2048) + "\n\xe2\x80\xa6 [truncated]";
        setLabel(tree, state.content_h, body);
        setLabel(tree, state.content_title_h, entry);
        setStatus(tree, state.status_h, "opened  " + uri, true);
    } else {
        setStatus(tree, state.status_h, "read failed: " + result.error(), false);
    }
}

} // namespace


// ── jade_app_init ─────────────────────────────────────────────────────────────

void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle                 root,
                   pce::sdlos::IEventBus&                 bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& out_handler)
{
    (void)renderer;
    (void)out_handler;

    // jade_host only calls SDL_Init(SDL_INIT_VIDEO | SDL_INIT_CAMERA).
    // Audio needs its own subsystem init (SDL reference-counts these calls).
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    auto state = std::make_shared<VfsState>();

    // ── Load UI sound effects ─────────────────────────────────────────────────
    {
        const char* bp = SDL_GetBasePath();
        const std::string snd = bp ? std::string(bp) + "data/sounds/" : "data/sounds/";
        state->sfx.load("click",  snd + "neueis_7.wav");
        state->sfx.load("nav",    snd + "neueis_8.wav");
        state->sfx.load("action", snd + "neueis_9.wav");
    }

    // VFS mounts

    // asset:// → binary directory / data/
    // CMake copies each app's data/ tree alongside the executable at build time,
    // so "asset://audio/click.wav" resolves to <binary>/data/audio/click.wav.
    const char* base = SDL_GetBasePath();
    if (base) {
        state->vfs.mount_local("asset", std::string(base) + "data");
    }

    // mem:// → ephemeral in-memory mount, pre-seeded with demo text files.
    {
        auto mem = std::make_unique<pce::vfs::MemMount>();
        mem->put_text("readme.txt",
            "Hello from MemMount!\n"
            "This file lives entirely in RAM — nothing touches the disk.\n"
            "\n"
            "Demo ops (right panel):\n"
            "  ✎ write  →  creates mem://demo_write.txt\n"
            "  ↓ read   →  displays it in the content viewer\n"
            "  ✕ delete →  removes it (read will fail after this)\n");
        mem->put_text("config.json",
            "{\n"
            "  \"app\": \"vfs\",\n"
            "  \"template\": \"vfs\",\n"
            "  \"mounts\": [\"asset\", \"mem\"]\n"
            "}\n");
        state->vfs.mount("mem", std::move(mem));
    }

    // User extension point
    // Mount additional schemes here.  Chips rebuild automatically.
    // --- enter the forrest ---

    // state->vfs.mount_local("scene", std::string(base) + "scenes");
    // auto extra = std::make_unique<pce::vfs::MemMount>();
    // state->vfs.mount("extra", std::move(extra));

    // --- back to the sea ---

    // ── Locate nodes ───────────────────────────────────────────────────────────
    state->schemes_col_h   = tree.findById(root, "vfs-schemes");
    state->path_h          = tree.findById(root, "vfs-path");
    state->file_list_h     = tree.findById(root, "vfs-file-list");
    state->content_title_h = tree.findById(root, "vfs-content-title");
    state->content_h       = tree.findById(root, "vfs-content");
    state->status_h        = tree.findById(root, "vfs-status");
    state->vfs_info_h      = tree.findById(root, "vfs-vfs-info");
    state->audio_node_h    = tree.findById(root, "vfs-audio");
    state->audio_title_h   = tree.findById(root, "vfs-audio-title");
    state->audio_badge_h   = tree.findById(root, "vfs-audio-badge");

    sdlos_log("[vfs] nodes — "
              "schemes="      + std::string(state->schemes_col_h.valid()   ? "ok" : "MISSING")
              + " file-list=" + (state->file_list_h.valid()     ? "ok" : "MISSING")
              + " audio="     + (state->audio_node_h.valid()    ? "ok" : "MISSING"));

    // ── Build scheme chips from live vfs.schemes() ────────────────────────────
    rebuildSchemeChips(tree, bus, *state);

    // ── Default view: asset://sounds/ ─────────────────────────────────────────
    // Select the "asset" scheme and navigate into sounds/ so the user sees the
    // list of audio files immediately on launch.
    {
        state->active_scheme = "asset";
        state->active_path   = "sounds/";
        highlightChips(tree, state->scheme_chips, "asset");
        setLabel(tree, state->path_h, "asset://sounds/");
        populateFileList(tree, bus, *state);
    }

    // ── Auto-play a static src= on the audio node (if any) ───────────────────
    // If the jade source has  audio(src="asset://audio/ambient.wav")  the URI
    // is preserved verbatim (resolveAssetPaths skips "://") so we can play it
    // here before the user interacts with the file browser.
    if (state->audio_node_h.valid()) {
        if (const pce::sdlos::RenderNode* an = tree.node(state->audio_node_h)) {
            const std::string src{an->style("src")};
            if (!src.empty()) {
                const auto bytes = state->vfs.read(src);
                if (bytes && state->audio.play(*bytes)) {
                    setLabel(tree, state->audio_title_h, src);
                    setLabel(tree, state->audio_badge_h, "playing");
                    sdlos_log("[vfs] auto-play: " + src);
                }
            }
        }
    }

    // ── Bus subscriptions ──────────────────────────────────────────────────────

    // Scheme chip clicked → list that scheme's root directory.
    bus.subscribe("vfs:scheme",
        [&tree, state, &bus](const std::string& scheme) {
            state->sfx.play("click");
            state->active_scheme = scheme;
            state->active_path   = "";
            highlightChips(tree, state->scheme_chips, scheme);
            setLabel(tree, state->path_h, scheme + "://");
            populateFileList(tree, bus, *state);
            setStatus(tree, state->status_h, "listed  " + scheme + "://", true);
        });

    // File-list entry clicked → navigate or open.
    bus.subscribe("vfs:open-file",
        [&tree, state, &bus](const std::string& entry) {
            state->sfx.play("nav");
            openEntry(tree, bus, *state, entry);
        });

    // Refresh button → re-list current directory.
    bus.subscribe("vfs:refresh",
        [&tree, state, &bus](const std::string&) {
            populateFileList(tree, bus, *state);
            setStatus(tree, state->status_h, "refreshed", true);
        });

    // Demo: write a file to mem:// at runtime.
    bus.subscribe("vfs:demo-write",
        [&tree, state](const std::string&) {
            state->sfx.play("action");
            const auto r = state->vfs.write_text(
                "mem://demo_write.txt",
                "Written at runtime via vfs.write_text()!\n"
                "Select 'mem://' in the scheme list to see it.\n");
            setStatus(tree, state->status_h,
                r ? "wrote  mem://demo_write.txt"
                  : "write failed: " + r.error(),
                r.has_value());
        });

    // Demo: read it back and display in the content pane.
    bus.subscribe("vfs:demo-read",
        [&tree, state](const std::string&) {
            state->sfx.play("action");
            const auto r = state->vfs.read_text("mem://demo_write.txt");
            if (r) {
                setLabel(tree, state->content_h, *r);
                setLabel(tree, state->content_title_h, "demo_write.txt");
                setStatus(tree, state->status_h, "read  mem://demo_write.txt", true);
            } else {
                setStatus(tree, state->status_h,
                          "read failed: " + r.error(), false);
            }
        });

    // Demo: delete it (idempotent — no error if already gone).
    bus.subscribe("vfs:demo-delete",
        [&tree, state](const std::string&) {
            state->sfx.play("action");
            const auto r = state->vfs.remove("mem://demo_write.txt");
            setStatus(tree, state->status_h,
                r ? "deleted  mem://demo_write.txt"
                  : "delete failed: " + r.error(),
                r.has_value());
        });

    // Audio: load src= from the audio node, read bytes via VFS, play via SDL3.
    bus.subscribe("vfs:audio-play",
        [&tree, state](const std::string&) {
            state->sfx.play("click");
            if (!state->audio_node_h.valid()) return;

            const pce::sdlos::RenderNode* an = tree.node(state->audio_node_h);
            if (!an) return;

            const std::string src{an->style("src")};
            if (src.empty()) {
                setStatus(tree, state->status_h,
                          "no audio \xe2\x80\x94 select a .wav in the browser first", false);
                return;
            }

            // VFS: load bytes — works with any mounted scheme (asset://, mem://, …)
            const auto bytes = state->vfs.read(src);
            if (!bytes) {
                setStatus(tree, state->status_h,
                          "audio load failed: " + bytes.error(), false);
                return;
            }

            if (state->audio.play(*bytes)) {
                setLabel(tree, state->audio_badge_h, "playing");
                setStatus(tree, state->status_h, "playing  " + src, true);
            } else {
                setStatus(tree, state->status_h,
                          "SDL audio error: " + std::string(SDL_GetError()), false);
            }
        });

    // Audio: stop the current stream.
    bus.subscribe("vfs:audio-stop",
        [&tree, state](const std::string&) {
            state->sfx.play("click");
            state->audio.stop();
            setLabel(tree, state->audio_badge_h, "stopped");
            setStatus(tree, state->status_h, "audio stopped", true);
        });

    sdlos_log("[vfs] ready — "
              + std::to_string(state->vfs.schemes().size()) + " vfs mounts");
}
