/* Own CW decoder, see cwdec.h. Runs in the main loop (500 frames/s, a few dozen float ops each). */
#include <string.h>
#include "cwdec.h"
#include "fastmath.h"

#define DOT_DEFAULT  30.0f      /* frames: 60 ms = 20 WPM */
#define DOT_MIN      10.0f      /* 60 WPM */
#define DOT_MAX      120.0f     /* 5 WPM */
#define HIST_N       16         /* mark lengths for the dot / dash split */
#define GAP_N        12         /* character / word gaps for the word threshold */
#define EL_MAX       8          /* elements per character (8 dots = error, <HH>) */
#define IDLE_FRAMES  (10000 / CWDEC_FRAME_MS)
#define BOX_N        8          /* power moving average, up to 16 ms */
#define WARM_FRAMES  (50 / CWDEC_FRAME_MS)
#define CARRIER      (1500 / CWDEC_FRAME_MS)    /* longer marks = a carrier, not an element */

/* ITU Morse; the prosigns that collide with punctuation are shown as prosigns (<AR> = "+",
 * <KN> = "(", <AS> = "&"), <BT> as "=". */
static const struct { const char *pat, *txt; } morse[] = {
    {".-", "A"}, {"-...", "B"}, {"-.-.", "C"}, {"-..", "D"}, {".", "E"}, {"..-.", "F"},
    {"--.", "G"}, {"....", "H"}, {"..", "I"}, {".---", "J"}, {"-.-", "K"}, {".-..", "L"},
    {"--", "M"}, {"-.", "N"}, {"---", "O"}, {".--.", "P"}, {"--.-", "Q"}, {".-.", "R"},
    {"...", "S"}, {"-", "T"}, {"..-", "U"}, {"...-", "V"}, {".--", "W"}, {"-..-", "X"},
    {"-.--", "Y"}, {"--..", "Z"},
    {"-----", "0"}, {".----", "1"}, {"..---", "2"}, {"...--", "3"}, {"....-", "4"},
    {".....", "5"}, {"-....", "6"}, {"--...", "7"}, {"---..", "8"}, {"----.", "9"},
    {".-.-.-", "."}, {"--..--", ","}, {"..--..", "?"}, {".----.", "'"}, {"-.-.--", "!"},
    {"-..-.", "/"}, {"-.--.-", ")"}, {"---...", ":"}, {"-.-.-.", ";"}, {"-...-", "="},
    {"-....-", "-"}, {"..--.-", "_"}, {".-..-.", "\""}, {"...-..-", "$"}, {".--.-.", "@"},
    {".-.-.", "<AR>"}, {"-.--.", "<KN>"}, {"...-.-", "<SK>"}, {".-...", "<AS>"},
    {"........", "<HH>"},
};

/* detector */
static float box[BOX_N], noise_db, peak_db;
static unsigned box_pos;
static int warm;                    /* frames since reset (detector warm-up) */
static bool raw, mark;
static int run, pend;               /* frames in the current key state; frames of a pending change */
static int level;                   /* 0 = AUTO */
/* timing */
static uint16_t hist[HIST_N], gaps[GAP_N];
static int hist_n, hist_pos, gap_n, gap_pos;
static float th = 2 * DOT_DEFAULT, dot_f = DOT_DEFAULT, esp_f = DOT_DEFAULT, wsplit;
static uint16_t el[EL_MAX];
static int nel;
static bool overflow, word_done = true, seen_mark;
static uint32_t idle = IDLE_FRAMES;
/* text */
static char text[CWDEC_TEXT_N];
static unsigned text_n, text_head;
static uint32_t seq;

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

void cwdec_reset(void)
{
    memset(box, 0, sizeof box);
    warm = 0;
    raw = mark = false;
    run = pend = 0;
    hist_n = hist_pos = gap_n = gap_pos = 0;
    dot_f = esp_f = DOT_DEFAULT;
    th = 2 * DOT_DEFAULT;
    wsplit = 0;
    nel = 0;
    overflow = false;
    word_done = true;
    seen_mark = false;
    idle = IDLE_FRAMES;
}

void cwdec_clear(void)
{
    text_n = text_head = 0;
    seq++;
}

void cwdec_set_level(int db)
{
    level = db < 0 ? 0 : db > CWDEC_LEVEL_MAX ? CWDEC_LEVEL_MAX : db;
}

static void put(char c)
{
    text[text_head] = c;
    text_head = (text_head + 1) % CWDEC_TEXT_N;
    if (text_n < CWDEC_TEXT_N)
        text_n++;
    seq++;
}

static char last_char(void)
{
    return text_n ? text[(text_head + CWDEC_TEXT_N - 1) % CWDEC_TEXT_N] : ' ';
}

/* Gap thresholds. With dot = u + b and element gap = u - b (b = detector bias): element / char
 * gap boundary 2u - b, char / word gap boundary 5u - b. */
static float char_thr(void)
{
    return 0.5f * dot_f + 1.5f * esp_f;
}

static float word_thr(void)
{
    float w = 2.0f * dot_f + 3.0f * esp_f;
    return wsplit > w ? wsplit : w;
}

/* Two-means split of v[0..n): low / high cluster means; false if all values are alike. */
static bool split2(const uint16_t *v, int n, float ratio, float *lo, float *hi)
{
    if (n < 2)
        return false;
    int mn = v[0], mx = v[0];
    for (int i = 1; i < n; i++) {
        if (v[i] < mn) mn = v[i];
        if (v[i] > mx) mx = v[i];
    }
    if (mx < ratio * mn)
        return false;
    float c = 0.5f * (mn + mx);
    for (int it = 0; it < 3; it++) {
        float sl = 0, sh = 0;
        int nl = 0, nh = 0;
        for (int i = 0; i < n; i++) {
            if (v[i] < c) { sl += v[i]; nl++; } else { sh += v[i]; nh++; }
        }
        *lo = sl / nl;          /* both non-empty: mn < c <= mx */
        *hi = sh / nh;
        c = 0.5f * (*lo + *hi);
    }
    return true;
}

static void retime(void)
{
    float lo, hi, old = dot_f;
    if (split2(hist, hist_n, 1.8f, &lo, &hi)) {
        th = 0.5f * (lo + hi);
        dot_f = lo;
    } else {                    /* only dots or only dashes lately: keep the split */
        float s = 0;
        for (int i = 0; i < hist_n; i++)
            s += hist[i];
        s /= hist_n;
        dot_f = s < th ? s : s / 3.0f;
    }
    dot_f = clampf(dot_f, DOT_MIN, DOT_MAX);
    th = clampf(th, 1.5f * dot_f, 4.0f * dot_f);
    esp_f = clampf(esp_f * dot_f / old, 0.3f * dot_f, 2.0f * dot_f);     /* speed change */
}

static void emit_char(void)
{
    if (!nel)
        return;
    const char *t = "*";
    if (!overflow) {
        char pat[EL_MAX + 1];
        for (int i = 0; i < nel; i++)
            pat[i] = el[i] >= th ? '-' : '.';
        pat[nel] = 0;
        for (unsigned k = 0; k < sizeof morse / sizeof morse[0]; k++)
            if (!strcmp(morse[k].pat, pat)) {
                t = morse[k].txt;
                break;
            }
    }
    while (*t)
        put(*t++);
    nel = 0;
    overflow = false;
}

static void mark_end(int len)
{
    if (len > CARRIER) {        /* carrier (tuning, test): no element, drop the character */
        nel = 0;
        overflow = false;
        return;
    }
    hist[hist_pos] = (uint16_t)len;
    hist_pos = (hist_pos + 1) % HIST_N;
    if (hist_n < HIST_N)
        hist_n++;
    retime();
    seen_mark = true;
    if (nel < EL_MAX)
        el[nel++] = (uint16_t)len;
    else
        overflow = true;
}

static void space_end(int len)
{
    word_done = false;
    if (len < char_thr()) {
        esp_f += (len - esp_f) * 0.25f;
        esp_f = clampf(esp_f, 0.3f * dot_f, 2.0f * dot_f);
    } else if (seen_mark && len < 60.0f * 0.5f * (dot_f + esp_f)) {
        /* Word threshold 5/3 of the character gap (3 : 5 : 7), taken as the median of the recent
         * character / word gaps: character gaps are the majority in any text, pauses between
         * overs are outliers; follows Farnsworth spacing too */
        gaps[gap_pos] = (uint16_t)len;
        gap_pos = (gap_pos + 1) % GAP_N;
        if (gap_n < GAP_N)
            gap_n++;
        if (gap_n >= 5) {
            uint16_t v[GAP_N];
            memcpy(v, gaps, sizeof v);
            for (int i = 1; i < gap_n; i++)
                for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
                    uint16_t t = v[j];
                    v[j] = v[j - 1];
                    v[j - 1] = t;
                }
            wsplit = v[gap_n / 2] * (5.0f / 3.0f);
        }
    }
}

/* Debounced key state machine, one call per frame. */
static void key(bool on)
{
    if (run < 1 << 24)          /* long silence: no overflow */
        run++;
    int glitch = (int)(0.15f * dot_f);
    glitch = glitch < 1 ? 1 : glitch > 6 ? 6 : glitch;
    if (on == mark) {
        pend = 0;
    } else if (++pend >= glitch) {
        int len = run - pend;
        if (mark)
            mark_end(len);
        else
            space_end(len);
        mark = on;
        run = pend;
        pend = 0;
    }
    if (mark) {
        idle = 0;
    } else {
        if (idle < IDLE_FRAMES)
            idle++;
        if (nel && run >= char_thr())
            emit_char();
        if (!word_done && run >= word_thr()) {
            word_done = true;
            if (last_char() != ' ')
                put(' ');
        }
    }
}

void cwdec_feed(float p)
{
    /* moving average over ~0.2 dot: unlike a one-pole low-pass its decay does not depend on
     * the signal-to-noise ratio (a one-pole stretched strong marks by tens of ms) */
    if (!warm)
        for (int i = 0; i < BOX_N; i++)
            box[i] = p;
    box[box_pos] = p;
    box_pos = (box_pos + 1) % BOX_N;
    int n = (int)(0.2f * dot_f);
    n = n < 1 ? 1 : n > BOX_N ? BOX_N : n;
    float s = 0;
    for (int i = 1; i <= n; i++)
        s += box[(box_pos + BOX_N - i) % BOX_N];
    float db = 3.0103f * fast_log2(s / n + 1e-30f);
    if (!warm)
        noise_db = peak_db = db;
    /* noise: mean level in the spaces (falls fast); while marking it only creeps up (2 s), so
     * a detector stuck on noise frees itself. Fast tracking during the first 50 ms. */
    if (warm < WARM_FRAMES) {
        warm++;
        noise_db += (db - noise_db) * 0.2f;
    } else if (!raw) {
        noise_db += (db - noise_db) * (db < noise_db ? 0.05f : 0.02f);
    } else {
        noise_db += (db - noise_db) * (db < noise_db ? 0.05f : 0.001f);
    }
    /* signal peak: fast attack, follows fading during marks, decays 5 dB/s in spaces */
    if (db > peak_db)
        peak_db += (db - peak_db) * 0.15f;
    else if (raw)
        peak_db += (db - peak_db) * 0.04f;
    else
        peak_db -= 0.005f * CWDEC_FRAME_MS;
    if (peak_db < noise_db)
        peak_db = noise_db;

    float thr, hyst;
    bool gate = true;
    if (level) {
        thr = noise_db + level;
        hyst = 1.0f;
    } else {
        /* 6 dB under the peak (room for fading; the moving average lengthens the marks by only
         * ~1/2 of its length), at 0.7 x SNR for weak signals; hysteresis on the falling edge */
        float snr = peak_db - noise_db;
        gate = snr >= CWDEC_AUTO_SNR;
        float up = snr - 6.0f > 0.7f * snr ? snr - 6.0f : 0.7f * snr;
        thr = noise_db + up;
        hyst = 0.2f * up;
        if (hyst > 3.0f)
            hyst = 3.0f;
    }
    raw = gate && warm >= WARM_FRAMES && (raw ? db > thr - hyst : db > thr);
    key(raw);
}

int cwdec_text(char *out, int n)
{
    if (n > (int)text_n)
        n = (int)text_n;
    if (n < 0)
        n = 0;
    unsigned start = (text_head + CWDEC_TEXT_N - n) % CWDEC_TEXT_N;
    for (int i = 0; i < n; i++)
        out[i] = text[(start + i) % CWDEC_TEXT_N];
    out[n] = 0;
    return n;
}

uint32_t cwdec_seq(void)
{
    return seq;
}

int cwdec_wpm(void)
{
    if (idle >= IDLE_FRAMES)
        return 0;
    float u = 0.5f * (dot_f + esp_f) * CWDEC_FRAME_MS;      /* ms */
    return (int)(1200.0f / u + 0.5f);
}

bool cwdec_key(void)
{
    return mark;
}
