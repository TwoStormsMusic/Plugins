// Shared memory between every TwoStorms CLAP plug-in on the machine (the JSFX suite's gmem
// "TwoStormsGS", redone as a typed struct). It is a named OS shared-memory block, so instances see
// each other even when the host runs plug-ins in separate processes.
//
// Who writes what is the same as in the JSFX suite: a slot belongs to one instance (its owner
// writes its status, meters and published parameters); windows and the Gain Stage talk to an
// instance through its command ring; the owner's main thread writes the names that the JSFX
// suite's helper script used to write (a CLAP plug-in gets them from REAPER directly).
//
// Every field is an 8-byte atomic, read and written relaxed (or acquire/release where noted), so
// the audio threads never lock. Strings use a sequence counter.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ts {
namespace shm {

constexpr int NSLOT = 1024;   // instances that can be live at once
constexpr int NPAR = 256;     // published parameters per slot (indexed by parameter ID)
constexpr int RINGN = 16;     // commands queued per slot
constexpr int NCH = 8;        // channels an Act can have
constexpr int AUD_LANES = 64; // audio-key lanes
constexpr int AUD_R = 32768;  // samples per lane ring (per channel)
constexpr int AUD_MASK = AUD_R - 1;
constexpr int NAMELEN = 128;
constexpr int LAYOUT = 4;     // bump when the layout changes (a new name keeps old and new builds apart)
                              // 2: the Gain Stages' shared table; 3: the sample rate and the Tuner's mute;
                              // 4: the main thread's heartbeat

using AD = std::atomic<double>;
using AI = std::atomic<int64_t>;

// slot kinds and the plug-in an Act is built into (subkind), as in the JSFX suite
enum Kind { KIND_POINT = 1, KIND_MASTER = 2 };
enum SubKind {
    SK_ACT = 0, SK_COMPRESSOR = 1, SK_EQ = 2, SK_MULTIBAND = 3, SK_IRLOADER = 4, SK_SATURATOR = 5,
    SK_DELAY = 6, SK_CHORUS = 7, SK_TREMOLO = 8, SK_DEFITCHER = 9, SK_GATE = 10, SK_WAH = 11,
    SK_TUNER = 12,   // (no slot of its own: it only reads others)
};

// commands (ring entries: c, x, y, z)
enum Cmd {
    C_SET = 1,       // x = parameter, y = value
    C_ADDGAIN = 2,   // drive link: x = group, y = delta dB, z = the source's group count (0 = all groups)
    C_AUTO = 3,      // x = mode (0 Target, 1 Match, 2 Reference), y = run ID (0 = this one alone)
    C_STOP = 4,
    C_CANCEL = 5,
    C_RESETHOLD = 6,
    C_TGSTART = 7,   // Set threshold(s): x = 1 when started from Auto all (waits for the queued Auto)
    C_TGSTOP = 8,
};

// auto states and results
enum { AS_IDLE = 0, AS_QUEUED = 1, AS_WAIT = 2, AS_MEAS = 3 };
enum {
    R_AUTO = 1, R_MATCH = 2, R_NOSIG = 3, R_CANCEL = 4, R_NOPREV = 5, R_PREVGONE = 6,
    R_REF = 7, R_NOREF = 8, R_REFAFTER = 9, R_REFGONE = 10,
};

// name-record flags (nflags)
enum {
    NF_INPROJECT = 1, NF_ENABLED = 2, NF_OFFLINE = 4,
    NF_REFFED = 8,   // this track feeds the reference's track (sends / folders)
    NF_TAIL = 16,    // an enabled plug-in comes after this one in its chain
};
// placement flags (pflags): which chain of the track
enum { PL_TAKE = 1, PL_INPUT = 2, PL_CHAIN_MASK = 5, PL_CONTAINER = 8 };   // PL_CONTAINER: inside an FX container
// drive-link status for a plug-in parameter (dstat)
enum { DS_NONE = 0, DS_LINKED = 1, DS_LEARNING = 2, DS_GONE = 3, DS_NOTDB = 4 };

struct CmdEntry { AD c, x, y, z; };

struct Slot {
    // ---- identity and heartbeat ----
    AI owner;      // instance ID (100000 and up), 0 = free
    AI nonce;      // the live object's random number (tells a copied plug-in from a restarted one)
    AD heart;      // when the owner last published (now_sec)
    AI hbc;        // publish counter
    AI kind, subkind;
    AI on;         // 1 unless a Gain Stage is switched off
    // ---- placement (from the host) ----
    AI track;      // chain key: a hash of the track's (or take's) GUID; 0 = unknown
    AI trackno;    // 1-based track number, -1 master, 0 unknown
    AI pflags;     // PL_*
    AD order;      // chain order (input FX come first)
    // ---- Auto ----
    AI astate, amode, runid;
    AD donet;      // when the last run ended
    AI result, resseq, limit;
    AD atime, adur;
    AI blkseq, blklen;   // published blocks and the last one's length
    AI kact;             // 1 while this instance K-weights its output for the next one's LUFS match
    AD kreq;             // written by the next point while it matches against this one (time)
    AI dlearn;           // bumped by a window to learn a drive-link parameter
    // ---- audio key (the Gate keying from this plug-in's output) ----
    AI areq;       // written by the reader: counts up once per block it runs
    AI alane;      // lane + 1 (0 = not writing, -1 = no lane free)
    AI aseq, astart, alen;
    // ---- meters (per channel) ----
    AD pkd[NCH], pkh[NCH], rms[NCH], rmh[NCH], clip[NCH];
    AD bpk[NCH], ben[NCH], bke[NCH];   // the last block's raw numbers, for Match
    AD rlvl[NCH];  // the level the last Auto measured
    AD drvs[NCH];  // how much of each channel's gain came from drive links
    // ---- the Gain Stage's Set row for this plug-in ----
    AD tgs, tgel, tgav;   // state (0 idle, 1 waiting, 2 listening, 3 working), seconds listened, 1 = has something to set
    // ---- command ring (many writers: windows, the Gain Stage; one reader: the owner's audio thread) ----
    AI ringW, ringR;
    std::atomic<int32_t> ringLock;
    int32_t pad0;
    CmdEntry ring[RINGN];
    // ---- published parameters: p[id] ----
    AD p[NPAR];
    // ---- names (written by the owner's main thread; seqlock) ----
    AI nseq;       // odd while writing
    AI nid;        // the owner these names belong to (0 = none yet)
    AI norder;     // chain order as REAPER numbers it
    AI nflags;     // NF_*
    AI dstat;      // DS_*
    char tname[NAMELEN];   // track name
    char prev[NAMELEN];    // the plug-in before this one in its chain
    char dname[NAMELEN];   // the drive-linked plug-in parameter
    // ---- the Tuner ----
    AD srate;      // the owner's sample rate (the Tuner reads its audio key)
    AD mheart;     // the owner's main thread, while the slot is still its own (a bypassed plug-in's audio stops,
                   // its main thread doesn't: nobody takes the slot of a plug-in that is only bypassed)
    AD tmute;      // written by a Tuner listening to this plug-in while its Mute is on (time): the output goes
                   // silent (after the audio key, so the Tuner still hears it); back on a quarter second after
                   // the Tuner stops asking
};

struct AudioLane {
    AI slot1;      // owner slot + 1 (0 = free)
    AI ownerid;
    AD lastw;      // last write time
    AI pad;
    std::atomic<float> l[AUD_R], r[AUD_R];
};

struct Header {
    AI magic;
    AI layout;
    AI size;
    AI slotver;    // bumped when a slot changes hands
    AI maxslot;    // highest slot ever used + 1
    AI zoom;       // window zoom shared by every TwoStorms window
    AI zoomv;      // and its change counter
    AI presetv;    // bumped when a user preset is saved or deleted (windows re-read the list)
    AI reserved[56];
};

// The Gain Stages' shared table (the JSFX's gmem SHB): Stages/Finales hide and reference settings, Auto all
// ticks and Set rows, kept the same in every Gain Stage. Rows of 8: id, hidden on Stages, hidden on Finales,
// reference on Stages, reference on Finales, time of the last change (ms), left out of Auto all, Set row shown.
constexpr int GS_ROWS = 1024, GS_W = 8;
struct GsTable {
    AI n;          // rows in use
    AI ver;        // bumped on every change
    std::atomic<int32_t> lock;
    int32_t pad;
    AD row[GS_ROWS * GS_W];
};

struct Shared {
    Header h;
    Slot slot[NSLOT];
    AudioLane lane[AUD_LANES];
    GsTable gs;
};

constexpr int64_t MAGIC = 0x3153524F54534F57LL;   // "WOSTORS1"

// The block, attached on first use and kept for the life of the process. Never null: if the OS
// refuses a named block, a private one is used (instances then only see their own process).
Shared *get();
bool isShared();   // false if it fell back to private memory

inline Slot &slot(int s) { return get()->slot[s]; }

// command ring
bool send(int s, int cmd, double x, double y, double z);
// names: write (owner) and read (anyone) without tearing
void writeNames(int s, int64_t id, int64_t order, int64_t flags, int64_t dstat, const char *tname,
                const char *prev, const char *dname);
bool readNames(int s, int64_t *id, char *tname, char *prev, char *dname);

} // namespace shm
} // namespace ts
