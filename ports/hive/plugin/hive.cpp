// Hive: a note sequencer on a hexagonal grid inside a hexagon (CLAP feature "note-effect"). Spawn tiles start cursors
// on a pulse or on incoming notes, cursors walk the tiles' arrows one step at a time, tiles play notes when a cursor
// enters, sinks swallow them. Runs on the host's beat position. openDAW shows the page in its device panel (feature
// "opendaw:device-panel", extension "opendaw.device-panel/1"); the panel, the page's shared messages and the gestures
// come from modules/opendaw-clap/opendaw_page.h, the panel's controls from modules/opendaw-page.

#include <clap/clap.h>
#include "opendaw_page.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void emscripten_notify_memory_growth(size_t index) {(void) index;}

enum : clap_id {PARAM_PATTERN = 0, PARAM_GATE = 1, PARAM_TRANSPOSE = 2, PARAM_RATE = 3, PARAM_COUNT = 4};
enum : uint8_t {KIND_PATH = 0, KIND_SPAWN = 1, KIND_SINK = 2, KIND_TURN = 3, KIND_SPLIT = 4, KIND_RANDOM = 5, KIND_COUNT = 6};
enum : uint8_t {EDGE_WRAP = 0, EDGE_BOUNCE = 1, EDGE_DIE = 2, EDGE_COUNT = 3};
enum : uint8_t {TRIGGER_PULSE = 0, TRIGGER_NOTE = 1, TRIGGER_BOTH = 2, TRIGGER_COUNT = 3};

#define MAX_N 8
#define MAX_R (MAX_N - 1)
#define SPAN (2 * MAX_R + 1)
#define TILE_COUNT 169 // 3 * 8 * 7 + 1
#define PATTERN_COUNT 8
#define MAX_CURSORS 64
#define PPQ 384 // fine enough for a 1/32 at a rate of 1/8 and 8/1 to stay whole ticks
#define BAR_TICKS (PPQ * 4)
#define DURATION_COUNT 9
#define DURATION_DEFAULT 2 // 1/16
#define GRID_TICKS (PPQ / 4) // incoming notes and spawn offsets in 1/16
#define RATE_STEPS 7
#define RATE_DEFAULT 3 // 1/1
#define RATE_COUNT 6
#define SCALE_COUNT 6
#define PENDING_NOTES 32
#define SCHEDULED 2048
#define EDITS 256
#define EDGE_HITS 32
#define OUT_EVENTS 64
#define KEYS 128
#define DIR_KEEP 255
#define PITCH_AUTO 255
#define KEY_ANY 255
#define FLAG_NOTE 1
#define FLAG_HOLD 2
#define GUI_WIDTH 1000
#define GUI_HEIGHT 640
#define STATE_VERSION 3

// the device panel at 176 px: the hex grid (160 px) and the controls, as page/index.html lays them out
#define PANEL_WIDTH 266

// Hive's own messages, see page/index.html; the shared ones (poll, parameters, hover) are opendaw_page.h's
#define PAGE_HELLO 8
#define PAGE_SHOW_WINDOW 9
#define PAGE_TILE 10
#define PAGE_HEADER 11
#define PAGE_PATTERN 12
#define PAGE_COMMIT 13
#define PAGE_LIVE 1
#define PLUGIN_SNAPSHOT 1
#define PLUGIN_DELTA 2
#define DELTA_PLAYING 1
#define EDIT_TILE 0
#define EDIT_HEADER 1

// one tile, also its layout in the saved state and in the page's messages (16 bytes)
struct Tile {
    uint8_t kind;
    uint8_t flags;    // FLAG_NOTE, FLAG_HOLD
    uint8_t dir;      // 0..5 (E NE NW W SW SE), DIR_KEEP
    int8_t turn;      // 2, 1, -1, -2, 3 (60° steps, counter clockwise)
    uint8_t fork;     // split: a bit per direction
    uint8_t pitch;    // 0..127, PITCH_AUTO
    uint8_t velocity; // 1..127
    uint8_t repeat;   // 1..8
    uint8_t chance;   // 0..100
    uint8_t trigger;
    uint8_t rate;     // index into rate_ticks
    uint8_t offset;   // steps, 0..63
    uint8_t life;     // steps, 0 = endless
    uint8_t key;      // 0..127, KEY_ANY
    uint8_t duration; // how long a cursor stays, index into duration_ticks
    uint8_t reserved;
};
static_assert(sizeof(Tile) == 16, "tile layout");

struct Header {
    uint8_t size;  // tiles on an edge, 2..8
    uint8_t edge;
    uint8_t root;  // midi key
    uint8_t scale;
    uint8_t reserved[4];
};
static_assert(sizeof(Header) == 8, "header layout");

struct Pattern {
    Header header;
    Tile tiles[TILE_COUNT];
};
static_assert(sizeof(Pattern) == 8 + TILE_COUNT * 16, "pattern layout");

#define STATE_HEADER 16
#define STATE_BYTES (STATE_HEADER + PATTERN_COUNT * sizeof(Pattern))

struct Cursor {
    int8_t q, r;
    uint8_t heading;
    uint8_t life;
    uint16_t age;
    bool hold;
    uint8_t key;
    int8_t transpose;
    float velocity;
    int64_t leaves; // the tick it moves on: its arrival plus the tile's duration
};

struct Scheduled {
    uint64_t time;
    bool on;
    uint8_t key;
    float velocity;
};

struct PendingNote {
    uint8_t key;
    float velocity;
};

// a cursor that reached the edge: where it was, where it headed, what the edge did (the page sends particles)
struct EdgeHit {
    int8_t q, r;
    uint8_t heading;
    uint8_t edge;
};

struct Edit {
    uint8_t pattern;
    uint8_t type;
    uint16_t index;
};

struct Hive {
    clap_plugin_t plugin;
    const clap_host_t *host;
    opendaw::PageHost page; // the host's params, state, webview and hovered parameter
    const clap_host_gui_t *host_gui;
    Pattern patterns[PATTERN_COUNT];
    uint8_t pattern, gate, rate; // the parameters
    int8_t transpose;
    uint8_t active; // the pattern playing: follows the parameter at the next bar
    double sample_rate;
    uint64_t now;
    bool playing;
    int64_t last_tick; // the last tick run: a host that warps time (a groove before us) may hand us ranges that overlap
    Cursor cursors[MAX_CURSORS];
    uint32_t cursor_count;
    int16_t spawners[TILE_COUNT];
    uint32_t spawner_count;
    bool spawners_dirty;
    Scheduled scheduled[SCHEDULED];
    uint32_t scheduled_count;
    uint8_t sounding[KEYS];
    bool down[KEYS];
    PendingNote pending[PENDING_NOTES];
    uint32_t pending_count;
    uint8_t flashes[(TILE_COUNT + 7) / 8];
    EdgeHit edge_hits[EDGE_HITS];
    uint32_t edge_hit_count;
    // page traffic
    Edit edits[EDITS];
    uint32_t edit_count;
    uint32_t changed;
    bool snapshot_due;
    bool status_due;
    opendaw::OutEvents<OUT_EVENTS> out_events; // the page's gestures and values for the host
};

// cells: axial (q, r) within MAX_R of the centre, an index per cell, a lookup per coordinate
static int8_t cell_q[TILE_COUNT], cell_r[TILE_COUNT];
static int16_t cell_index[SPAN][SPAN];
static const int8_t dir_q[6] = {1, 1, 0, -1, -1, 0};
static const int8_t dir_r[6] = {0, -1, -1, 0, 1, 1};
// how long a cursor stays on a tile: 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2, 1 bar
static const uint16_t duration_ticks[DURATION_COUNT] = {48, 64, 96, 128, 192, 256, 384, 768, 1536};
static const uint16_t rate_ticks[RATE_COUNT] = {384, 768, 1536, 3072, 6144, 12288};
// the global rate as a fraction: how fast cursors walk, the tiles' durations divided by it
static const uint8_t rate_numerators[RATE_STEPS] = {1, 1, 1, 1, 2, 4, 8};
static const uint8_t rate_denominators[RATE_STEPS] = {8, 4, 2, 1, 1, 1, 1};
// scales as bits per semitone above the root
static const uint16_t scales[SCALE_COUNT] = {
    0xfff,                                                                   // chromatic
    1 << 0 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 11,           // major
    1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 10,           // minor
    1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10,           // dorian
    1 << 0 | 1 << 2 | 1 << 4 | 1 << 7 | 1 << 9,                              // pentatonic major
    1 << 0 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 10                              // pentatonic minor
};

static void build_cells() {
    static bool built = false;
    if (built) {return;}
    built = true;
    for (int q = 0; q < SPAN; q++) {for (int r = 0; r < SPAN; r++) {cell_index[q][r] = -1;}}
    int index = 0;
    for (int q = -MAX_R; q <= MAX_R; q++) {
        const int from = q < 0 ? -MAX_R - q : -MAX_R, to = q < 0 ? MAX_R : MAX_R - q;
        for (int r = from; r <= to; r++) {
            cell_q[index] = static_cast<int8_t>(q);
            cell_r[index] = static_cast<int8_t>(r);
            cell_index[q + MAX_R][r + MAX_R] = static_cast<int16_t>(index++);
        }
    }
}

static int distance(int q, int r) {return (std::abs(q) + std::abs(r) + std::abs(q + r)) / 2;}

static int index_of(int q, int r) {
    if (q < -MAX_R || q > MAX_R || r < -MAX_R || r > MAX_R) {return -1;}
    return cell_index[q + MAX_R][r + MAX_R];
}

static Hive *hive_of(const clap_plugin_t *plugin) {return static_cast<Hive *>(plugin->plugin_data);}

static const clap_plugin_descriptor_t *descriptor_ptr();

// ------------------------------------------------------------------------------------------------ the model

static Tile default_tile() {
    Tile tile = {};
    tile.kind = KIND_PATH;
    tile.dir = DIR_KEEP;
    tile.turn = 1;
    tile.fork = 1 << 1 | 1 << 5;
    tile.pitch = PITCH_AUTO;
    tile.velocity = 100;
    tile.repeat = 1;
    tile.chance = 100;
    tile.trigger = TRIGGER_PULSE;
    tile.rate = 2;
    tile.life = 16;
    tile.key = KEY_ANY;
    tile.duration = DURATION_DEFAULT;
    return tile;
}

static void clear_pattern(Pattern &pattern) {
    pattern.header = Header{5, EDGE_WRAP, 48, 5, {0, 0, 0, 0}};
    for (Tile &tile : pattern.tiles) {tile = default_tile();}
}

static Tile &tile_at(Pattern &pattern, int q, int r) {return pattern.tiles[index_of(q, r)];}

// the cells of a ring, each with the direction to the next, so a ring is a closed path
template<typename F>
static void walk_ring(int radius, F visit) {
    int q = dir_q[4] * radius, r = dir_r[4] * radius;
    for (int side = 0; side < 6; side++) {
        for (int step = 0; step < radius; step++) {
            visit(q, r, side);
            q += dir_q[side];
            r += dir_r[side];
        }
    }
}

static void default_patterns(Hive *hive) {
    for (Pattern &pattern : hive->patterns) {clear_pattern(pattern);}
    { // 1: a one-bar groove. The centre spawns a cursor that circles a ring of twelve notes (12 steps against the
      // bar's 16), a split sends a branch to a corner where it dies, a bass walks the bottom row in quarters into a
      // sink, a high ratchet fills every half bar off the beat (and on incoming notes)
        Pattern &p = hive->patterns[0];
        p.header.edge = EDGE_DIE;
        Tile &centre = tile_at(p, 0, 0);
        centre.kind = KIND_SPAWN; centre.dir = 0; centre.rate = 2; centre.life = 28;
        Tile &exit = tile_at(p, 1, 0);
        exit.dir = 0; exit.duration = 4; // an eighth's pause before the ring
        int step = 0;
        walk_ring(2, [&](int q, int r, int side) {
            Tile &tile = tile_at(p, q, r);
            tile.dir = static_cast<uint8_t>(side);
            tile.flags |= FLAG_NOTE;
            if (step % 3 == 2) {tile.chance = 70;}
            step++;
        });
        tile_at(p, 0, 2).repeat = 2;
        Tile &split = tile_at(p, 2, -2);
        split.kind = KIND_SPLIT; split.fork = 1 << 3 | 1 << 1; // on along the ring, a branch to the corner
        Tile &branch = tile_at(p, 3, -3);
        branch.dir = 1; branch.flags |= FLAG_NOTE; branch.velocity = 80;
        Tile &corner = tile_at(p, 4, -4);
        corner.dir = 1; corner.flags |= FLAG_NOTE; corner.velocity = 64; corner.duration = 4;
        const uint8_t bass[4] = {36, 43, 39, 41}; // C2 G2 D#2 F2
        for (int i = 0; i < 4; i++) {
            Tile &tile = tile_at(p, -4 + i, 4);
            tile.dir = 0; tile.flags |= FLAG_NOTE; tile.pitch = bass[i]; tile.velocity = 110; tile.duration = 6;
        }
        Tile &bassSpawn = tile_at(p, -4, 4);
        bassSpawn.kind = KIND_SPAWN; bassSpawn.rate = 2; bassSpawn.life = 4;
        tile_at(p, 0, 4).kind = KIND_SINK;
        Tile &fills = tile_at(p, 4, 0);
        fills.kind = KIND_SPAWN; fills.dir = 3; fills.rate = 1; fills.offset = 6; fills.life = 2; fills.trigger = TRIGGER_BOTH;
        Tile &ratchet = tile_at(p, 3, 0);
        ratchet.dir = 3; ratchet.flags |= FLAG_NOTE; ratchet.pitch = 72; ratchet.repeat = 3; ratchet.chance = 70; ratchet.velocity = 70;
    }
    { // 2: a random walker around the centre
        Pattern &p = hive->patterns[1];
        p.header.scale = 3;
        Tile &centre = tile_at(p, 0, 0);
        centre.kind = KIND_SPAWN; centre.dir = 1; centre.rate = 1; centre.life = 12;
        walk_ring(1, [&](int q, int r, int) {
            Tile &tile = tile_at(p, q, r);
            tile.kind = KIND_RANDOM; tile.flags |= FLAG_NOTE;
        });
        walk_ring(3, [&](int q, int r, int side) {
            Tile &tile = tile_at(p, q, r);
            tile.dir = static_cast<uint8_t>((side + 1) % 6);
            if ((q + r) % 2 == 0) {tile.flags |= FLAG_NOTE;}
        });
    }
}

static uint8_t clamp_u8(int value, int min, int max) {return static_cast<uint8_t>(value < min ? min : value > max ? max : value);}

static void sanitize_tile(Tile &tile) {
    if (tile.kind >= KIND_COUNT) {tile.kind = KIND_PATH;}
    tile.flags &= FLAG_NOTE | FLAG_HOLD;
    if (tile.dir > 5) {tile.dir = DIR_KEEP;}
    if (tile.turn != 1 && tile.turn != 2 && tile.turn != -1 && tile.turn != -2 && tile.turn != 3) {tile.turn = 1;}
    tile.fork &= 63;
    if (tile.pitch > 127) {tile.pitch = PITCH_AUTO;}
    tile.velocity = clamp_u8(tile.velocity, 1, 127);
    tile.repeat = clamp_u8(tile.repeat, 1, 8);
    tile.chance = clamp_u8(tile.chance, 0, 100);
    if (tile.trigger >= TRIGGER_COUNT) {tile.trigger = TRIGGER_PULSE;}
    if (tile.rate >= RATE_COUNT) {tile.rate = 2;}
    tile.offset = clamp_u8(tile.offset, 0, 63);
    if (tile.key > 127) {tile.key = KEY_ANY;}
    if (tile.duration >= DURATION_COUNT) {tile.duration = DURATION_DEFAULT;}
    tile.reserved = 0;
}

static void sanitize_header(Header &header) {
    header.size = clamp_u8(header.size, 2, MAX_N);
    if (header.edge >= EDGE_COUNT) {header.edge = EDGE_WRAP;}
    header.root = clamp_u8(header.root, 0, 127);
    if (header.scale >= SCALE_COUNT) {header.scale = 0;}
    std::memset(header.reserved, 0, sizeof(header.reserved));
}

// ------------------------------------------------------------------------------------------------ parameters

static double min_of(clap_id id) {
    switch (id) {
        case PARAM_GATE: return 5.0;
        case PARAM_TRANSPOSE: return -24.0;
        default: return 0.0;
    }
}

static double max_of(clap_id id) {
    switch (id) {
        case PARAM_PATTERN: return PATTERN_COUNT - 1;
        case PARAM_GATE: return 100.0;
        case PARAM_RATE: return RATE_STEPS - 1;
        default: return 24.0;
    }
}

static double default_of(clap_id id) {
    switch (id) {
        case PARAM_GATE: return 80.0;
        case PARAM_RATE: return RATE_DEFAULT;
        default: return 0.0;
    }
}

static double clamp_value(clap_id id, double value) {
    if (!std::isfinite(value)) {value = default_of(id);}
    return std::round(std::fmin(max_of(id), std::fmax(min_of(id), value)));
}

static double value_of(const Hive *hive, clap_id id) {
    switch (id) {
        case PARAM_PATTERN: return hive->pattern;
        case PARAM_GATE: return hive->gate;
        case PARAM_RATE: return hive->rate;
        default: return hive->transpose;
    }
}

static void set_value(Hive *hive, clap_id id, double value) {
    if (id >= PARAM_COUNT) {return;}
    const double clamped = clamp_value(id, value);
    if (clamped == value_of(hive, id)) {return;}
    switch (id) {
        case PARAM_PATTERN: hive->pattern = static_cast<uint8_t>(clamped); break;
        case PARAM_GATE: hive->gate = static_cast<uint8_t>(clamped); break;
        case PARAM_RATE: hive->rate = static_cast<uint8_t>(clamped); break;
        default: hive->transpose = static_cast<int8_t>(clamped); break;
    }
    hive->changed |= 1u << id;
    // stopped, a pattern pick shows at once; playing, it waits for the bar
    if (id == PARAM_PATTERN && !hive->playing && hive->active != hive->pattern) {
        hive->active = hive->pattern;
        hive->spawners_dirty = true;
        hive->status_due = true;
    }
}

// ------------------------------------------------------------------------------------------------ notes out

static void push_note(const clap_output_events_t *out, uint32_t time, bool on, int16_t key, double velocity) {
    clap_event_note_t note = {};
    note.header = {sizeof(note), time, CLAP_CORE_EVENT_SPACE_ID, static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
    note.note_id = -1;
    note.port_index = 0;
    note.channel = 0;
    note.key = key;
    note.velocity = velocity;
    out->try_push(out, &note.header);
}

static void insert_scheduled(Hive *hive, const Scheduled &event) {
    uint32_t index = hive->scheduled_count;
    while (index > 0 && hive->scheduled[index - 1].time > event.time) {
        hive->scheduled[index] = hive->scheduled[index - 1];
        index--;
    }
    hive->scheduled[index] = event;
    hive->scheduled_count++;
}

// two cursors playing the same key on the same sample play it once
static void schedule_note(Hive *hive, uint64_t time, uint64_t length, uint8_t key, float velocity) {
    if (hive->scheduled_count + 2 > SCHEDULED) {return;}
    for (uint32_t index = 0; index < hive->scheduled_count; index++) {
        const Scheduled &event = hive->scheduled[index];
        if (event.on && event.key == key && event.time == time) {return;}
    }
    insert_scheduled(hive, Scheduled{time, true, key, velocity});
    insert_scheduled(hive, Scheduled{time + (length < 1 ? 1 : length), false, key, 0.0f});
}

// a key that sounds already is re-attacked, it ends once every note on it ended
static void fire_scheduled(Hive *hive, const clap_output_events_t *out, uint64_t until) {
    uint32_t fired = 0;
    while (fired < hive->scheduled_count && hive->scheduled[fired].time <= until) {
        const Scheduled &event = hive->scheduled[fired++];
        const uint32_t offset = event.time > hive->now ? static_cast<uint32_t>(event.time - hive->now) : 0;
        if (event.on) {
            if (hive->sounding[event.key] > 0) {push_note(out, offset, false, event.key, 0.0);}
            push_note(out, offset, true, event.key, event.velocity);
            if (hive->sounding[event.key] < 255) {hive->sounding[event.key]++;}
        } else if (hive->sounding[event.key] > 0 && --hive->sounding[event.key] == 0) {
            push_note(out, offset, false, event.key, 0.0);
        }
    }
    if (fired == 0) {return;}
    std::memmove(hive->scheduled, hive->scheduled + fired, (hive->scheduled_count - fired) * sizeof(Scheduled));
    hive->scheduled_count -= fired;
}

static void release_all(Hive *hive, const clap_output_events_t *out) {
    for (int16_t key = 0; key < KEYS; key++) {
        if (hive->sounding[key] == 0) {continue;}
        hive->sounding[key] = 0;
        if (out != nullptr) {push_note(out, 0, false, key, 0.0);}
    }
    hive->scheduled_count = 0;
}

// the host releases what it got from us when it resets, deactivates or stops us, we only forget
static void forget(Hive *hive) {
    std::memset(hive->sounding, 0, sizeof(hive->sounding));
    std::memset(hive->down, 0, sizeof(hive->down));
    hive->scheduled_count = 0;
    hive->cursor_count = 0;
    hive->pending_count = 0;
    hive->playing = false;
    hive->status_due = true;
}

// ------------------------------------------------------------------------------------------------ the engine

// repeatable randomness: a hash of where and when, so a pattern plays the same every time from the same position
static double random_at(int64_t tick, uint32_t a, uint32_t b) {
    uint32_t x = static_cast<uint32_t>(tick) * 0x9e3779b1u ^ a * 0x85ebca77u ^ b * 0xc2b2ae3du;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x / 4294967296.0;
}

static bool inside(const Pattern &pattern, int q, int r) {return distance(q, r) <= pattern.header.size - 1;}

static int auto_pitch(int q, int r, int root) {return root + (((7 * q + 4 * r) % 12) + 12) % 12;}

static int quantize(int pitch, int root, uint8_t scale) {
    const int rel = pitch - root;
    const int octave = rel >= 0 ? rel / 12 : -((11 - rel) / 12);
    const int pc = rel - octave * 12;
    int best = 0;
    for (int degree = 0; degree <= pc; degree++) {if (scales[scale] & (1 << degree)) {best = degree;}}
    return root + octave * 12 + best;
}

static void rebuild_spawners(Hive *hive) {
    const Pattern &pattern = hive->patterns[hive->active];
    hive->spawner_count = 0;
    for (int index = 0; index < TILE_COUNT; index++) {
        if (pattern.tiles[index].kind == KIND_SPAWN && inside(pattern, cell_q[index], cell_r[index])) {
            hive->spawners[hive->spawner_count++] = static_cast<int16_t>(index);
        }
    }
    hive->spawners_dirty = false;
}

// where a cursor goes from (q, r): false when it leaves a grid that ends at its edge
static bool move(Hive *hive, const Pattern &pattern, int q, int r, uint8_t heading, Cursor &to) {
    int nq = q + dir_q[heading], nr = r + dir_r[heading];
    if (!inside(pattern, nq, nr)) {
        if (hive->edge_hit_count < EDGE_HITS) {
            hive->edge_hits[hive->edge_hit_count++] = EdgeHit{static_cast<int8_t>(q), static_cast<int8_t>(r), heading, pattern.header.edge};
        }
        if (pattern.header.edge == EDGE_DIE) {return false;}
        if (pattern.header.edge == EDGE_BOUNCE) {
            heading = static_cast<uint8_t>((heading + 3) % 6);
            nq = q + dir_q[heading];
            nr = r + dir_r[heading];
            if (!inside(pattern, nq, nr)) {nq = q; nr = r;}
        } else { // wrap: re-enter on the far side of the same line
            nq = q; nr = r;
            while (inside(pattern, nq - dir_q[heading], nr - dir_r[heading])) {nq -= dir_q[heading]; nr -= dir_r[heading];}
        }
    }
    to.q = static_cast<int8_t>(nq);
    to.r = static_cast<int8_t>(nr);
    to.heading = heading;
    return true;
}

// a cursor enters a tile: it plays the tile's note (its repeats share the tile's duration), it stays for the tile's
// duration, a sink swallows it (false)
static bool arrive(Hive *hive, Cursor &cursor, int64_t tick, uint32_t salt, uint64_t time, double samples_per_tick) {
    const Pattern &pattern = hive->patterns[hive->active];
    const int index = index_of(cursor.q, cursor.r);
    const Tile &tile = pattern.tiles[index];
    const int64_t stay = duration_ticks[tile.duration] * rate_denominators[hive->rate] / rate_numerators[hive->rate];
    cursor.leaves = tick + stay;
    if ((tile.flags & FLAG_NOTE) && random_at(tick, salt, static_cast<uint32_t>(index)) * 100.0 < tile.chance) {
        const int base = tile.pitch == PITCH_AUTO ? auto_pitch(cursor.q, cursor.r, pattern.header.root) : tile.pitch;
        const int pitch = quantize(base + cursor.transpose + hive->transpose, pattern.header.root, pattern.header.scale);
        if (pitch >= 0 && pitch < KEYS) {
            const double slice = stay * samples_per_tick / tile.repeat;
            const uint64_t length = static_cast<uint64_t>(slice * hive->gate / 100.0);
            const float velocity = static_cast<float>(tile.velocity / 127.0 * cursor.velocity);
            for (int repeat = 0; repeat < tile.repeat; repeat++) {
                schedule_note(hive, time + static_cast<uint64_t>(repeat * slice), length, static_cast<uint8_t>(pitch), velocity);
            }
            hive->flashes[index >> 3] |= static_cast<uint8_t>(1 << (index & 7));
        }
    }
    return tile.kind != KIND_SINK;
}

static void spawn(Hive *hive, int index, int64_t tick, uint64_t time, double samples_per_tick, int8_t transpose, float velocity,
                  uint8_t key) {
    if (hive->cursor_count >= MAX_CURSORS) {return;}
    const Tile &tile = hive->patterns[hive->active].tiles[index];
    Cursor cursor = {};
    cursor.q = cell_q[index];
    cursor.r = cell_r[index];
    cursor.heading = tile.dir == DIR_KEEP ? 0 : tile.dir;
    cursor.life = tile.life;
    cursor.hold = (tile.flags & FLAG_HOLD) && key != KEY_ANY;
    cursor.key = key;
    cursor.transpose = transpose;
    cursor.velocity = velocity;
    if (arrive(hive, cursor, tick, 1000 + hive->cursor_count, time, samples_per_tick)) {hive->cursors[hive->cursor_count++] = cursor;}
}

static void run_tick(Hive *hive, int64_t tick, uint64_t time, double samples_per_tick) {
    if (tick % BAR_TICKS == 0 && hive->active != hive->pattern) { // pattern changes land on the bar
        hive->active = hive->pattern;
        hive->cursor_count = 0;
        hive->spawners_dirty = true;
        hive->status_due = true;
    }
    if (hive->spawners_dirty) {rebuild_spawners(hive);}
    const Pattern &pattern = hive->patterns[hive->active];
    // the cursors whose time on their tile is up move on, the others stay
    Cursor next[MAX_CURSORS];
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < hive->cursor_count; slot++) {
        const Cursor &cursor = hive->cursors[slot];
        if (!inside(pattern, cursor.q, cursor.r)) {continue;}
        if (cursor.leaves > tick) {
            next[count++] = cursor;
            continue;
        }
        if (cursor.hold ? !hive->down[cursor.key] : cursor.life > 0 && cursor.age + 1 > cursor.life) {continue;}
        const Tile &tile = pattern.tiles[index_of(cursor.q, cursor.r)];
        uint8_t exits[6];
        int exit_count = 0;
        switch (tile.kind) {
            case KIND_TURN: exits[exit_count++] = static_cast<uint8_t>((cursor.heading + tile.turn + 6) % 6); break;
            case KIND_SPLIT:
                for (uint8_t dir = 0; dir < 6; dir++) {if (tile.fork & (1 << dir)) {exits[exit_count++] = dir;}}
                if (exit_count == 0) {exits[exit_count++] = cursor.heading;}
                break;
            case KIND_RANDOM:
                exits[exit_count++] = static_cast<uint8_t>(random_at(tick, slot, 77) * 6.0);
                break;
            default: exits[exit_count++] = tile.dir == DIR_KEEP ? cursor.heading : tile.dir; break;
        }
        for (int exit = 0; exit < exit_count && count < MAX_CURSORS; exit++) {
            Cursor moved = cursor;
            if (!move(hive, pattern, cursor.q, cursor.r, exits[exit], moved)) {continue;}
            moved.age = static_cast<uint16_t>(cursor.age < 65535 ? cursor.age + 1 : cursor.age);
            if (arrive(hive, moved, tick, slot * 8 + exit, time, samples_per_tick)) {next[count++] = moved;}
        }
    }
    std::memcpy(hive->cursors, next, count * sizeof(Cursor));
    hive->cursor_count = count;
    // incoming notes start cursors on the next 1/16 (no swing of our own: openDAW's Zeitgeist after us grooves the notes)
    if (tick % GRID_TICKS == 0) {
        for (uint32_t note = 0; note < hive->pending_count; note++) {
            const PendingNote &pending = hive->pending[note];
            for (uint32_t s = 0; s < hive->spawner_count; s++) {
                const Tile &tile = pattern.tiles[hive->spawners[s]];
                if (tile.trigger == TRIGGER_PULSE || (tile.key != KEY_ANY && tile.key != pending.key)) {continue;}
                spawn(hive, hive->spawners[s], tick, time, samples_per_tick, static_cast<int8_t>(pending.key - 60),
                      pending.velocity, pending.key);
            }
        }
        hive->pending_count = 0;
    }
    for (uint32_t s = 0; s < hive->spawner_count; s++) {
        const Tile &tile = pattern.tiles[hive->spawners[s]];
        if (tile.trigger == TRIGGER_NOTE) {continue;}
        const int64_t period = rate_ticks[tile.rate];
        const int64_t phase = ((tick - static_cast<int64_t>(tile.offset) * GRID_TICKS) % period + period) % period;
        if (phase == 0) {spawn(hive, hive->spawners[s], tick, time, samples_per_tick, 0, 1.0f, KEY_ANY);}
    }
}

static void note_in(Hive *hive, int key, double velocity, bool on) {
    if (key < 0 || key >= KEYS) {return;}
    hive->down[key] = on;
    if (on && hive->playing && hive->pending_count < PENDING_NOTES) {
        hive->pending[hive->pending_count++] = PendingNote{static_cast<uint8_t>(key), static_cast<float>(velocity)};
    }
}

static void handle_event(Hive *hive, const clap_event_header_t *header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {return;}
    switch (header->type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            const clap_event_note_t *note = reinterpret_cast<const clap_event_note_t *>(header);
            note_in(hive, note->key, note->velocity, header->type == CLAP_EVENT_NOTE_ON && note->velocity > 0.0);
            return;
        }
        case CLAP_EVENT_MIDI: {
            const clap_event_midi_t *midi = reinterpret_cast<const clap_event_midi_t *>(header);
            const uint8_t status = midi->data[0] & 0xf0;
            if (status == 0x90 || status == 0x80) {
                note_in(hive, midi->data[1] & 0x7f, midi->data[2] / 127.0, status == 0x90 && midi->data[2] > 0);
            }
            return;
        }
        case CLAP_EVENT_PARAM_VALUE: {
            const clap_event_param_value_t *param = reinterpret_cast<const clap_event_param_value_t *>(header);
            set_value(hive, param->param_id, param->value);
            return;
        }
        default:
            return;
    }
}


// ------------------------------------------------------------------------------------------------ plugin

static bool hive_init(const clap_plugin_t *plugin) {
    Hive *hive = hive_of(plugin);
    const clap_host_t *host = hive->host;
    hive->page.init(host);
    hive->host_gui = static_cast<const clap_host_gui_t *>(host->get_extension(host, CLAP_EXT_GUI));
    return true;
}

static void hive_destroy(const clap_plugin_t *plugin) {
    Hive *hive = hive_of(plugin);
    hive->~Hive();
    std::free(hive);
}

static bool hive_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    (void) min_frames; (void) max_frames;
    Hive *hive = hive_of(plugin);
    hive->sample_rate = sample_rate;
    hive->now = 0;
    forget(hive);
    return true;
}

static void hive_deactivate(const clap_plugin_t *plugin) {forget(hive_of(plugin));}

static bool hive_start_processing(const clap_plugin_t *plugin) {(void) plugin; return true;}

static void hive_stop_processing(const clap_plugin_t *plugin) {forget(hive_of(plugin));}

static void hive_reset(const clap_plugin_t *plugin) {forget(hive_of(plugin));}

static clap_process_status hive_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    Hive *hive = hive_of(plugin);
    const uint32_t frames = process->frames_count;
    const clap_output_events_t *out = process->out_events;
    const clap_input_events_t *in = process->in_events;
    const uint32_t count = in->size(in);
    hive->out_events.drain(out);
    const clap_event_transport_t *transport = process->transport;
    const uint32_t needed = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TEMPO;
    const bool playing = transport != nullptr && (transport->flags & needed) == needed && transport->tempo > 0.0;
    uint32_t event = 0;
    if (playing) {
        const double tick0 = static_cast<double>(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR * PPQ;
        const double ticks_per_sample = transport->tempo / 60.0 * PPQ / hive->sample_rate;
        // started or jumped (more than a beat off): the cursors start over, what sounds ends. Less than that
        // carries on from the last tick run, so a groove that shifts our ranges a little neither skips nor repeats
        if (!hive->playing || tick0 > hive->last_tick + 1 + PPQ || tick0 < hive->last_tick - PPQ) {
            release_all(hive, out);
            hive->cursor_count = 0;
            if (!hive->playing) {hive->pending_count = 0;}
            hive->playing = true;
            hive->status_due = true;
            if (hive->active != hive->pattern) {
                hive->active = hive->pattern;
                hive->spawners_dirty = true;
            }
            hive->last_tick = static_cast<int64_t>(std::ceil(tick0 - 1e-6)) - 1;
        }
        for (int64_t tick = hive->last_tick + 1; ; tick++) {
            const double offset = (static_cast<double>(tick) - tick0) / ticks_per_sample;
            if (offset >= frames) {break;}
            hive->last_tick = tick;
            // a tick on a sample stays on it, whatever the beat position's rounding
            const uint32_t at = offset > 0.0 ? static_cast<uint32_t>(std::floor(offset + 1e-4)) : 0;
            for (; event < count; event++) {
                const clap_event_header_t *header = in->get(in, event);
                if (header->time > at) {break;}
                handle_event(hive, header);
            }
            run_tick(hive, tick, hive->now + at, 1.0 / ticks_per_sample);
        }
    } else if (hive->playing) {
        release_all(hive, out);
        hive->playing = false;
        hive->cursor_count = 0;
        hive->pending_count = 0;
        hive->status_due = true;
    }
    for (; event < count; event++) {handle_event(hive, in->get(in, event));}
    if (!hive->playing && hive->active != hive->pattern) {
        hive->active = hive->pattern;
        hive->spawners_dirty = true;
        hive->status_due = true;
    }
    if (frames > 0) {fire_scheduled(hive, out, hive->now + frames - 1);}
    hive->now += frames;
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input) {(void) plugin; (void) is_input; return 0;}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void) plugin; (void) index; (void) is_input; (void) info;
    return false;
}

static const clap_plugin_audio_ports_t audio_ports = {audio_ports_count, audio_ports_get};

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input) {(void) plugin; (void) is_input; return 1;}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info) {
    (void) plugin;
    if (index > 0) {return false;}
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "Triggers" : "Notes Out");
    return true;
}

static const clap_plugin_note_ports_t note_ports = {note_ports_count, note_ports_get};

static uint32_t params_count(const clap_plugin_t *plugin) {(void) plugin; return PARAM_COUNT;}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void) plugin;
    if (index >= PARAM_COUNT) {return false;}
    static const char *names[PARAM_COUNT] = {"Pattern", "Gate", "Transpose", "Rate"};
    std::memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    if (index == PARAM_PATTERN || index == PARAM_RATE) {info->flags |= CLAP_PARAM_IS_ENUM;}
    std::snprintf(info->name, sizeof(info->name), "%s", names[index]);
    info->min_value = min_of(index);
    info->max_value = max_of(index);
    info->default_value = default_of(index);
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value) {
    if (id >= PARAM_COUNT) {return false;}
    *value = value_of(hive_of(plugin), id);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size) {
    (void) plugin;
    if (id >= PARAM_COUNT || size == 0) {return false;}
    const int clamped = static_cast<int>(clamp_value(id, value));
    switch (id) {
        case PARAM_PATTERN: std::snprintf(text, size, "%d", clamped + 1); break;
        case PARAM_GATE: std::snprintf(text, size, "%d %%", clamped); break;
        case PARAM_RATE: std::snprintf(text, size, "%d/%d", rate_numerators[clamped], rate_denominators[clamped]); break;
        default: std::snprintf(text, size, "%+d st", clamped); break;
    }
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
    (void) plugin;
    if (id >= PARAM_COUNT || text == nullptr) {return false;}
    if (id == PARAM_RATE) { // "2/1", "1/4", or a plain multiplier
        const char *slash = std::strchr(text, '/');
        const double numerator = std::atof(text), denominator = slash != nullptr ? std::atof(slash + 1) : 1.0;
        if (numerator <= 0.0 || denominator <= 0.0) {return false;}
        const double ratio = numerator / denominator;
        int best = 0;
        for (int step = 1; step < RATE_STEPS; step++) {
            const double candidate = static_cast<double>(rate_numerators[step]) / rate_denominators[step];
            const double current = static_cast<double>(rate_numerators[best]) / rate_denominators[best];
            if (std::fabs(std::log(candidate / ratio)) < std::fabs(std::log(current / ratio))) {best = step;}
        }
        *value = best;
        return true;
    }
    const double parsed = std::atof(text);
    *value = clamp_value(id, id == PARAM_PATTERN ? parsed - 1.0 : parsed);
    return true;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    Hive *hive = hive_of(plugin);
    hive->out_events.drain(out);
    const uint32_t count = in->size(in);
    for (uint32_t index = 0; index < count; index++) {
        const clap_event_header_t *header = in->get(in, index);
        if (header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE) {continue;}
        const clap_event_param_value_t *param = reinterpret_cast<const clap_event_param_value_t *>(header);
        set_value(hive, param->param_id, param->value);
    }
}

static const clap_plugin_params_t params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

// "HIVE", version, the parameters, then the eight patterns: a header (8 bytes) and 169 tiles (16 bytes each) per
// pattern. The layout never moves, so a tile edit changes at most 16 bytes.
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    const Hive *hive = hive_of(plugin);
    static uint8_t bytes[STATE_BYTES];
    std::memset(bytes, 0, STATE_HEADER);
    std::memcpy(bytes, "HIVE", 4);
    bytes[4] = STATE_VERSION;
    bytes[5] = hive->pattern;
    bytes[6] = hive->gate;
    bytes[7] = static_cast<uint8_t>(hive->transpose);
    bytes[8] = hive->rate;
    std::memcpy(bytes + STATE_HEADER, hive->patterns, sizeof(hive->patterns));
    int64_t written = 0;
    while (written < static_cast<int64_t>(STATE_BYTES)) {
        const int64_t result = stream->write(stream, bytes + written, STATE_BYTES - written);
        if (result <= 0) {return false;}
        written += result;
    }
    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Hive *hive = hive_of(plugin);
    static uint8_t bytes[STATE_BYTES];
    int64_t read = 0;
    while (read < static_cast<int64_t>(STATE_BYTES)) {
        const int64_t result = stream->read(stream, bytes + read, STATE_BYTES - read);
        if (result <= 0) {break;}
        read += result;
    }
    if (read < static_cast<int64_t>(STATE_BYTES) || std::memcmp(bytes, "HIVE", 4) != 0 || bytes[4] != STATE_VERSION) {return false;}
    std::memcpy(hive->patterns, bytes + STATE_HEADER, sizeof(hive->patterns));
    for (Pattern &pattern : hive->patterns) {
        sanitize_header(pattern.header);
        for (Tile &tile : pattern.tiles) {sanitize_tile(tile);}
    }
    hive->pattern = static_cast<uint8_t>(clamp_value(PARAM_PATTERN, bytes[5]));
    hive->gate = static_cast<uint8_t>(clamp_value(PARAM_GATE, bytes[6]));
    hive->transpose = static_cast<int8_t>(clamp_value(PARAM_TRANSPOSE, static_cast<int8_t>(bytes[7])));
    hive->rate = static_cast<uint8_t>(clamp_value(PARAM_RATE, bytes[8]));
    if (!hive->playing) {hive->active = hive->pattern;}
    hive->spawners_dirty = true;
    hive->snapshot_due = true;
    hive->changed = (1u << PARAM_COUNT) - 1;
    return true;
}

static const clap_plugin_state_t state = {state_save, state_load};

// ------------------------------------------------------------------------------------------------ the page

static int32_t write_uri(const char *page, uint32_t length, char *uri, uint32_t capacity) {
    if (capacity > 0 && uri != nullptr) {std::snprintf(uri, capacity, "%s", page);}
    return static_cast<int32_t>(length);
}

static int32_t webview_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    (void) plugin;
    static const char page[] = "/index.html";
    return write_uri(page, sizeof(page), uri, capacity);
}

static bool webview_get_resource(const clap_plugin_t *plugin, const char *path, char *mime, uint32_t capacity,
                                 const clap_ostream_t *stream) {
    (void) plugin; (void) path; (void) mime; (void) capacity; (void) stream;
    return false;
}

// everything: the parameters, the pattern playing, every pattern (every page gets every message)
static void send_snapshot(Hive *hive) {
    static uint8_t message[8 + PARAM_COUNT * 8 + sizeof(Pattern) * PATTERN_COUNT];
    std::memset(message, 0, 8);
    message[0] = PLUGIN_SNAPSHOT;
    message[1] = hive->active;
    message[2] = hive->playing ? 1 : 0;
    for (clap_id id = 0; id < PARAM_COUNT; id++) {
        const double value = value_of(hive, id);
        std::memcpy(message + 8 + id * 8, &value, 8);
    }
    std::memcpy(message + 8 + PARAM_COUNT * 8, hive->patterns, sizeof(hive->patterns));
    hive->page.send(message, sizeof(message));
    hive->changed = 0;
    hive->edit_count = 0;
    hive->snapshot_due = false;
    hive->status_due = false;
    hive->edge_hit_count = 0;
    std::memset(hive->flashes, 0, sizeof(hive->flashes));
}

// what changed since the last answer: the parameters, tile and header edits, and while playing the tiles that played
// and where the cursors are. Nothing at all if nothing did.
static void send_delta(Hive *hive) {
    if (hive->snapshot_due) {
        send_snapshot(hive);
        return;
    }
    if (hive->changed == 0 && hive->edit_count == 0 && !hive->playing && !hive->status_due) {return;}
    static uint8_t message[8 + PARAM_COUNT * 8 + EDITS * 20 + sizeof(hive->flashes) + MAX_CURSORS * 4 + EDGE_HITS * 4];
    message[0] = PLUGIN_DELTA;
    message[1] = static_cast<uint8_t>(hive->changed);
    message[2] = hive->active;
    message[3] = hive->playing ? DELTA_PLAYING : 0;
    message[4] = static_cast<uint8_t>(hive->cursor_count);
    message[5] = static_cast<uint8_t>(hive->edge_hit_count);
    const uint16_t edits = static_cast<uint16_t>(hive->edit_count);
    std::memcpy(message + 6, &edits, 2);
    uint32_t size = 8;
    for (clap_id id = 0; id < PARAM_COUNT; id++) {
        if ((hive->changed & (1u << id)) == 0) {continue;}
        const double value = value_of(hive, id);
        std::memcpy(message + size, &value, 8);
        size += 8;
    }
    for (uint32_t index = 0; index < hive->edit_count; index++) {
        const Edit &edit = hive->edits[index];
        const Pattern &pattern = hive->patterns[edit.pattern];
        message[size] = edit.pattern;
        message[size + 1] = edit.type;
        std::memcpy(message + size + 2, &edit.index, 2);
        std::memset(message + size + 4, 0, 16);
        if (edit.type == EDIT_TILE) {
            std::memcpy(message + size + 4, &pattern.tiles[edit.index], 16);
        } else {
            std::memcpy(message + size + 4, &pattern.header, 8);
        }
        size += 20;
    }
    std::memcpy(message + size, hive->flashes, sizeof(hive->flashes));
    size += sizeof(hive->flashes);
    for (uint32_t index = 0; index < hive->cursor_count; index++) {
        const Cursor &cursor = hive->cursors[index];
        message[size++] = static_cast<uint8_t>(cursor.q);
        message[size++] = static_cast<uint8_t>(cursor.r);
        message[size++] = cursor.heading;
        const double left = cursor.hold || cursor.life == 0 ? 1.0 : 1.0 - static_cast<double>(cursor.age) / (cursor.life + 1);
        message[size++] = static_cast<uint8_t>(std::lround(std::fmax(0.0, left) * 255.0));
    }
    for (uint32_t index = 0; index < hive->edge_hit_count; index++) {
        const EdgeHit &hit = hive->edge_hits[index];
        message[size++] = static_cast<uint8_t>(hit.q);
        message[size++] = static_cast<uint8_t>(hit.r);
        message[size++] = hit.heading;
        message[size++] = hit.edge;
    }
    hive->page.send(message, size);
    hive->changed = 0;
    hive->edit_count = 0;
    hive->edge_hit_count = 0;
    hive->status_due = false;
    std::memset(hive->flashes, 0, sizeof(hive->flashes));
}

static void record_edit(Hive *hive, uint8_t pattern, uint8_t type, uint16_t index) {
    for (uint32_t at = 0; at < hive->edit_count; at++) {
        const Edit &edit = hive->edits[at];
        if (edit.pattern == pattern && edit.type == type && edit.index == index) {return;}
    }
    if (hive->edit_count >= EDITS) {
        hive->snapshot_due = true;
        return;
    }
    hive->edits[hive->edit_count++] = Edit{pattern, type, index};
}

// a page edit: live while a drag goes on (the host hears of it with the commit at the end), else at once
static void edited(Hive *hive, uint8_t flags, uint8_t pattern) {
    if (pattern == hive->active) {hive->spawners_dirty = true;}
    if ((flags & PAGE_LIVE) == 0 && hive->page.state != nullptr) {hive->page.state->mark_dirty(hive->page.host);}
}

// the page's shared messages (opendaw_page.h): parameter gestures and values, the hovered parameter, polls answered
// with the changes since the last (send_delta)
struct PageOf {
    Hive *hive;
    bool has(clap_id id) const {return id < PARAM_COUNT;}
    void set(clap_id id, double value) {set_value(hive, id, value);}
    double value(clap_id id) const {return value_of(hive, id);}
    void status() {send_delta(hive);}
};

static bool webview_receive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
    Hive *hive = hive_of(plugin);
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    if (size < 1 || hive->page.webview == nullptr) {return false;}
    switch (bytes[0]) {
        case opendaw::PAGE_POLL:
        case opendaw::PAGE_PARAM_BEGIN:
        case opendaw::PAGE_PARAM_SET:
        case opendaw::PAGE_PARAM_END:
        case opendaw::PAGE_HOVER: {
            PageOf page{hive};
            return opendaw::receive(page, hive->page, hive->out_events, buffer, size);
        }
        case PAGE_HELLO:
            send_snapshot(hive);
            return true;
        case PAGE_SHOW_WINDOW:
            return hive->host_gui != nullptr && hive->host_gui->request_show(hive->host);
        case PAGE_TILE: {
            uint16_t index;
            if (size < 24 || bytes[2] >= PATTERN_COUNT) {return false;}
            std::memcpy(&index, bytes + 4, 2);
            if (index >= TILE_COUNT) {return false;}
            Tile &tile = hive->patterns[bytes[2]].tiles[index];
            std::memcpy(&tile, bytes + 8, 16);
            sanitize_tile(tile);
            record_edit(hive, bytes[2], EDIT_TILE, index);
            edited(hive, bytes[1], bytes[2]);
            return true;
        }
        case PAGE_HEADER: {
            if (size < 16 || bytes[2] >= PATTERN_COUNT) {return false;}
            Header &header = hive->patterns[bytes[2]].header;
            std::memcpy(&header, bytes + 8, 8);
            sanitize_header(header);
            record_edit(hive, bytes[2], EDIT_HEADER, 0);
            edited(hive, bytes[1], bytes[2]);
            return true;
        }
        case PAGE_PATTERN: {
            if (size < 8 + sizeof(Pattern) || bytes[2] >= PATTERN_COUNT) {return false;}
            Pattern &pattern = hive->patterns[bytes[2]];
            std::memcpy(&pattern, bytes + 8, sizeof(Pattern));
            sanitize_header(pattern.header);
            for (Tile &tile : pattern.tiles) {sanitize_tile(tile);}
            hive->snapshot_due = true;
            edited(hive, bytes[1], bytes[2]);
            return true;
        }
        case PAGE_COMMIT:
            if (hive->page.state != nullptr) {hive->page.state->mark_dirty(hive->page.host);}
            return true;
        default:
            return false;
    }
}

static const clap_plugin_webview_t webview = {webview_get_uri, webview_get_resource, webview_receive};

static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    (void) plugin;
    *width = GUI_WIDTH;
    *height = GUI_HEIGHT;
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin) {(void) plugin; return false;}

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {(void) plugin; (void) hints; return false;}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    (void) plugin;
    *width = GUI_WIDTH;
    *height = GUI_HEIGHT;
    return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
    (void) plugin;
    return width == GUI_WIDTH && height == GUI_HEIGHT;
}

static const clap_plugin_gui_t gui = {
    opendaw::gui_is_api_supported, opendaw::gui_get_preferred_api, opendaw::gui_create, opendaw::gui_destroy,
    opendaw::gui_set_scale, gui_get_size, gui_can_resize, gui_get_resize_hints, gui_adjust_size, gui_set_size,
    opendaw::gui_set_parent, opendaw::gui_set_transient, opendaw::gui_suggest_title, opendaw::gui_show, opendaw::gui_hide
};

static int32_t panel_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    (void) plugin;
    static const char page[] = "/index.html?panel";
    return write_uri(page, sizeof(page), uri, capacity);
}

static bool panel_get_width(const clap_plugin_t *plugin, uint32_t height, uint32_t *width) {
    (void) plugin;
    *width = opendaw::panel_width_at(PANEL_WIDTH, height);
    return true;
}

static const opendaw_plugin_device_panel_t device_panel = {panel_get_uri, panel_get_width};

static const void *hive_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void) plugin;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {return &audio_ports;}
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) {return &note_ports;}
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {return &params;}
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {return &state;}
    if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0) {return &webview;}
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {return &gui;}
    if (std::strcmp(id, OPENDAW_EXT_DEVICE_PANEL) == 0) {return &device_panel;}
    return nullptr;
}

static void hive_on_main_thread(const clap_plugin_t *plugin) {(void) plugin;}

static const char *features[] = {
    CLAP_PLUGIN_FEATURE_NOTE_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL, OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL_WIDTH(PANEL_WIDTH), nullptr
};

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "studio.opendaw.hive",
    "Hive",
    "open-webclap",
    "https://github.com/andremichelle/open-webclap/tree/main/ports/hive",
    "https://github.com/andremichelle/open-webclap/tree/main/ports/hive",
    "",
    "0.1.0",
    "Note sequencer on a hexagonal grid: cursors walk the tiles' arrows and play their notes",
    features
};

static const clap_plugin_descriptor_t *descriptor_ptr() {return &descriptor;}

static const clap_plugin_t *create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void) factory;
    if (std::strcmp(plugin_id, descriptor.id) != 0) {return nullptr;}
    build_cells();
    void *memory = std::calloc(1, sizeof(Hive));
    if (memory == nullptr) {return nullptr;}
    Hive *hive = new(memory) Hive();
    hive->host = host;
    default_patterns(hive);
    hive->gate = 80;
    hive->rate = RATE_DEFAULT;
    hive->spawners_dirty = true;
    hive->sample_rate = 48000.0;
    hive->plugin.desc = &descriptor;
    hive->plugin.plugin_data = hive;
    hive->plugin.init = hive_init;
    hive->plugin.destroy = hive_destroy;
    hive->plugin.activate = hive_activate;
    hive->plugin.deactivate = hive_deactivate;
    hive->plugin.start_processing = hive_start_processing;
    hive->plugin.stop_processing = hive_stop_processing;
    hive->plugin.reset = hive_reset;
    hive->plugin.process = hive_process;
    hive->plugin.get_extension = hive_get_extension;
    hive->plugin.on_main_thread = hive_on_main_thread;
    return &hive->plugin;
}

static uint32_t plugin_count(const clap_plugin_factory_t *factory) {(void) factory; return 1;}

static const clap_plugin_descriptor_t *plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index) {
    (void) factory;
    return index == 0 ? descriptor_ptr() : nullptr;
}

static const clap_plugin_factory_t factory = {plugin_count, plugin_descriptor, create};

static bool entry_init(const char *path) {(void) path; build_cells(); return true;}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
