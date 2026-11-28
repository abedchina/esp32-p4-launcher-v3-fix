/*
 * "os", open() and file loading for Developer Mode, on the launcher's FS interface.
 *
 * Python sees "/" = /sd/python on the card (main.py, boot.py, lib/ - like the flash of
 * an ESP32), so Thonny's file panel shows the student's files only. Paths that start
 * with /sd reach the whole card (pictures, models).
 */
#include <string.h>
#include <stdio.h>
#include "py/runtime.h"
#include "py/stream.h"
#include "py/objstr.h"
#include "py/builtin.h"
#include "py/lexer.h"
#include "py/reader.h"
#include "py/mperrno.h"
#include "genhdr/mpversion.h"
#include "papp/papp.h"
#include "devmode.h"

static char s_cwd[128] = "/";

void dm_os_reset(void) { strcpy(s_cwd, "/"); }

static const papp_if_fs_t *fs2(void)
{
    const papp_if_fs_t *f = papp_fs();
    if (!f) mp_raise_OSError(MP_ENODEV);
    return f;
}

/* "/a/./b/../c" -> "/a/c" in place */
static void normalise(char *p)
{
    char out[256];
    size_t o = 0;
    const char *s = p;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == 1 && s[0] == '.') {
        } else if (l == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
        } else if (o + 1 + l < sizeof out) {
            out[o++] = '/';
            memcpy(out + o, s, l);
            o += l;
        }
        s += l;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    strcpy(p, out);
}

/* Python path -> absolute Python path (cwd applied) */
static void py_abs(const char *py, char *out, size_t n)
{
    if (py[0] == '/') snprintf(out, n, "%s", py);
    else snprintf(out, n, "%s/%s", s_cwd, py);
    normalise(out);
}

void dm_path(const char *py, char *out, size_t n)
{
    char a[256];
    py_abs(py, a, sizeof a);
    if (!strcmp(a, "/sd") || !strncmp(a, "/sd/", 4)) snprintf(out, n, "%s", a);
    else if (!strcmp(a, "/")) snprintf(out, n, "%s", DM_ROOT);
    else snprintf(out, n, "%s%s", DM_ROOT, a);
}

static void real_path(mp_obj_t path_in, char *out, size_t n)
{
    dm_path(mp_obj_str_get_str(path_in), out, n);
}

/* â”€â”€ file objects â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

typedef struct {
    mp_obj_base_t base;
    papp_file_t *f;
} dm_file_obj_t;

static void check_open(dm_file_obj_t *o)
{
    if (!o->f) mp_raise_ValueError(MP_ERROR_TEXT("I/O operation on closed file"));
}

static mp_uint_t file_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode)
{
    dm_file_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (!o->f) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }
    return (mp_uint_t)papp_fs()->read(o->f, buf, size);
}

static mp_uint_t file_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode)
{
    dm_file_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (!o->f) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }
    size_t n = papp_fs()->write(o->f, buf, size);
    if (n == 0 && size) { *errcode = MP_EIO; return MP_STREAM_ERROR; }
    return n;
}

static mp_uint_t file_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    dm_file_obj_t *o = MP_OBJ_TO_PTR(self_in);
    const papp_if_fs_t *f = papp_fs();
    if (request == MP_STREAM_CLOSE) {
        if (o->f) { f->close(o->f); o->f = NULL; }
        return 0;
    }
    if (!o->f) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }
    switch (request) {
    case MP_STREAM_FLUSH:
        if (f->hdr.version >= 2) f->flush(o->f);
        return 0;
    case MP_STREAM_SEEK: {
        struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
        if (f->seek(o->f, (long)s->offset, s->whence) != 0) { *errcode = MP_EINVAL; return MP_STREAM_ERROR; }
        s->offset = f->tell(o->f);
        return 0;
    }
    default:
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
}

static void file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_printf(print, "<io.%s %p>", mp_obj_get_type_str(self_in), MP_OBJ_TO_PTR(self_in));
}

static const mp_rom_map_elem_t file_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(file_locals_dict, file_locals_table);

static const mp_stream_p_t fileio_stream_p = { .read = file_read, .write = file_write, .ioctl = file_ioctl };
static const mp_stream_p_t textio_stream_p = { .read = file_read, .write = file_write, .ioctl = file_ioctl, .is_text = true };

MP_DEFINE_CONST_OBJ_TYPE(dm_type_fileio, MP_QSTR_FileIO, MP_TYPE_FLAG_ITER_IS_STREAM,
    print, file_print, protocol, &fileio_stream_p, locals_dict, &file_locals_dict);
MP_DEFINE_CONST_OBJ_TYPE(dm_type_textio, MP_QSTR_TextIOWrapper, MP_TYPE_FLAG_ITER_IS_STREAM,
    print, file_print, protocol, &textio_stream_p, locals_dict, &file_locals_dict);

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    enum { ARG_file, ARG_mode };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_file, MP_ARG_OBJ | MP_ARG_REQUIRED, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_mode, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_r) } },
        { MP_QSTR_buffering, MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_encoding, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, args, kwargs, MP_ARRAY_SIZE(allowed), allowed, a);
    const char *m = mp_obj_str_get_str(a[ARG_mode].u_obj);
    char base = 'r';
    bool plus = false, bin = false;
    for (const char *c = m; *c; c++) {
        if (*c == 'r' || *c == 'w' || *c == 'a' || *c == 'x') base = *c == 'x' ? 'w' : *c;
        else if (*c == '+') plus = true;
        else if (*c == 'b') bin = true;
    }
    char fm[4] = { base, plus ? '+' : 'b', plus ? 'b' : 0, 0 };
    char path[256];
    real_path(a[ARG_file].u_obj, path, sizeof path);
    papp_file_t *f = fs2()->open(path, fm);
    if (!f) mp_raise_OSError(MP_ENOENT);
    dm_file_obj_t *o = mp_obj_malloc_with_finaliser(dm_file_obj_t, bin ? &dm_type_fileio : &dm_type_textio);
    o->f = f;
    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);

/* â”€â”€ import / run files â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

mp_import_stat_t mp_import_stat(const char *path)
{
    const papp_if_fs_t *f = papp_fs();
    char real[256];
    dm_path(path, real, sizeof real);
    papp_stat_t st;
    if (!f || f->hdr.version < 2 || f->stat(real, &st) != 0) return MP_IMPORT_STAT_NO_EXIST;
    return st.is_dir ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

static char *load_file(qstr filename, size_t *len_out)
{
    char real[256];
    dm_path(qstr_str(filename), real, sizeof real);
    const papp_if_fs_t *f = fs2();
    long sz = f->size(real);
    papp_file_t *fp = sz >= 0 ? f->open(real, "rb") : NULL;
    if (!fp) mp_raise_OSError(MP_ENOENT);
    char *buf = m_new(char, sz + 1);
    size_t n = f->read(fp, buf, (size_t)sz);
    f->close(fp);
    buf[n] = 0;
    *len_out = n;
    return buf;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    size_t len;
    char *buf = load_file(filename, &len);
    return mp_lexer_new_from_str_len(filename, buf, len, len + 1);
}

void mp_reader_new_file(mp_reader_t *reader, qstr filename)
{
    size_t len;
    char *buf = load_file(filename, &len);
    mp_reader_new_mem(reader, (const byte *)buf, len, len + 1);
}

/* â”€â”€ os â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

static mp_obj_t os_getcwd(void) { return mp_obj_new_str(s_cwd, strlen(s_cwd)); }
static MP_DEFINE_CONST_FUN_OBJ_0(os_getcwd_obj, os_getcwd);

static mp_obj_t os_chdir(mp_obj_t path_in)
{
    char a[256], real[256];
    py_abs(mp_obj_str_get_str(path_in), a, sizeof a);
    dm_path(a, real, sizeof real);
    papp_stat_t st;
    if (fs2()->hdr.version < 2 || papp_fs()->stat(real, &st) != 0 || !st.is_dir) mp_raise_OSError(MP_ENOENT);
    snprintf(s_cwd, sizeof s_cwd, "%s", a);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_chdir_obj, os_chdir);

typedef struct {
    mp_obj_base_t base;
    mp_fun_1_t iternext;
    mp_fun_1_t finaliser;
    papp_dir_t *d;
    bool names_only;
} dm_ilistdir_t;

static mp_obj_t ilistdir_next(mp_obj_t self_in)
{
    dm_ilistdir_t *it = MP_OBJ_TO_PTR(self_in);
    papp_dirent_t e;
    if (!it->d || papp_fs()->dir_read(it->d, &e) != 1) {
        if (it->d) { papp_fs()->dir_close(it->d); it->d = NULL; }
        return MP_OBJ_STOP_ITERATION;
    }
    mp_obj_t name = mp_obj_new_str(e.name, strlen(e.name));
    if (it->names_only) return name;
    mp_obj_t t[4] = { name, MP_OBJ_NEW_SMALL_INT(e.is_dir ? 0x4000 : 0x8000), MP_OBJ_NEW_SMALL_INT(0),
                      mp_obj_new_int_from_uint(e.size) };
    return mp_obj_new_tuple(4, t);
}

static mp_obj_t ilistdir_del(mp_obj_t self_in)
{
    dm_ilistdir_t *it = MP_OBJ_TO_PTR(self_in);
    if (it->d) { papp_fs()->dir_close(it->d); it->d = NULL; }
    return mp_const_none;
}

static mp_obj_t open_dir(size_t n_args, const mp_obj_t *args, bool names)
{
    char real[256];
    dm_path(n_args ? mp_obj_str_get_str(args[0]) : s_cwd, real, sizeof real);
    const papp_if_fs_t *f = fs2();
    if (f->hdr.version < 2) mp_raise_OSError(MP_EIO);
    if (!strcmp(real, DM_ROOT)) f->mkdir(DM_ROOT);              /* first use: create the root */
    papp_dir_t *d = f->dir_open(real);
    if (!d) mp_raise_OSError(MP_ENOENT);
    dm_ilistdir_t *it = mp_obj_malloc_with_finaliser(dm_ilistdir_t, &mp_type_polymorph_iter_with_finaliser);
    it->iternext = ilistdir_next;
    it->finaliser = ilistdir_del;
    it->d = d;
    it->names_only = names;
    return MP_OBJ_FROM_PTR(it);
}

static mp_obj_t os_ilistdir(size_t n_args, const mp_obj_t *args) { return open_dir(n_args, args, false); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(os_ilistdir_obj, 0, 1, os_ilistdir);

static mp_obj_t os_listdir(size_t n_args, const mp_obj_t *args)
{
    mp_obj_t it = open_dir(n_args, args, true), list = mp_obj_new_list(0, NULL), x;
    while ((x = mp_iternext(it)) != MP_OBJ_STOP_ITERATION) mp_obj_list_append(list, x);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(os_listdir_obj, 0, 1, os_listdir);

static mp_obj_t os_stat(mp_obj_t path_in)
{
    char real[256];
    real_path(path_in, real, sizeof real);
    const papp_if_fs_t *f = fs2();
    papp_stat_t st;
    if (f->hdr.version < 2 || f->stat(real, &st) != 0) mp_raise_OSError(MP_ENOENT);
    mp_obj_t t[10];
    t[0] = MP_OBJ_NEW_SMALL_INT(st.is_dir ? 0x4000 : 0x8000);
    for (int i = 1; i < 6; i++) t[i] = MP_OBJ_NEW_SMALL_INT(0);
    t[6] = mp_obj_new_int_from_uint(st.size);
    t[7] = t[8] = t[9] = mp_obj_new_int_from_ll(st.mtime);
    return mp_obj_new_tuple(10, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_stat_obj, os_stat);

static mp_obj_t os_statvfs(mp_obj_t path_in)
{
    (void)path_in;                       /* the card: 4 KB blocks, size not exposed to apps */
    mp_obj_t t[10] = {
        MP_OBJ_NEW_SMALL_INT(4096), MP_OBJ_NEW_SMALL_INT(4096), mp_obj_new_int(8 * 1024 * 1024),
        mp_obj_new_int(4 * 1024 * 1024), mp_obj_new_int(4 * 1024 * 1024),
        MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0),
        MP_OBJ_NEW_SMALL_INT(255),
    };
    return mp_obj_new_tuple(10, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_statvfs_obj, os_statvfs);

static mp_obj_t os_remove(mp_obj_t path_in)
{
    char real[256];
    real_path(path_in, real, sizeof real);
    if (fs2()->remove(real) != 0) mp_raise_OSError(MP_ENOENT);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_remove_obj, os_remove);

static mp_obj_t os_mkdir(mp_obj_t path_in)
{
    char real[256];
    real_path(path_in, real, sizeof real);
    if (fs2()->mkdir(real) != 0) mp_raise_OSError(MP_EEXIST);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_mkdir_obj, os_mkdir);

static mp_obj_t os_rmdir(mp_obj_t path_in)
{
    char real[256];
    real_path(path_in, real, sizeof real);
    const papp_if_fs_t *f = fs2();
    if (f->hdr.version < 2 || f->rmdir(real) != 0) mp_raise_OSError(MP_EACCES);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_rmdir_obj, os_rmdir);

static mp_obj_t os_rename(mp_obj_t a_in, mp_obj_t b_in)
{
    char a[256], b[256];
    real_path(a_in, a, sizeof a);
    real_path(b_in, b, sizeof b);
    const papp_if_fs_t *f = fs2();
    if (f->hdr.version < 2) mp_raise_OSError(MP_EIO);
    f->remove(b);                        /* like MicroPython on a board: replace the target */
    if (f->rename(a, b) != 0) mp_raise_OSError(MP_ENOENT);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(os_rename_obj, os_rename);

static mp_obj_t os_sync(void) { return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(os_sync_obj, os_sync);

static mp_obj_t os_urandom(mp_obj_t n_in)
{
    mp_int_t n = mp_obj_get_int(n_in);
    vstr_t v;
    vstr_init_len(&v, n);
    uint32_t x = (uint32_t)papp_time_us() * 2654435761u;
    for (mp_int_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; v.buf[i] = (char)x; }
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_urandom_obj, os_urandom);

static const MP_DEFINE_STR_OBJ(os_sysname, "esp32");
static const MP_DEFINE_STR_OBJ(os_nodename, "esp32p4");
static const MP_DEFINE_STR_OBJ(os_release, MICROPY_VERSION_STRING);
static const MP_DEFINE_STR_OBJ(os_version, MICROPY_VERSION_STRING " (Abed Mihilia P4 Launcher)");
static const MP_DEFINE_STR_OBJ(os_machine, MICROPY_BANNER_MACHINE);
static const mp_rom_obj_tuple_t os_uname_info = {
    { &mp_type_tuple }, 5,
    { MP_ROM_PTR(&os_sysname), MP_ROM_PTR(&os_nodename), MP_ROM_PTR(&os_release),
      MP_ROM_PTR(&os_version), MP_ROM_PTR(&os_machine) },
};
static mp_obj_t os_uname(void) { return MP_OBJ_FROM_PTR(&os_uname_info); }
static MP_DEFINE_CONST_FUN_OBJ_0(os_uname_obj, os_uname);

static const mp_rom_map_elem_t os_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&os_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&os_ilistdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_statvfs), MP_ROM_PTR(&os_statvfs_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_unlink), MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&os_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&os_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_sync), MP_ROM_PTR(&os_sync_obj) },
    { MP_ROM_QSTR(MP_QSTR_urandom), MP_ROM_PTR(&os_urandom_obj) },
    { MP_ROM_QSTR(MP_QSTR_uname), MP_ROM_PTR(&os_uname_obj) },
    { MP_ROM_QSTR(MP_QSTR_sep), MP_ROM_QSTR(MP_QSTR__slash_) },
};
static MP_DEFINE_CONST_DICT(os_globals, os_globals_table);

const mp_obj_module_t dm_module_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&os_globals,
};
MP_REGISTER_MODULE(MP_QSTR_os, dm_module_os);
