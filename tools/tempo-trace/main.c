/* tempo-trace -- convert a binary trace into something readable.
 *
 * Formatting belongs here, not on the execution path (S22). The runtime
 * writes fixed-size binary records; this tool turns them into text, JSONL,
 * CSV, or a Chrome trace.
 *
 *     tempo-trace dump run.trace
 *     tempo-trace dump --format=jsonl run.trace
 */
#include "tempo/event.h"
#include "tempo/time.h"
#include "tempo/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { FMT_TEXT, FMT_JSONL, FMT_CSV, FMT_CHROME } format_t;

static void usage(FILE *out)
{
    fprintf(out,
            "usage: tempo-trace dump [--format=text|jsonl|csv|chrome] [--limit=N] FILE\n"
            "       tempo-trace info FILE\n");
}

static const char *flags_str(uint16_t flags, char *buf, size_t len)
{
    /* OBSERVE and SYNCHRONIZE is the distinction that matters most when
     * reading a trace (S11). */
    snprintf(buf, len, "%s%s%s",
             (flags & TEMPO_EVF_SYNCHRONIZE) ? "sync" : "",
             (flags & TEMPO_EVF_SYNCHRONIZE) && (flags & TEMPO_EVF_OBSERVE) ? "," : "",
             (flags & TEMPO_EVF_OBSERVE) ? "obs" : "");
    return buf;
}

static int read_header(FILE *f, tempo_trace_file_header_t *hdr, const char *path)
{
    if (fread(hdr, sizeof(*hdr), 1, f) != 1) {
        fprintf(stderr, "tempo-trace: %s: cannot read header\n", path);
        return 1;
    }
    if (memcmp(hdr->magic, TEMPO_TRACE_MAGIC, 8) != 0) {
        fprintf(stderr, "tempo-trace: %s: not a tempo trace\n", path);
        return 1;
    }
    if (hdr->version != TEMPO_TRACE_VERSION) {
        fprintf(stderr, "tempo-trace: %s: trace version %u, this tool understands %u\n",
                path, hdr->version, TEMPO_TRACE_VERSION);
        return 1;
    }
    if (hdr->record_size != sizeof(tempo_event_t)) {
        fprintf(stderr, "tempo-trace: %s: record size %u, expected %zu\n",
                path, hdr->record_size, sizeof(tempo_event_t));
        return 1;
    }
    return 0;
}

static int cmd_info(const char *path)
{
    tempo_trace_file_header_t hdr;
    FILE *f = fopen(path, "rb");

    if (!f) {
        fprintf(stderr, "tempo-trace: cannot open %s\n", path);
        return 1;
    }
    if (read_header(f, &hdr, path)) {
        fclose(f);
        return 1;
    }
    printf("file:          %s\n", path);
    printf("version:       %u\n", hdr.version);
    printf("record size:   %u bytes\n", hdr.record_size);
    printf("records:       %llu\n", (unsigned long long)hdr.record_count);
    printf("dropped:       %llu\n", (unsigned long long)hdr.dropped_count);
    printf("participants:  %u\n", hdr.n_participants);
    fclose(f);
    return 0;
}

static int cmd_dump(const char *path, format_t fmt, uint64_t limit)
{
    tempo_trace_file_header_t hdr;
    tempo_event_t e;
    FILE *f = fopen(path, "rb");
    uint64_t n = 0;
    char tbuf[32], fbuf[16];

    if (!f) {
        fprintf(stderr, "tempo-trace: cannot open %s\n", path);
        return 1;
    }
    if (read_header(f, &hdr, path)) {
        fclose(f);
        return 1;
    }

    if (fmt == FMT_CSV)
        printf("sequence,logical_time_ns,microstep,participant,type,type_name,flags,arg0,arg1,cause\n");
    else if (fmt == FMT_CHROME)
        printf("[\n");

    while (fread(&e, sizeof(e), 1, f) == 1) {
        if (limit && n >= limit)
            break;
        switch (fmt) {
        case FMT_TEXT:
            printf("%8llu  %14s  ms=%-3u  p%-3u  %-18s [%s] arg0=%u arg1=0x%llx\n",
                   (unsigned long long)e.hdr.sequence,
                   tempo_time_format(e.hdr.logical_time, tbuf, sizeof(tbuf)),
                   e.microstep, e.hdr.participant_id,
                   tempo_event_type_str(e.hdr.type),
                   flags_str(e.hdr.flags, fbuf, sizeof(fbuf)),
                   e.arg0, (unsigned long long)e.arg1);
            break;
        case FMT_JSONL:
            printf("{\"seq\":%llu,\"t\":%llu,\"microstep\":%u,\"participant\":%u,"
                   "\"type\":%u,\"type_name\":\"%s\",\"flags\":%u,"
                   "\"arg0\":%u,\"arg1\":%llu,\"cause\":%llu}\n",
                   (unsigned long long)e.hdr.sequence,
                   (unsigned long long)e.hdr.logical_time,
                   e.microstep, e.hdr.participant_id, e.hdr.type,
                   tempo_event_type_str(e.hdr.type), e.hdr.flags,
                   e.arg0, (unsigned long long)e.arg1,
                   (unsigned long long)e.cause);
            break;
        case FMT_CSV:
            printf("%llu,%llu,%u,%u,%u,%s,%u,%u,%llu,%llu\n",
                   (unsigned long long)e.hdr.sequence,
                   (unsigned long long)e.hdr.logical_time,
                   e.microstep, e.hdr.participant_id, e.hdr.type,
                   tempo_event_type_str(e.hdr.type), e.hdr.flags,
                   e.arg0, (unsigned long long)e.arg1,
                   (unsigned long long)e.cause);
            break;
        case FMT_CHROME:
            /* Chrome's tracing viewer wants microseconds, and logical time is
             * the only clock worth plotting here. */
            printf("%s{\"name\":\"%s\",\"ph\":\"i\",\"s\":\"g\",\"pid\":1,\"tid\":%u,"
                   "\"ts\":%.3f,\"args\":{\"seq\":%llu,\"microstep\":%u,"
                   "\"arg0\":%u,\"arg1\":%llu}}",
                   n ? ",\n" : "",
                   tempo_event_type_str(e.hdr.type), e.hdr.participant_id,
                   (double)e.hdr.logical_time / 1000.0,
                   (unsigned long long)e.hdr.sequence, e.microstep,
                   e.arg0, (unsigned long long)e.arg1);
            break;
        }
        n++;
    }
    if (fmt == FMT_CHROME)
        printf("\n]\n");
    if (fmt == FMT_TEXT)
        fprintf(stderr, "%llu record(s)%s\n", (unsigned long long)n,
                hdr.dropped_count ? " (trace dropped records)" : "");
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    format_t fmt = FMT_TEXT;
    uint64_t limit = 0;
    const char *path = NULL;
    const char *cmd;
    int i;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    cmd = argv[1];
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        usage(stdout);
        return 0;
    }

    for (i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--format=", 9)) {
            const char *v = argv[i] + 9;
            if (!strcmp(v, "text"))        fmt = FMT_TEXT;
            else if (!strcmp(v, "jsonl"))  fmt = FMT_JSONL;
            else if (!strcmp(v, "csv"))    fmt = FMT_CSV;
            else if (!strcmp(v, "chrome")) fmt = FMT_CHROME;
            else {
                fprintf(stderr, "tempo-trace: unknown format '%s'\n", v);
                return 2;
            }
        } else if (!strncmp(argv[i], "--limit=", 8)) {
            limit = strtoull(argv[i] + 8, NULL, 10);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "tempo-trace: unknown option '%s'\n", argv[i]);
            return 2;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        usage(stderr);
        return 2;
    }

    if (!strcmp(cmd, "dump"))
        return cmd_dump(path, fmt, limit);
    if (!strcmp(cmd, "info"))
        return cmd_info(path);
    fprintf(stderr, "tempo-trace: unknown command '%s'\n", cmd);
    usage(stderr);
    return 2;
}
