#include "machine_prep.h"

#include "fat32.h"
#include "udf.h"

#include <cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <hivex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wimlib.h>
#include <zlib.h>

/* Builds a bootable Windows disk image from an install ISO, offline, without Windows Setup:
 *   GPT [ESP 260 MiB FAT32 | NTFS rest]
 *   NTFS  <- install.wim/esd image applied by wimlib, plus HyperWin drivers registered in the
 *            offline SYSTEM hive and an answer file that skips OOBE
 *   ESP   <- our EFI loader as \EFI\BOOT\BOOTAA64.EFI, Windows boot files, BCD from a template
 *            with its device GUIDs pointed at this disk.
 * Assets directory: drivers.json, drivers/<file>.sys, efi/<loader>.EFI, BCD, unattend.xml,
 * guest/ (copied to the HyperWin folder on C:; firstboot.cmd runs from the answer file). */

int hw_mkntfs_main(int argc, char **argv);

#define SECTOR 512ull
#define ESP_FIRST (HW_PREP_ESP_OFFSET / 512)
#define ESP_SECTORS (260ull * 2048)

static const uint8_t ESP_TYPE[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static const uint8_t DATA_TYPE[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
/* Device GUIDs (on-disk byte order) inside the reference BCD: disk, ESP, Windows partition. */
static const uint8_t BCD_DISK[16] = {0x4b, 0x10, 0xbb, 0xd4, 0x77, 0x72, 0xe8, 0x11, 0x81, 0xf0, 0x28, 0x16, 0xad, 0x97, 0xcc, 0xb3};
static const uint8_t BCD_ESP[16] = {0x4c, 0x10, 0xbb, 0xd4, 0x77, 0x72, 0xe8, 0x11, 0x81, 0xf0, 0x28, 0x16, 0xad, 0x97, 0xcc, 0xb3};
static const uint8_t BCD_OS[16] = {0x61, 0x85, 0x5b, 0x97, 0x2e, 0x5f, 0x22, 0x4b, 0xa0, 0xd9, 0x65, 0x4b, 0x09, 0x80, 0x07, 0xe0};

typedef struct {
    const HwPrepRequest *req;
    char *err;
    size_t errlen;
    char work[1024];
    uint8_t disk_guid[16], esp_guid[16], os_guid[16];
    uint64_t ntfs_first, ntfs_sectors;
} Prep;

static int fail(Prep *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, p->errlen, fmt, ap);
    va_end(ap);
    return -1;
}

static void progress(Prep *p, int pct, const char *msg)
{
    if (p->req->progress) p->req->progress(p->req->ctx, pct, msg);
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)n + 1);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
        if (buf) { buf[n] = 0; if (len) *len = (size_t)n; }
    }
    fclose(f);
    return buf;
}

static int write_all(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f && fclose(f)) ok = 0;
    return ok ? 0 : -1;
}

static int copy_file(const char *from, const char *to)
{
    size_t len;
    char *d = read_all(from, &len);
    int rc = d ? write_all(to, d, len) : -1;
    free(d);
    return rc;
}

static int mkdirs(const char *path)
{
    char tmp[1024];
    char *s;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (s = tmp + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
        *s = '/';
    }
    return mkdir(tmp, 0755) && errno != EEXIST ? -1 : 0;
}

static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    if (!d) { unlink(path); return; }
    while ((de = readdir(d))) {
        char child[1024];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        rmtree(child);
    }
    closedir(d);
    rmdir(path);
}

static void new_guid(uint8_t g[16])
{
    arc4random_buf(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0f) | 0x40);
    g[8] = (uint8_t)((g[8] & 0x3f) | 0x80);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static void gpt_entry(uint8_t *e, const uint8_t type[16], const uint8_t guid[16], uint64_t first, uint64_t last, const char *name)
{
    int i;
    memcpy(e, type, 16);
    memcpy(e + 16, guid, 16);
    put64(e + 32, first);
    put64(e + 40, last);
    for (i = 0; name[i] && i < 36; i++) e[56 + 2 * i] = (uint8_t)name[i];
}

static void gpt_header(uint8_t *h, uint64_t cur, uint64_t backup, uint64_t last_usable, const uint8_t disk[16],
                       uint64_t entries, uint32_t crc)
{
    memset(h, 0, SECTOR);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x10000);
    put32(h + 12, 92);
    put64(h + 24, cur);
    put64(h + 32, backup);
    put64(h + 40, 34);
    put64(h + 48, last_usable);
    memcpy(h + 56, disk, 16);
    put64(h + 72, entries);
    put32(h + 80, 128);
    put32(h + 84, 128);
    put32(h + 88, crc);
    put32(h + 16, (uint32_t)crc32(0, h, 92));
}

/* Sparse raw disk with protective MBR, primary and backup GPT. */
static int write_gpt(Prep *p, int fd)
{
    uint64_t total = p->req->disk_bytes / SECTOR, last = total - 1;
    uint8_t mbr[SECTOR], hdr[SECTOR], table[128 * 128];
    uint32_t crc;
    new_guid(p->disk_guid);
    new_guid(p->esp_guid);
    new_guid(p->os_guid);
    p->ntfs_first = ESP_FIRST + ESP_SECTORS;
    p->ntfs_sectors = (last - 33) / 2048 * 2048 - p->ntfs_first;
    memset(table, 0, sizeof table);
    gpt_entry(table, ESP_TYPE, p->esp_guid, ESP_FIRST, ESP_FIRST + ESP_SECTORS - 1, "EFI system partition");
    gpt_entry(table + 128, DATA_TYPE, p->os_guid, p->ntfs_first, p->ntfs_first + p->ntfs_sectors - 1, "Windows");
    crc = (uint32_t)crc32(0, table, sizeof table);
    memset(mbr, 0, sizeof mbr);
    mbr[446 + 2] = 2;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 5] = mbr[446 + 6] = mbr[446 + 7] = 0xFF;
    put32(mbr + 446 + 8, 1);
    put32(mbr + 446 + 12, total - 1 > 0xffffffffull ? 0xffffffffu : (uint32_t)(total - 1));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (ftruncate(fd, (off_t)(total * SECTOR))) return fail(p, "Could not size the disk image: %s", strerror(errno));
    if (pwrite(fd, mbr, SECTOR, 0) != (ssize_t)SECTOR) goto io;
    gpt_header(hdr, 1, last, last - 33, p->disk_guid, 2, crc);
    if (pwrite(fd, hdr, SECTOR, (off_t)SECTOR) != (ssize_t)SECTOR) goto io;
    if (pwrite(fd, table, sizeof table, (off_t)(2 * SECTOR)) != (ssize_t)sizeof table) goto io;
    if (pwrite(fd, table, sizeof table, (off_t)((last - 32) * SECTOR)) != (ssize_t)sizeof table) goto io;
    gpt_header(hdr, last, 1, last - 33, p->disk_guid, last - 32, crc);
    if (pwrite(fd, hdr, SECTOR, (off_t)(last * SECTOR)) != (ssize_t)SECTOR) goto io;
    return 0;
io:
    return fail(p, "Could not write the partition table: %s", strerror(errno));
}

static void ntfs_window(Prep *p, char *out, size_t len)
{
    snprintf(out, len, "hwwin:%llu:%llu:%s", (unsigned long long)(p->ntfs_first * SECTOR),
             (unsigned long long)(p->ntfs_sectors * SECTOR), p->req->disk_path);
}

static int format_ntfs(Prep *p)
{
    char dev[1200], start[32], count[32];
    char *argv[] = {"mkntfs", "-q", "-Q", "-F", "-L", "Windows", "-s", "512", "-c", "4096", "-H", "255", "-S", "63",
                    "-p", start, dev, count, NULL};
    ntfs_window(p, dev, sizeof dev);
    snprintf(start, sizeof start, "%llu", (unsigned long long)p->ntfs_first);
    snprintf(count, sizeof count, "%llu", (unsigned long long)p->ntfs_sectors);
    optind = 1;
#if defined(__APPLE__) || defined(__ANDROID__)
    optreset = 1;
#endif
    if (hw_mkntfs_main((int)(sizeof argv / sizeof *argv) - 1, argv) != 0)
        return fail(p, "Formatting the Windows partition failed.");
    return 0;
}

/* ---- offline SYSTEM hive ---- */

static hive_node_h hive_path(hive_h *h, const char *path)
{
    char buf[512], *s, *save = NULL;
    hive_node_h node = hivex_root(h);
    snprintf(buf, sizeof buf, "%s", path);
    for (s = strtok_r(buf, "\\", &save); s && node; s = strtok_r(NULL, "\\", &save)) {
        hive_node_h child = hivex_node_get_child(h, node, s);
        node = child ? child : hivex_node_add_child(h, node, s);
    }
    return node;
}

/* UTF-8 (BMP only) to UTF-16LE, appending a terminator. Returns bytes written. */
static size_t utf16(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && n + 4 <= cap) {
        unsigned c = *u++;
        if (c >= 0xe0 && u[0] && u[1]) { c = (c & 0x0f) << 12 | (u[0] & 0x3f) << 6 | (u[1] & 0x3f); u += 2; }
        else if (c >= 0xc0 && u[0]) { c = (c & 0x1f) << 6 | (u[0] & 0x3f); u += 1; }
        out[n++] = (uint8_t)c;
        out[n++] = (uint8_t)(c >> 8);
    }
    out[n++] = 0;
    out[n++] = 0;
    return n;
}

static int hive_set(hive_h *h, hive_node_h node, const char *name, hive_type t, const void *data, size_t len)
{
    hive_set_value v;
    v.key = (char *)name;
    v.t = t;
    v.len = len;
    v.value = (char *)data;
    return node ? hivex_node_set_value(h, node, &v, 0) : -1;
}

static int hive_sz(hive_h *h, hive_node_h node, const char *name, const char *value, hive_type t)
{
    uint8_t buf[2048];
    return hive_set(h, node, name, t, buf, utf16(value, buf, sizeof buf));
}

static int hive_dword(hive_h *h, hive_node_h node, const char *name, uint32_t value)
{
    uint8_t buf[4];
    put32(buf, value);
    return hive_set(h, node, name, hive_t_REG_DWORD, buf, 4);
}

static int hive_multi(hive_h *h, hive_node_h node, const char *name, const cJSON *list)
{
    uint8_t buf[4096];
    size_t n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list) if (cJSON_IsString(it) && n + 1024 < sizeof buf) n += utf16(it->valuestring, buf + n, sizeof buf - n - 2);
    buf[n++] = 0;
    buf[n++] = 0;
    return hive_set(h, node, name, hive_t_REG_MULTI_SZ, buf, n);
}

/* values: { "Sub\\Key": { "Name": 1, "Other": "text" } } under base. */
static int hive_values(hive_h *h, const char *base, const cJSON *tree)
{
    const cJSON *sub, *v;
    cJSON_ArrayForEach(sub, tree) {
        char path[512];
        hive_node_h node;
        snprintf(path, sizeof path, "%s\\%s", base, sub->string);
        node = hive_path(h, path);
        cJSON_ArrayForEach(v, sub) {
            int rc = cJSON_IsString(v) ? hive_sz(h, node, v->string, v->valuestring, hive_t_REG_SZ)
                                       : hive_dword(h, node, v->string, (uint32_t)v->valuedouble);
            if (rc) return -1;
        }
    }
    return 0;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static const char *jstr(const cJSON *o, const char *key, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

/* Mirrors tools/offline/chntpw_script.py: service keys, class keys and preseeded Enum devnodes. */
static int register_drivers(Prep *p, const char *hive_file, const cJSON *drivers)
{
    hive_h *h = hivex_open(hive_file, HIVEX_OPEN_WRITE);
    const cJSON *d;
    int i = 0, rc = 0;
    if (!h) return fail(p, "Could not open the offline SYSTEM hive.");
    cJSON_ArrayForEach(d, drivers) {
        char svc[256], image[256];
        const char *service = jstr(d, "service", "");
        const char *desc = jstr(d, "description", service);
        const cJSON *values = cJSON_GetObjectItemCaseSensitive(d, "values");
        const cJSON *dev;
        hive_node_h node;
        int j = 0;
        snprintf(svc, sizeof svc, "ControlSet001\\Services\\%s", service);
        snprintf(image, sizeof image, "System32\\drivers\\%s", base_name(jstr(d, "file", "")));
        node = hive_path(h, svc);
        rc |= hive_dword(h, node, "Type", (uint32_t)jint(d, "type", 1));
        rc |= hive_dword(h, node, "Start", (uint32_t)jint(d, "start", 3));
        rc |= hive_dword(h, node, "ErrorControl", 1);
        rc |= hive_sz(h, node, "ImagePath", image, hive_t_REG_EXPAND_SZ);
        rc |= hive_sz(h, node, "DisplayName", desc, hive_t_REG_SZ);
        if (*jstr(d, "group", "")) rc |= hive_sz(h, node, "Group", jstr(d, "group", ""), hive_t_REG_SZ);
        rc |= hive_values(h, svc, cJSON_GetObjectItemCaseSensitive(values, "service"));
        cJSON_ArrayForEach(dev, cJSON_GetObjectItemCaseSensitive(d, "devices")) {
            char cls[256], inst[512], driver[256], lower[256];
            const char *klass = jstr(dev, "class", jstr(d, "class", ""));
            const char *ddesc = jstr(dev, "description", desc);
            const cJSON *hwids = cJSON_GetObjectItemCaseSensitive(dev, "hardwareIds");
            const cJSON *first = cJSON_GetArrayItem(hwids, 0);
            int slot = 90 + 4 * i + (j < 3 ? j : 3), k;
            snprintf(cls, sizeof cls, "ControlSet001\\Control\\Class\\%s\\%04d", klass, slot);
            snprintf(driver, sizeof driver, "%s\\%04d", klass, slot);
            snprintf(lower, sizeof lower, "%s", cJSON_IsString(first) ? first->valuestring : "");
            for (k = 0; lower[k]; k++) if (lower[k] >= 'A' && lower[k] <= 'Z') lower[k] += 32;
            node = hive_path(h, cls);
            rc |= hive_sz(h, node, "DriverDesc", ddesc, hive_t_REG_SZ);
            rc |= hive_sz(h, node, "ProviderName", "HyperWin", hive_t_REG_SZ);
            rc |= hive_sz(h, node, "MatchingDeviceId", lower, hive_t_REG_SZ);
            rc |= hive_sz(h, node, "DriverVersion", "1.0.0.0", hive_t_REG_SZ);
            snprintf(inst, sizeof inst, "ControlSet001\\Enum\\%s", jstr(dev, "instance", ""));
            node = hive_path(h, inst);
            rc |= hive_sz(h, node, "Service", service, hive_t_REG_SZ);
            rc |= hive_sz(h, node, "ClassGUID", klass, hive_t_REG_SZ);
            rc |= hive_sz(h, node, "Class", jstr(dev, "className", jstr(d, "className", "")), hive_t_REG_SZ);
            rc |= hive_multi(h, node, "HardwareID", hwids);
            if (cJSON_GetObjectItemCaseSensitive(dev, "compatibleIds"))
                rc |= hive_multi(h, node, "CompatibleIDs", cJSON_GetObjectItemCaseSensitive(dev, "compatibleIds"));
            rc |= hive_sz(h, node, "DeviceDesc", ddesc, hive_t_REG_SZ);
            rc |= hive_sz(h, node, "Mfg", "HyperWin", hive_t_REG_SZ);
            rc |= hive_dword(h, node, "ConfigFlags", 0);
            rc |= hive_sz(h, node, "Driver", driver, hive_t_REG_SZ);
            rc |= hive_values(h, inst, cJSON_GetObjectItemCaseSensitive(values, "device"));
            j++;
        }
        i++;
    }
    if (rc || hivex_commit(h, NULL, 0)) {
        hivex_close(h);
        return fail(p, "Could not register drivers in the SYSTEM hive.");
    }
    hivex_close(h);
    return 0;
}

/* ---- answer file and BCD ---- */

static void xml_escape(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        if (r) { memcpy(out + n, r, strlen(r)); n += strlen(r); }
        else out[n++] = *s;
    }
    out[n] = 0;
}

static int write_unattend(Prep *p, const char *tmpl_path, const char *out_path)
{
    char *t = read_all(tmpl_path, NULL), *res, *o;
    const char *keys[3] = {"@USER@", "@PASSWORD@", "@COMPUTER@"};
    char vals[3][512];
    size_t cap;
    if (!t) return fail(p, "Answer file template is missing.");
    xml_escape(p->req->username, vals[0], sizeof vals[0]);
    xml_escape(p->req->password, vals[1], sizeof vals[1]);
    xml_escape(p->req->computer_name, vals[2], sizeof vals[2]);
    cap = strlen(t) * 2 + 4096;
    res = o = malloc(cap);
    if (!res) { free(t); return fail(p, "Out of memory."); }
    for (const char *s = t; *s;) {
        int k, hit = 0;
        for (k = 0; k < 3; k++) {
            size_t kl = strlen(keys[k]);
            if (!strncmp(s, keys[k], kl) && (size_t)(o - res) + strlen(vals[k]) + 1 < cap) {
                strcpy(o, vals[k]);
                o += strlen(vals[k]);
                s += kl;
                hit = 1;
                break;
            }
        }
        if (!hit && (size_t)(o - res) + 2 < cap) *o++ = *s++;
        else if (!hit) break;
    }
    *o = 0;
    free(t);
    if (write_all(out_path, res, strlen(res))) { free(res); return fail(p, "Could not write the answer file."); }
    free(res);
    return 0;
}

static void replace16(uint8_t *d, size_t n, const uint8_t from[16], const uint8_t to[16])
{
    size_t i;
    for (i = 0; i + 16 <= n; i++) if (!memcmp(d + i, from, 16)) memcpy(d + i, to, 16);
}

/* BCD element names (hex type ids) and the values a VM needs. */
#define BCD_RECOVERY_ENABLED "16000009"      /* boolean, one byte */
#define BCD_BOOT_STATUS_POLICY "250000e0"    /* integer, eight bytes: 1 ignores all failures */
#define BCD_SYSTEM_ROOT "22000002"           /* present only on Windows loader entries */

static int bcd_element(hive_h *h, hive_node_h elements, const char *id, const uint8_t *want, size_t len)
{
    hive_node_h e = hivex_node_get_child(h, elements, id);
    hive_value_h v = e ? hivex_node_get_value(h, e, "Element") : 0;
    hive_type t;
    size_t have = 0;
    char *cur = v ? hivex_value_value(h, v, &t, &have) : NULL;
    int same = cur && have == len && !memcmp(cur, want, len);
    free(cur);
    if (same) return 0;
    if (!e) e = hivex_node_add_child(h, elements, id);
    return e && !hive_set(h, e, "Element", hive_t_REG_BINARY, want, len) ? 1 : -1;
}

/* Windows sends a boot that did not finish (the app closed, the phone short of memory) to its
 * recovery environment, which cannot start in this VM: its kernel is a different build. Turn
 * that off in every Windows entry. Returns 1 when the store changed, 0 when it already was so. */
int hw_bcd_harden(const char *path)
{
    static const uint8_t off[1] = {0}, ignore_all[8] = {1};
    hive_h *h = hivex_open(path, HIVEX_OPEN_WRITE);
    hive_node_h objects = h ? hivex_node_get_child(h, hivex_root(h), "Objects") : 0;
    hive_node_h *list = objects ? hivex_node_children(h, objects) : NULL;
    int changed = 0, bad = !list, i;
    for (i = 0; list && list[i]; i++) {
        hive_node_h elements = hivex_node_get_child(h, list[i], "Elements");
        int r1, r2;
        if (!elements || !hivex_node_get_child(h, elements, BCD_SYSTEM_ROOT)) continue;
        r1 = bcd_element(h, elements, BCD_RECOVERY_ENABLED, off, sizeof off);
        r2 = bcd_element(h, elements, BCD_BOOT_STATUS_POLICY, ignore_all, sizeof ignore_all);
        if (r1 < 0 || r2 < 0) bad = 1;
        changed |= r1 > 0 || r2 > 0;
    }
    free(list);
    if (h && changed && !bad && hivex_commit(h, NULL, 0)) bad = 1;
    if (h) hivex_close(h);
    return bad ? -1 : changed;
}

static int write_bcd(Prep *p, const char *tmpl, const char *out)
{
    size_t n;
    uint8_t *d = (uint8_t *)read_all(tmpl, &n);
    int rc;
    if (!d) return fail(p, "BCD template is missing.");
    replace16(d, n, BCD_DISK, p->disk_guid);
    replace16(d, n, BCD_ESP, p->esp_guid);
    replace16(d, n, BCD_OS, p->os_guid);
    rc = write_all(out, d, n);
    free(d);
    if (!rc && hw_bcd_harden(out) < 0) rc = -1;
    return rc ? fail(p, "Could not write the BCD store.") : 0;
}

/* ---- wimlib ---- */

typedef struct { Prep *p; int lo, hi; } WimCtx;

static enum wimlib_progress_status wim_progress(enum wimlib_progress_msg msg, union wimlib_progress_info *info, void *ctx)
{
    WimCtx *w = ctx;
    if (msg == WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS && info->extract.total_bytes) {
        static int last = -1;
        int pct = w->lo + (int)((uint64_t)(w->hi - w->lo) * info->extract.completed_bytes / info->extract.total_bytes);
        if (pct != last) {
            char m[96];
            last = pct;
            snprintf(m, sizeof m, "Copying Windows files (%llu of %llu MB)",
                     (unsigned long long)(info->extract.completed_bytes >> 20), (unsigned long long)(info->extract.total_bytes >> 20));
            progress(w->p, pct, m);
        }
    }
    return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

static int wim_fail(Prep *p, const char *what, int code)
{
    return fail(p, "%s: %s", what, wimlib_get_error_string((enum wimlib_error_code)code));
}

static int find_install_image(Prep *p, int iso_fd, char *window, size_t len)
{
    static const char *names[] = {"sources/install.wim", "sources/install.esd"};
    size_t i;
    for (i = 0; i < 2; i++) {
        uint64_t off, size;
        if (hw_udf_find(iso_fd, names[i], &off, &size) == 0) {
            snprintf(window, len, "hwwin:%llu:%llu:%s", (unsigned long long)off, (unsigned long long)size, p->req->iso_path);
            return 0;
        }
    }
    return fail(p, "This ISO has no sources/install.wim or install.esd.");
}

static int prepare(Prep *p)
{
    const HwPrepRequest *r = p->req;
    char window[1200], path[1200], x[1100], esp[1100], tmp[1200], tmp2[1200];
    WIMStruct *wim = NULL;
    WimCtx wc = {p, 0, 0};
    cJSON *manifest = NULL;
    const cJSON *d;
    int fd = -1, iso = -1, rc = -1, ret, image;
    char *json;
    struct wimlib_update_command *cmds = NULL;
    int ncmds = 0;

    progress(p, 0, "Reading the ISO");
    iso = open(r->iso_path, O_RDONLY);
    if (iso < 0) return fail(p, "Could not open the ISO: %s", strerror(errno));
    if (find_install_image(p, iso, window, sizeof window)) goto out;
    if ((ret = wimlib_global_init(0))) { wim_fail(p, "wimlib", ret); goto out; }
    if ((ret = wimlib_open_wim_with_progress(window, 0, &wim, wim_progress, &wc))) { wim_fail(p, "Could not read the Windows image", ret); goto out; }
    {
        struct wimlib_wim_info info;
        wimlib_get_wim_info(wim, &info);
        image = r->image_index;
        if (image < 1 || (uint32_t)image > info.image_count) { fail(p, "Edition %d is not in this ISO (it has %u).", image, info.image_count); goto out; }
    }

    snprintf(x, sizeof x, "%s/x", p->work);
    snprintf(esp, sizeof esp, "%s/esp", p->work);
    rmtree(p->work);
    if (mkdirs(x) || mkdirs(esp)) { fail(p, "Could not create %s.", p->work); goto out; }

    progress(p, 1, "Reading boot files and registry");
    {
        const char *paths[] = {"/Windows/System32/config/SYSTEM", "/Windows/Boot/EFI", "/Windows/Boot/Fonts"};
        ret = wimlib_extract_paths(wim, image, x, paths, 3, WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES);
        if (ret) { wim_fail(p, "Could not read boot files from the image", ret); goto out; }
    }

    progress(p, 2, "Adding drivers");
    snprintf(path, sizeof path, "%s/drivers.json", r->assets_dir);
    json = read_all(path, NULL);
    manifest = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!manifest) { fail(p, "drivers.json is missing or invalid."); goto out; }
    snprintf(tmp, sizeof tmp, "%s/Windows/System32/config/SYSTEM", x);
    if (register_drivers(p, tmp, cJSON_GetObjectItemCaseSensitive(manifest, "drivers"))) goto out;
    snprintf(tmp2, sizeof tmp2, "%s/unattend.xml", p->work);
    snprintf(path, sizeof path, "%s/unattend.xml", r->assets_dir);
    if (write_unattend(p, path, tmp2)) goto out;

    /* Image edits applied in memory before extraction: hive, driver files, answer file. */
    cmds = calloc(64, sizeof *cmds);
    if (!cmds) { fail(p, "Out of memory."); goto out; }
#define ADD(src, dst) do { cmds[ncmds].op = WIMLIB_UPDATE_OP_ADD; \
        cmds[ncmds].add.fs_source_path = strdup(src); cmds[ncmds].add.wim_target_path = strdup(dst); \
        cmds[ncmds].add.add_flags = WIMLIB_ADD_FLAG_NO_ACLS; ncmds++; } while (0)
    ADD(tmp, "/Windows/System32/config/SYSTEM");
    ADD(tmp2, "/Windows/Panther/unattend.xml");
    snprintf(path, sizeof path, "%s/guest", r->assets_dir);   /* first-boot script and extras */
    if (!access(path, R_OK)) ADD(path, "/HyperWin");
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(manifest, "drivers")) {
        char dst[256];
        const char *file = base_name(jstr(d, "file", ""));
        snprintf(path, sizeof path, "%s/drivers/%s", r->assets_dir, file);
        if (access(path, R_OK)) { fail(p, "Driver %s is missing from the app.", file); goto out; }
        snprintf(dst, sizeof dst, "/Windows/System32/drivers/%s", file);
        if (ncmds < 62) ADD(path, dst);
    }
#undef ADD
    if ((ret = wimlib_update_image(wim, image, cmds, (size_t)ncmds, 0))) { wim_fail(p, "Could not add drivers to the image", ret); goto out; }

    progress(p, 3, "Creating the disk");
    fd = open(r->disk_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fail(p, "Could not create %s: %s", r->disk_path, strerror(errno)); goto out; }
    if (write_gpt(p, fd)) goto out;

    progress(p, 4, "Formatting the Windows partition");
    if (format_ntfs(p)) goto out;

    wc.lo = 5;
    wc.hi = 97;
    ntfs_window(p, tmp, sizeof tmp);
    if ((ret = wimlib_extract_image(wim, image, tmp, WIMLIB_EXTRACT_FLAG_NTFS))) { wim_fail(p, "Copying Windows failed", ret); goto out; }

    progress(p, 98, "Writing the boot partition");
    snprintf(path, sizeof path, "%s/EFI/Microsoft", esp);
    snprintf(tmp, sizeof tmp, "%s/EFI/BOOT", esp);
    if (mkdirs(path) || mkdirs(tmp)) { fail(p, "Could not stage the boot partition."); goto out; }
    snprintf(tmp2, sizeof tmp2, "%s/Windows/Boot/EFI", x);
    snprintf(path, sizeof path, "%s/EFI/Microsoft/Boot", esp);
    if (rename(tmp2, path)) { fail(p, "Windows boot files are missing from the image."); goto out; }
    /* Boot-time code integrity policies from the ESP block our test-signed boot drivers. */
    snprintf(tmp2, sizeof tmp2, "%s/EFI/Microsoft/Boot/CIPolicies", esp);
    rmtree(tmp2);
    snprintf(tmp2, sizeof tmp2, "%s/Windows/Boot/Fonts", x);
    snprintf(path, sizeof path, "%s/EFI/Microsoft/Boot/Fonts", esp);
    rename(tmp2, path);
    snprintf(tmp2, sizeof tmp2, "%s/BCD", r->assets_dir);
    snprintf(path, sizeof path, "%s/EFI/Microsoft/Boot/BCD", esp);
    if (write_bcd(p, tmp2, path)) goto out;
    {
        static const char *efi[] = {"BOOTAA64.EFI", "GOP.EFI", "COMPAT.EFI"};
        int k;
        for (k = 0; k < 3; k++) {
            snprintf(tmp2, sizeof tmp2, "%s/efi/%s", r->assets_dir, efi[k]);
            snprintf(path, sizeof path, "%s/EFI/BOOT/%s", esp, efi[k]);
            if (copy_file(tmp2, path)) { fail(p, "Boot loader %s is missing from the app.", efi[k]); goto out; }
        }
    }
    if (hw_fat32_write(esp, fd, ESP_FIRST * SECTOR, ESP_SECTORS, "SYSTEM")) { fail(p, "Could not write the boot partition."); goto out; }
    if (fsync(fd)) { fail(p, "Could not flush the disk image: %s", strerror(errno)); goto out; }
    progress(p, 100, "Done");
    rc = 0;
out:
    if (cmds) {
        int k;
        for (k = 0; k < ncmds; k++) { free(cmds[k].add.fs_source_path); free(cmds[k].add.wim_target_path); }
        free(cmds);
    }
    cJSON_Delete(manifest);
    if (wim) wimlib_free(wim);
    if (fd >= 0) close(fd);
    if (iso >= 0) close(iso);
    rmtree(p->work);
    return rc;
}

int hw_prepare_machine(const HwPrepRequest *req, char *err, size_t errlen)
{
    Prep p;
    int rc;
    memset(&p, 0, sizeof p);
    p.req = req;
    p.err = err;
    p.errlen = errlen;
    snprintf(err, errlen, "ok");
    snprintf(p.work, sizeof p.work, "%s", req->work_dir);
    rc = prepare(&p);
    if (rc) unlink(req->disk_path);
    return rc;
}

#ifdef HW_PREP_MAIN
static void print_progress(void *ctx, int pct, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "[%3d%%] %s\n", pct, msg);
}

/* usage: machine_prep ISO ASSETS_DIR OUT.img SIZE_GB [INDEX [USER]] */
int main(int argc, char **argv)
{
    HwPrepRequest r;
    char err[512], work[1100];
    if (argc < 5) { fprintf(stderr, "usage: %s ISO ASSETS OUT.img SIZE_GB [INDEX [USER]]\n", argv[0]); return 2; }
    memset(&r, 0, sizeof r);
    snprintf(work, sizeof work, "%s.work", argv[3]);
    r.iso_path = argv[1];
    r.assets_dir = argv[2];
    r.disk_path = argv[3];
    r.work_dir = work;
    r.disk_bytes = strtoull(argv[4], NULL, 10) << 30;
    r.image_index = argc > 5 ? atoi(argv[5]) : 1;
    r.username = argc > 6 ? argv[6] : "User";
    r.password = "";
    r.computer_name = "HYPERWIN";
    r.progress = print_progress;
    if (hw_prepare_machine(&r, err, sizeof err)) { fprintf(stderr, "error: %s\n", err); return 1; }
    return 0;
}
#endif
