/* fs.file und fs.async_read; arbeitet in einem Ordner im aktuellen Verzeichnis. */
#include "nhal/features/fs/async_read.h"
#include "nhal/features/fs/file.h"
#include "check.h"

#include <string.h>

#define DIR_NAME "nhal_fs_test_dir"
#define FILE_NAME DIR_NAME "/daten.bin"

typedef struct listing {
    int files;
    int dirs;
    bool found;
} listing;

static bool on_entry(void* user, const nhal_dir_entry* e) {
    listing* l = (listing*)user;
    if (e->is_dir) ++l->dirs; else ++l->files;
    if (strcmp(e->name, "daten.bin") == 0 && e->size == 1000) l->found = true;
    return true;
}

typedef struct read_result {
    int calls;
    nhal_status status;
    size_t bytes;
} read_result;

static void on_read(void* user, nhal_status status, size_t bytes) {
    read_result* r = (read_result*)user;
    ++r->calls;
    r->status = status;
    r->bytes = bytes;
}

static void test_file(const nhal_fs_file_v1* fs) {
    nhal_file_stat st;
    CHECK_OK(fs->stat(DIR_NAME, &st));
    if (st.exists) { /* Reste eines abgebrochenen Laufs */
        fs->remove(FILE_NAME);
        fs->remove(DIR_NAME "/unter");
        fs->remove(DIR_NAME);
    }
    CHECK_OK(fs->make_dir(DIR_NAME));
    CHECK_OK(fs->make_dir(DIR_NAME "/unter"));

    unsigned char data[1000];
    for (int i = 0; i < 1000; ++i) data[i] = (unsigned char)(i * 7);

    nhal_file* f = NULL;
    CHECK_OK(fs->open(FILE_NAME, NHAL_FILE_WRITE | NHAL_FILE_CREATE | NHAL_FILE_TRUNCATE, &f));
    if (f) {
        CHECK_OK(fs->write(f, data, 600));
        CHECK_OK(fs->write(f, data + 600, 400));
        CHECK_OK(fs->sync(f));
        uint64_t size = 0;
        CHECK_OK(fs->size(f, &size));
        CHECK(size == 1000);
        fs->close(f);
    }

    nhal_file* g = NULL;
    CHECK(fs->open(FILE_NAME, NHAL_FILE_WRITE | NHAL_FILE_CREATE | NHAL_FILE_EXCLUSIVE, &g).code != NHAL_OK);
    CHECK(g == NULL);

    CHECK_OK(fs->open(FILE_NAME, NHAL_FILE_READ, &f));
    if (f) {
        unsigned char back[2000];
        size_t done = 0;
        uint64_t pos = 0;
        CHECK_OK(fs->seek(f, 100, NHAL_SEEK_SET, &pos));
        CHECK(pos == 100);
        CHECK_OK(fs->read(f, back, 50, &done));
        CHECK(done == 50 && memcmp(back, data + 100, 50) == 0);
        CHECK_OK(fs->seek(f, -10, NHAL_SEEK_END, &pos));
        CHECK(pos == 990);
        CHECK_OK(fs->read(f, back, sizeof back, &done)); /* Dateiende ist kein Fehler */
        CHECK(done == 10 && memcmp(back, data + 990, 10) == 0);
        CHECK_OK(fs->read(f, back, 10, &done));
        CHECK(done == 0);
        fs->close(f);
    }

    CHECK_OK(fs->open(FILE_NAME, NHAL_FILE_APPEND, &f));
    if (f) {
        CHECK_OK(fs->write(f, "xyz", 3));
        uint64_t size = 0;
        CHECK_OK(fs->size(f, &size));
        CHECK(size == 1003);
        fs->close(f);
    }
    CHECK_OK(fs->open(FILE_NAME, NHAL_FILE_WRITE, &f)); /* ohne TRUNCATE: Inhalt bleibt */
    if (f) {
        uint64_t size = 0;
        CHECK_OK(fs->size(f, &size));
        CHECK(size == 1003);
        fs->close(f);
    }
    CHECK_OK(fs->open(FILE_NAME, NHAL_FILE_WRITE | NHAL_FILE_TRUNCATE, &f));
    if (f) {
        CHECK_OK(fs->write(f, data, 1000));
        fs->close(f);
    }

    CHECK_OK(fs->stat(FILE_NAME, &st));
    CHECK(st.exists && !st.is_dir && st.size == 1000);
    CHECK_OK(fs->stat(DIR_NAME, &st));
    CHECK(st.exists && st.is_dir);
    CHECK_OK(fs->stat(DIR_NAME "/gibt_es_nicht", &st));
    CHECK(!st.exists);

    listing l = { 0, 0, false };
    CHECK_OK(fs->list_dir(DIR_NAME, on_entry, &l));
    CHECK(l.files == 1 && l.dirs == 1 && l.found);

    nhal_status s = fs->open(DIR_NAME "/fehlt.bin", NHAL_FILE_READ, &f);
    CHECK(s.code == NHAL_ERR_NOT_FOUND);
    CHECK(s.reason && s.reason[0]);
    CHECK(fs->open(DIR_NAME, NHAL_FILE_READ, &f).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(fs->open(FILE_NAME, 0, &f).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(fs->remove(DIR_NAME).code != NHAL_OK); /* nicht leer */
}

static void test_async(const nhal_fs_async_read_v1* ar) {
    unsigned char expect[1000];
    for (int i = 0; i < 1000; ++i) expect[i] = (unsigned char)(i * 7);

    enum { N = 20 };
    unsigned char bufs[N][100];
    read_result results[N];
    memset(results, 0, sizeof results);
    for (int i = 0; i < N; ++i) {
        CHECK_OK(ar->submit(FILE_NAME, (uint64_t)i * 50, 100, bufs[i], on_read, &results[i]));
    }
    CHECK(ar->pending() == N);
    for (int i = 0; i < N; ++i) CHECK(results[i].calls == 0); /* nie innerhalb von submit */
    ar->drain();
    CHECK(ar->pending() == 0);
    for (int i = 0; i < N; ++i) {
        const size_t want = i * 50 + 100 <= 1000 ? 100 : (size_t)(1000 - i * 50);
        CHECK(results[i].calls == 1);
        CHECK(results[i].status.code == NHAL_OK);
        CHECK(results[i].bytes == want);
        CHECK(memcmp(bufs[i], expect + i * 50, want) == 0);
    }

    read_result miss = { 0, { NHAL_OK, 0, "" }, 0 };
    unsigned char tmp[8];
    CHECK_OK(ar->submit(DIR_NAME "/fehlt.bin", 0, sizeof tmp, tmp, on_read, &miss));
    read_result past = { 0, { NHAL_OK, 0, "" }, 0 };
    CHECK_OK(ar->submit(FILE_NAME, 5000, sizeof tmp, tmp, on_read, &past));
    while (ar->pending() > 0) ar->poll(1);
    CHECK(miss.calls == 1 && miss.status.code == NHAL_ERR_NOT_FOUND);
    CHECK(past.calls == 1 && past.status.code == NHAL_OK && past.bytes == 0);

    CHECK(ar->submit("", 0, 1, tmp, on_read, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(ar->submit(FILE_NAME, 0, 1, tmp, NULL, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
}

int main(void) {
    nhal_request req[] = {
        { NHAL_FEATURE_FS_FILE, 1u, NHAL_REQUIRED },
        { NHAL_FEATURE_FS_ASYNC_READ, 1u, NHAL_OPTIONAL },
    };
    const nhal_manifest m = { req, 2 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    CHECK_OK(nhal_init(&info, &ctx));
    const nhal_fs_file_v1* fs = nhal_get_fs_file_v1(ctx);
    CHECK(fs != NULL);
    if (!fs) return check_report("fs_test");

    test_file(fs);
    const nhal_fs_async_read_v1* ar = nhal_get_fs_async_read_v1(ctx);
    if (ar) test_async(ar);

    CHECK_OK(fs->remove(FILE_NAME));
    CHECK_OK(fs->remove(DIR_NAME "/unter"));
    CHECK_OK(fs->remove(DIR_NAME));
    nhal_shutdown(ctx);
    return check_report(ar ? "fs_test (file + async_read)" : "fs_test (file)");
}
