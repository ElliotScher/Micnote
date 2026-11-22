#include "recstore.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include <mbedtls/platform_util.h>

#include "aead.h"

/* "<dir>/<16 hex digits><ext>" without printf: %llx isn't in every libc
 * configuration Zephyr builds with. */
static int rec_path(char *out, const char *dir, uint64_t rec_id, const char *ext)
{
    static const char hex[] = "0123456789abcdef";
    const size_t dir_len = strlen(dir);
    const size_t ext_len = strlen(ext);

    if (dir_len + 1 + 16 + ext_len + 1 > MN_RECSTORE_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    for (int i = 0; i < 16; i++) {
        out[dir_len + 1 + i] = hex[(rec_id >> (60 - 4 * i)) & 0xF];
    }
    memcpy(&out[dir_len + 17], ext, ext_len + 1);
    return 0;
}

int mn_recstore_init(const char *dir)
{
    struct fs_dirent entry;
    int ret = fs_stat(dir, &entry);

    if (ret == -ENOENT) {
        return fs_mkdir(dir);
    }
    if (ret == 0 && entry.type != FS_DIR_ENTRY_DIR) {
        return -ENOTDIR;
    }
    return ret;
}

int mn_rec_secrets_random(struct mn_rec_secrets *s)
{
    return psa_generate_random((uint8_t *)s, sizeof(*s)) == PSA_SUCCESS ? 0 : -EIO;
}

static void wipe(struct mn_rec *r)
{
    psa_destroy_key(r->cek);
    r->cek = PSA_KEY_ID_NULL;
    mbedtls_platform_zeroize(r->pt, sizeof(r->pt));
    mbedtls_platform_zeroize(r->record, sizeof(r->record));
    r->open = false;
}

/* Seals the plaintext chunk and appends it as one record, then syncs. */
static int write_chunk(struct mn_rec *r)
{
    const struct mn_mnr_chunk_hdr ch = {.seq = r->seq, .ct_len = (uint16_t)r->pt_len};
    int ret =
        mn_chunk_seal(r->cek, r->rec_id, r->seq, r->core_hash, r->pt, r->pt_len,
                      &r->record[MN_MNR_CHUNK_HDR_SIZE], sizeof(r->record) - MN_MNR_CHUNK_HDR_SIZE);

    if (ret < 0) {
        return ret;
    }
    mn_mnr_chunk_hdr_write(&ch, r->record);

    const size_t len = MN_MNR_CHUNK_HDR_SIZE + (size_t)ret;
    const ssize_t written = fs_write(&r->file, r->record, len);

    if (written < 0) {
        return (int)written;
    }
    if ((size_t)written != len) {
        return -ENOSPC;
    }
    ret = fs_sync(&r->file);
    if (ret < 0) {
        return ret;
    }
    r->seq++;
    return 0;
}

/* Writes the audio chunk being filled, if it holds anything. */
static int flush_audio(struct mn_rec *r)
{
    if (r->pt_len <= 1) {
        return 0;
    }
    const int ret = write_chunk(r);

    if (ret == 0) {
        r->pt[0] = MN_CHUNK_AUDIO;
        r->pt_len = 1;
    }
    return ret;
}

/* Makes room for n more plaintext bytes in the current audio chunk. */
static int reserve(struct mn_rec *r, size_t n)
{
    if (r->pt_len + n > sizeof(r->pt)) {
        return flush_audio(r);
    }
    return 0;
}

int mn_rec_open(struct mn_rec *r, const char *dir, const struct mn_rec_params *p,
                struct mn_rec_secrets *s)
{
    static struct mn_mnr_header h;
    static uint8_t header[MN_MNR_HEADER_SIZE];
    struct fs_dirent entry;
    int ret;

    memset(r, 0, sizeof(*r));
    fs_file_t_init(&r->file);

    if (p->n_recipients == 0 || p->n_recipients > MN_MNR_MAX_WRAPS || p->frame_ms == 0) {
        ret = -EINVAL;
        goto out;
    }
    ret = rec_path(r->path, dir, p->rec_id, ".part");
    if (ret < 0) {
        goto out;
    }
    /* A recording is never overwritten, finished or not. */
    char done[MN_RECSTORE_PATH_MAX];

    ret = rec_path(done, dir, p->rec_id, ".mnr");
    if (ret < 0) {
        goto out;
    }
    if (fs_stat(r->path, &entry) == 0 || fs_stat(done, &entry) == 0) {
        ret = -EEXIST;
        goto out;
    }

    memset(&h, 0, sizeof(h));
    h.flags = p->flags;
    h.rec_id = p->rec_id;
    h.codec = MN_MNR_CODEC_OPUS;
    h.rate_hz = 16000;
    h.frame_ms = p->frame_ms;
    h.start_boot = p->start_boot;
    h.start_uptime_ms = p->start_uptime_ms;
    h.start_unix_ms = p->start_unix_ms;
    h.n_wraps = p->n_recipients;
    for (uint8_t i = 0; i < p->n_recipients; i++) {
        ret = mn_wrap_cek(s->eph_priv[i], p->recipients[i].pub, p->recipients[i].host_id, p->rec_id,
                          s->cek, &h.wraps[i]);
        if (ret < 0) {
            goto out;
        }
    }

    ret = mn_mnr_header_write(&h, header, sizeof(header));
    if (ret < 0) {
        goto out;
    }
    ret = mn_header_core_hash(header, r->core_hash);
    if (ret < 0) {
        goto out;
    }
    ret = mn_cek_import(s->cek, &r->cek);
    if (ret < 0) {
        goto out;
    }

    ret = fs_open(&r->file, r->path, FS_O_CREATE | FS_O_RDWR);
    if (ret < 0) {
        goto out;
    }
    const ssize_t written = fs_write(&r->file, header, sizeof(header));

    if (written != (ssize_t)sizeof(header)) {
        ret = written < 0 ? (int)written : -ENOSPC;
        fs_close(&r->file);
        fs_unlink(r->path);
        goto out;
    }
    ret = fs_sync(&r->file);
    if (ret < 0) {
        fs_close(&r->file);
        fs_unlink(r->path);
        goto out;
    }

    r->rec_id = p->rec_id;
    r->frame_ms = p->frame_ms;
    r->pt[0] = MN_CHUNK_AUDIO;
    r->pt_len = 1;
    r->open = true;

out:
    /* The raw CEK and ephemeral keys are needed only for the wraps above. */
    mbedtls_platform_zeroize(s, sizeof(*s));
    if (ret < 0) {
        wipe(r);
    }
    return ret;
}

int mn_rec_append(struct mn_rec *r, const uint8_t *packet, size_t len)
{
    if (!r->open) {
        return -EBADF;
    }
    if (len == 0 || len >= MN_CHUNK_MARKER_SENTINEL) {
        return -EINVAL;
    }
    int ret = reserve(r, 2 + len);

    if (ret < 0) {
        return ret;
    }
    if (r->pt_len + 2 + len > sizeof(r->pt)) {
        /* Even an empty chunk can't hold it. */
        return -EMSGSIZE;
    }
    sys_put_le16((uint16_t)len, &r->pt[r->pt_len]);
    memcpy(&r->pt[r->pt_len + 2], packet, len);
    r->pt_len += 2 + len;
    r->duration_ms += r->frame_ms;
    return 0;
}

int mn_rec_marker(struct mn_rec *r, uint8_t type, uint32_t offset_ms)
{
    if (!r->open) {
        return -EBADF;
    }
    const int ret = reserve(r, MN_CHUNK_MARKER_SIZE);

    if (ret < 0) {
        return ret;
    }
    sys_put_le16(MN_CHUNK_MARKER_SENTINEL, &r->pt[r->pt_len]);
    r->pt[r->pt_len + 2] = type;
    sys_put_le32(offset_ms, &r->pt[r->pt_len + 3]);
    r->pt_len += MN_CHUNK_MARKER_SIZE;
    r->marker_count++;
    return 0;
}

int mn_rec_close(struct mn_rec *r)
{
    char done[MN_RECSTORE_PATH_MAX];

    if (!r->open) {
        return -EBADF;
    }
    int ret = flush_audio(r);

    if (ret == 0) {
        r->pt[0] = MN_CHUNK_TRAILER;
        sys_put_le32(r->duration_ms, &r->pt[1]);
        sys_put_le32(r->marker_count, &r->pt[5]);
        r->pt_len = MN_CHUNK_TRAILER_SIZE;
        ret = write_chunk(r);
    }
    const int close_ret = fs_close(&r->file);

    if (ret == 0) {
        ret = close_ret;
    }
    if (ret == 0) {
        /* The path ends in ".part": swap the extension. */
        const size_t base = strlen(r->path) - strlen(".part");

        memcpy(done, r->path, base);
        memcpy(&done[base], ".mnr", sizeof(".mnr"));
        ret = fs_rename(r->path, done);
    }
    wipe(r);
    return ret;
}

/* ---- recovery ---------------------------------------------------------- */

static bool ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s);
    const size_t m = strlen(suffix);

    return n >= m && strcmp(&s[n - m], suffix) == 0;
}

/* The first .part file name in dir, or "" if none. */
static int find_part(const char *dir, char *name, size_t cap)
{
    struct fs_dir_t d;
    struct fs_dirent entry;
    int ret;

    fs_dir_t_init(&d);
    ret = fs_opendir(&d, dir);
    if (ret < 0) {
        return ret;
    }
    name[0] = '\0';
    for (;;) {
        ret = fs_readdir(&d, &entry);
        if (ret < 0 || entry.name[0] == '\0') {
            break;
        }
        if (entry.type == FS_DIR_ENTRY_FILE && ends_with(entry.name, ".part") &&
            strlen(entry.name) < cap) {
            strcpy(name, entry.name);
            break;
        }
    }
    fs_closedir(&d);
    return ret < 0 ? ret : 0;
}

/* The length of the valid prefix of an open .part file: its header plus every
 * complete, in-sequence chunk record. -EBADMSG if the header itself is
 * incomplete or invalid. */
static int valid_length(struct fs_file_t *f, size_t size, off_t *out)
{
    static uint8_t header[MN_MNR_HEADER_MAX];
    static struct mn_mnr_header h;
    uint8_t ch_bytes[MN_MNR_CHUNK_HDR_SIZE];
    struct mn_mnr_chunk_hdr ch;

    ssize_t n = fs_read(f, header, sizeof(header));

    if (n < 0) {
        return (int)n;
    }
    const int hdr_len = mn_mnr_header_parse(header, (size_t)n, &h);

    if (hdr_len < 0) {
        return -EBADMSG;
    }

    off_t off = hdr_len;

    for (uint32_t seq = 0;; seq++) {
        int ret = fs_seek(f, off, FS_SEEK_SET);

        if (ret < 0) {
            return ret;
        }
        n = fs_read(f, ch_bytes, sizeof(ch_bytes));
        if (n < 0) {
            return (int)n;
        }
        if (n != sizeof(ch_bytes) || mn_mnr_chunk_hdr_parse(ch_bytes, &ch) < 0 || ch.seq != seq) {
            break;
        }
        const off_t end = off + MN_MNR_CHUNK_HDR_SIZE + ch.ct_len + MN_MNR_TAG_SIZE;

        if ((size_t)end > size) {
            break;
        }
        off = end;
    }
    *out = off;
    return 0;
}

static int recover_one(const char *dir, const char *name)
{
    char path[MN_RECSTORE_PATH_MAX];
    char done[MN_RECSTORE_PATH_MAX];
    struct fs_dirent entry;
    struct fs_file_t f;
    off_t keep = 0;

    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);

    if (dir_len + 1 + name_len + 1 > sizeof(path)) {
        return -ENAMETOOLONG;
    }
    memcpy(path, dir, dir_len);
    path[dir_len] = '/';
    memcpy(&path[dir_len + 1], name, name_len + 1);

    int ret = fs_stat(path, &entry);

    if (ret < 0) {
        return ret;
    }
    fs_file_t_init(&f);
    ret = fs_open(&f, path, FS_O_RDWR);
    if (ret < 0) {
        return ret;
    }
    ret = valid_length(&f, entry.size, &keep);
    if (ret == -EBADMSG) {
        /* No complete header means no key wraps: nothing here can ever be
         * decrypted. The one case where the puck deletes a recording
         * without an Ack. */
        fs_close(&f);
        return fs_unlink(path);
    }
    if (ret == 0 && (size_t)keep < entry.size) {
        ret = fs_truncate(&f, keep);
    }
    const int close_ret = fs_close(&f);

    if (ret < 0) {
        return ret;
    }
    if (close_ret < 0) {
        return close_ret;
    }
    const size_t base = strlen(path) - strlen(".part");

    memcpy(done, path, base);
    memcpy(&done[base], ".mnr", sizeof(".mnr"));
    ret = fs_rename(path, done);
    return ret < 0 ? ret : 1;
}

int mn_recstore_recover(const char *dir)
{
    char name[MN_RECSTORE_PATH_MAX];
    int recovered = 0;

    /* Rescan after each file instead of renaming while a directory is open. */
    for (;;) {
        int ret = find_part(dir, name, sizeof(name));

        if (ret < 0) {
            return ret;
        }
        if (name[0] == '\0') {
            return recovered;
        }
        ret = recover_one(dir, name);
        if (ret < 0) {
            return ret;
        }
        recovered += ret;
    }
}
