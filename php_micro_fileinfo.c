/*
micro SAPI for PHP - php_micro_filesize.c
filesize reproduction utilities for micro

Copyright 2020 Longyan
Copyright 2022 Yun Dou <dixyes@gmail.com>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "php.h"

#include "php_micro.h"
#include "php_micro_helper.h"

#include <stdint.h>
#if defined(PHP_WIN32)
#    include "win32/codepage.h"
#    include <windows.h>
#elif defined(__linux) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
// all the things linux used should also work for other unix-like using ELF,
// but not well tested
#    include <elf.h>
#    include <sys/auxv.h>
#    if defined(__LP64__)
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Shdr Elf_Shdr;
typedef Elf64_Phdr Elf_Phdr;
#        define ELFCLASS ELFCLASS64
#    else
typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Phdr Elf_Phdr;
#        define ELFCLASS ELFCLASS32
#    endif
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <mach-o/getsect.h>
#    if defined(__LP64__)
typedef struct mach_header_64 mach_header;
typedef struct segment_command_64 segment_command;
#        define MACH_HEADER_MAGIC   MH_MAGIC_64
#        define LOADCOMMAND_SEGMENT LC_SEGMENT_64
#    else
typedef struct mach_header mach_header;
typedef struct segment_command segment_command;
#        define LOADCOMMAND_SEGMENT LC_SEGMENT
#    endif
#else
#    error because we donot support that platform yet
#endif

#ifndef PHP_WIN32
#    include <errno.h>
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif // ndef PHP_WIN32

const char *micro_get_filename(void);

// do we need uint64_t for sfx size?
static uint32_t _final_sfxsize = 0;
uint32_t _micro_get_sfxsize(void);
uint32_t micro_get_sfxsize(void) {
    return _final_sfxsize;
}

typedef struct _ext_ini_header_t {
    uint8_t magic[4];
    uint8_t len[4];
} ext_ini_header_t;

struct _ext_ini {
    size_t size;
    char *data;
} micro_ext_ini = {.size = 0, .data = NULL};

// shabby endian-independent check
#define checkmagic(var) \
    (var[0] != PHP_MICRO_INIMARK[0] || var[1] != PHP_MICRO_INIMARK[1] || var[2] != PHP_MICRO_INIMARK[2] || \
        var[3] != PHP_MICRO_INIMARK[3])

#ifdef PHP_WIN32
const wchar_t *micro_get_filename_w();
#endif // PHP_WIN32

int micro_fileinfo_init(void) {
    int ret = 0;
    uint32_t len = 0;
    uint32_t sfxsize = _micro_get_sfxsize();
    if (0 == sfxsize) {
        return FAILURE;
    }
#ifdef PHP_WIN32
    LPCWSTR self_path = micro_get_filename_w();
    HANDLE handle = CreateFileW(self_path,
        FILE_ATTRIBUTE_READONLY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    if (INVALID_HANDLE_VALUE == handle) {
        ret = FAILURE;
        goto end;
    }
    DWORD filesize = GetFileSize(handle, NULL);
    dbgprintf("%d, %d\n", sfxsize, filesize);
    if (filesize <= sfxsize) {
        fwprintf(stderr, L"no payload found.\n" PHP_MICRO_HINT, self_path);
        ret = FAILURE;
        goto end;
    }
#    define seekfile(x) \
        do { \
            SetFilePointer(handle, x, 0, FILE_BEGIN); \
        } while (0)
#    define readfile(dest, size, red) \
        do { \
            ReadFile(handle, dest, size, &red, NULL); \
        } while (0)
#    define closefile() \
        do { \
            if (INVALID_HANDLE_VALUE != handle) { \
                CloseHandle(handle); \
            } \
        } while (0)
#else
    const char *self_path = micro_get_filename();
    int fd = open(self_path, O_RDONLY);
    if (-1 == fd) {
        // TODO: tell failed here
        ret = errno;
        goto end;
    }
    struct stat stats;
    ret = stat(self_path, &stats);
    if (-1 == ret) {
        // TODO: tell failed here
        ret = errno;
        goto end;
    }
    size_t filesize = stats.st_size;
    dbgprintf("%d, %ld\n", sfxsize, filesize);
    if (filesize <= sfxsize) {
        fprintf(stderr, "no payload found.\n" PHP_MICRO_HINT, self_path);
        ret = FAILURE;
        goto end;
    }
#    define seekfile(x) \
        do { \
            lseek(fd, x, SEEK_SET); \
        } while (0)
#    define readfile(dest, size, red) \
        do { \
            red = read(fd, dest, size); \
        } while (0)
#    define closefile() \
        do { \
            if (-1 != fd) { \
                close(fd); \
            } \
        } while (0)
#endif // PHP_WIN32
    ext_ini_header_t ext_ini_header = {0};
    if (filesize <= sfxsize + sizeof(ext_ini_header)) {
        ret = FAILURE;
        goto end;
    }
    // we may have extra ini configs.
    seekfile(sfxsize);
    uint32_t red = 0;
    readfile(&ext_ini_header, sizeof(ext_ini_header), red);
    if (sizeof(ext_ini_header) != red) {
        // cannot read file
        ret = errno;
        goto end;
    }

    if (checkmagic(ext_ini_header.magic)) {
        // bad magic, not an extra ini
        ret = SUCCESS;
        goto end;
    }
    // shabby ntohl
    len = (ext_ini_header.len[0] << 24) + (ext_ini_header.len[1] << 16) + (ext_ini_header.len[2] << 8) +
        ext_ini_header.len[3];
    dbgprintf("len is %d\n", len);
    if (filesize <= sfxsize + sizeof(ext_ini_header) + len) {
        // bad len, not an extra ini
        ret = SUCCESS;
        len = 0;
        goto end;
    }
    micro_ext_ini.data = malloc(len + 2);
    readfile(micro_ext_ini.data, len, red);
    if (len != red) {
        // cannot read file
        ret = errno;
        len = 0;
        free(micro_ext_ini.data);
        micro_ext_ini.data = NULL;
        goto end;
    }
    // two '\0's like hardcoden inis
    micro_ext_ini.data[len] = '\0';
    micro_ext_ini.data[len + 1] = '\0';
    dbgprintf("using ext ini %s\n", micro_ext_ini.data);
    micro_ext_ini.size = len + 1;
    len += sizeof(ext_ini_header_t);

end:
    _final_sfxsize = sfxsize + len;
    closefile();
    return ret;
#undef seekfile
#undef readfile
#undef closefile
}

/*
 *   _micro_get_sfxsize - get (real) sfx size using
 *   Win32 resource (win) / __TEXT,__info_plist (Mach-O) / .microinf (ELF)
 */
uint32_t _micro_get_sfxsize(void) {
    static uint32_t _sfxsize = 0;
    if (0 != _sfxsize) {
        return _sfxsize;
    }
#if defined(PHP_WIN32)
    // get resource
    HRSRC resource = FindResourceA(NULL, MAKEINTRESOURCEA(PHP_MICRO_SFXSIZE_ID), RT_RCDATA);
    dbgprintf("resource: %p\n", resource);
    if (NULL == resource) {
        dbgprintf("GetLastError: %d\n", GetLastError());
        fprintf(stderr, "no sfx resource found (corrupt micro sfx).\n");
        return 0;
    }
    memcpy((void *)&_sfxsize, LockResource(LoadResource(NULL, HRSRC)), sizeof(uint32_t));
    return _sfxsize;
#elif defined(__APPLE__)
    // get mach header
    mach_header *header = (mach_header *)_dyld_get_image_header(0);
    const segment_command *linkedit = NULL;
    unsigned long size = 0;

    const char *section = (const char *)getsectiondata(header, "__DATA", "__micro_sfxsize", &size);
    if (NULL == section) {

        linkedit = getsegbyname("__LINKEDIT");
        if (NULL == linkedit) {
            fprintf(stderr, "no __LINKEDIT segment found (corrupt micro sfx).\n");
            return 0;
        }
        _sfxsize = (linkedit->fileoff + linkedit->filesize) % 0xffffffff;
        dbgprintf("no __DATA,__micro_sfxsize section found, use end of __LINKEDIT: %d\n", _sfxsize);
        return _sfxsize;
    }
    if (sizeof(uint32_t) > size) {
        fprintf(stderr, "bad __DATA,__micro_sfxsize section found (corrupt micro sfx).\n");
        return 0;
    }
    // read as big endian
    uint8_t sfxsize_bytes[4];
    memcpy((void *)&sfxsize_bytes, section, 4);
    _sfxsize = ((uint32_t)sfxsize_bytes[0] << 24) + ((uint32_t)sfxsize_bytes[1] << 16) +
        ((uint32_t)sfxsize_bytes[2] << 8) + (uint32_t)sfxsize_bytes[3];
    return _sfxsize;
#elif defined(__linux) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    // get section header
    Elf_Ehdr ehdr;
    Elf_Shdr *shdrs = NULL, *shdr, *strtabhdr;
    Elf_Phdr *phdrs = NULL;
    int i;
    const char *errMsg = "";
    char *strtab = NULL;
    const char *self_path = micro_get_filename();

    int fd = open(self_path, O_RDONLY);
    if (fd < 0) {
        errMsg = "cannot open self file";
        goto error;
    }

    if (sizeof(Elf_Ehdr) != read(fd, &ehdr, sizeof(Elf_Ehdr))) {
        errMsg = "cannot read elf header";
        goto error;
    }

    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_ident[EI_CLASS] != ELFCLASS ||
        sizeof(Elf_Ehdr) != ehdr.e_ehsize || (ehdr.e_shentsize > 0 && sizeof(Elf_Shdr) != ehdr.e_shentsize) ||
        (ehdr.e_phentsize > 0 && sizeof(Elf_Phdr) != ehdr.e_phentsize)) {
        errMsg = "bad elf header (corrupt micro sfx)";
        goto error;
    }

    // read section headers
    if (ehdr.e_shnum > 0) {
        dbgprintf("section headers: %d, offset %016lx\n", ehdr.e_shnum, (uint64_t)ehdr.e_shoff);
        _sfxsize = _sfxsize > ehdr.e_shoff + ehdr.e_shnum * sizeof(Elf_Shdr)
            ? _sfxsize
            : ehdr.e_shoff + ehdr.e_shnum * sizeof(Elf_Shdr);
        dbgprintf("%d: sfxsize: %d\n", __LINE__, _sfxsize);

        shdrs = malloc(sizeof(Elf_Shdr) * ehdr.e_shnum);
        if (NULL == shdrs) {
            // impossible
            errMsg = "cannot allocate memory for section header";
            goto error;
        }

        if (lseek(fd, ehdr.e_shoff, SEEK_SET) != ehdr.e_shoff) {
            errMsg = "cannot seek to section header (corrupt micro sfx)";
            goto error;
        }
        if (read(fd, shdrs, sizeof(Elf_Shdr) * ehdr.e_shnum) != sizeof(Elf_Shdr) * ehdr.e_shnum) {
            errMsg = "cannot read section headers";
            goto error;
        }

        // read strtab
        strtabhdr = &shdrs[ehdr.e_shstrndx];
        strtab = malloc(strtabhdr->sh_size + 1);
        if (NULL == strtab) {
            // impossible
            errMsg = "cannot allocate memory for section header string table";
            goto error;
        }
        if (lseek(fd, strtabhdr->sh_offset, SEEK_SET) != strtabhdr->sh_offset) {
            errMsg = "cannot seek to section header string table";
            goto error;
        }
        if (read(fd, strtab, strtabhdr->sh_size) != strtabhdr->sh_size) {
            errMsg = "cannot read section header string table";
            goto error;
        }
        strtab[strtabhdr->sh_size] = '\0';

        for (i = 0; i < ehdr.e_shnum; i++) {
            shdr = &shdrs[i];
            if (shdr->sh_type == SHT_NOBITS) {
                // if no data, skip it
                continue;
            }
            _sfxsize = _sfxsize > shdr->sh_offset + shdr->sh_size ? _sfxsize : shdr->sh_offset + shdr->sh_size;
            dbgprintf("%d: sfxsize: %d\n", __LINE__, _sfxsize);
            if (shdr->sh_name > strtabhdr->sh_size) {
                errMsg = "bad section header name (corrupt micro sfx)";
                goto error;
            }
            dbgprintf("%016lx %016lx %s type %08x\n",
                (uint64_t)shdr->sh_offset,
                (uint64_t)shdr->sh_size,
                &strtab[shdr->sh_name],
                shdr->sh_type);
            dbgprintf("%016lx %08x\n", shdr->sh_flags, shdr->sh_info);
            if (shdr->sh_type != 0x1 /* application spec */ || shdr->sh_name > SHN_LORESERVE ||
                strncmp(&strtab[shdr->sh_name], ".sfxsize", 8)) {
                continue;
            }
            if (shdr->sh_size < sizeof(uint32_t)) {
                errMsg = "bad .sfxsize section header (corrupt micro sfx)";
                goto error;
            }
            if (lseek(fd, shdr->sh_offset, SEEK_SET) != shdr->sh_offset) {
                errMsg = "cannot seek to .sfxsize section header (corrupt micro sfx)";
                goto error;
            }
            // read as big endian
            uint8_t sfxsize_bytes[4];
            if (read(fd, &sfxsize_bytes, sizeof(uint32_t)) != sizeof(uint32_t)) {
                errMsg = "cannot read .sfxsize section header (corrupt micro sfx)";
                goto error;
            }
            _sfxsize = ((uint32_t)sfxsize_bytes[0] << 24) + ((uint32_t)sfxsize_bytes[1] << 16) +
                ((uint32_t)sfxsize_bytes[2] << 8) + (uint32_t)sfxsize_bytes[3];
            // at this time, things must be allocated
            dbgprintf("using .sfxsize section: %d\n", _sfxsize);
            close(fd);
            free(shdrs);
            free(strtab);
            return _sfxsize;
        }
    }

    // try to get elf size
    // _sfxsize is now max section size
    _sfxsize = _sfxsize > ehdr.e_phoff + ehdr.e_phnum * sizeof(Elf_Phdr)
        ? _sfxsize
        : ehdr.e_phoff + ehdr.e_phnum * sizeof(Elf_Phdr);
    dbgprintf("%d: sfxsize: %d\n", __LINE__, _sfxsize);

    phdrs = malloc(sizeof(Elf_Phdr) * ehdr.e_phnum);
    if (NULL == phdrs) {
        // impossible
        errMsg = "cannot allocate memory for program header";
        goto error;
    }

    if (lseek(fd, ehdr.e_phoff, SEEK_SET) != ehdr.e_phoff) {
        errMsg = "cannot seek to program header";
        goto error;
    }
    if (read(fd, phdrs, sizeof(Elf_Phdr) * ehdr.e_phnum) != sizeof(Elf_Phdr) * ehdr.e_phnum) {
        errMsg = "cannot read program header";
        goto error;
    }

    for (i = 0; i < ehdr.e_phnum; i++) {
        _sfxsize = phdrs[i].p_offset + phdrs[i].p_filesz > _sfxsize ? phdrs[i].p_offset + phdrs[i].p_filesz : _sfxsize;
    }

    return _sfxsize;
error:
    fprintf(stderr, "failed get sfxsize: %s\n", errMsg);
    if (shdrs) {
        free(shdrs);
    }
    if (strtab) {
        free(strtab);
    }
    if (phdrs) {
        free(phdrs);
    }
    if (fd >= 0) {
        close(fd);
    }
    return _sfxsize;
#endif
}

#ifdef PHP_WIN32

const wchar_t *micro_get_filename_w() {
    static LPWSTR self_filename = NULL;
    // dbgprintf("fuck %S\n", self_filename);
    if (self_filename) {
        return self_filename;
    }
    DWORD self_filename_chars = MAX_PATH;
    self_filename = malloc(self_filename_chars * sizeof(WCHAR));
    DWORD wapiret = 0;

    DWORD (*myGetModuleFileNameExW)(HANDLE, HMODULE, LPWSTR, DWORD);

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    myGetModuleFileNameExW = (void *)GetProcAddress(hKernel32, "K32GetModuleFileNameExW");
    if (NULL == myGetModuleFileNameExW) {
        HMODULE hPsapi = GetModuleHandleW(L"psapi.dll");
        myGetModuleFileNameExW = (void *)GetProcAddress(hPsapi, "GetModuleFileNameExW");
    }
    if (NULL == myGetModuleFileNameExW) {
        dbgprintf("cannot get self path via win32api\n");
        return NULL;
    }
    while (self_filename_chars ==
        (wapiret = myGetModuleFileNameExW(GetCurrentProcess(), NULL, self_filename, self_filename_chars))) {
        dbgprintf("wapiret is %d\n", wapiret);
        dbgprintf("lensize is %d\n", self_filename_chars);
        if (
#    if WINVER < _WIN32_WINNT_VISTA
            ERROR_SUCCESS
#    else
            ERROR_INSUFFICIENT_BUFFER
#    endif
            == GetLastError()) {
            self_filename_chars += MAX_PATH;
            self_filename = realloc(self_filename, self_filename_chars * sizeof(WCHAR));
        } else {
            dbgprintf("cannot get self path\n");
            return NULL;
        }
    };
    dbgprintf("wapiret is %d\n", wapiret);
    dbgprintf("lensize is %d\n", self_filename_chars);

    if (wapiret > MAX_PATH && memcmp(L"\\\\?\\", self_filename, 4 * sizeof(WCHAR))) {
        dbgprintf("\\\\?\\-ize self_filename\n");
        LPWSTR buf = malloc((wapiret + 5) * sizeof(WCHAR));
        memcpy(buf, L"\\\\?\\", 4 * sizeof(WCHAR));
        memcpy(buf + 4, self_filename, wapiret * sizeof(WCHAR));
        buf[wapiret + 4] = L'\0';
        free(self_filename);
        self_filename = buf;
    }

    dbgprintf("self is %S\n", self_filename);

    return self_filename;
}

const char *micro_get_filename(void) {
    return php_win32_cp_w_to_utf8(micro_get_filename_w());
}

#elif defined(__linux) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
const char *micro_get_filename(void) {
    static char *self_filename = NULL;
    if (NULL == self_filename) {
        self_filename = malloc(PATH_MAX);
        (void)realpath((const char *)getauxval(AT_EXECFN), self_filename);
    }
    return self_filename;
}
#elif defined(__APPLE__)
const char *micro_get_filename(void) {
    static char *self_path = NULL;
    if (NULL == self_path) {
        uint32_t len = 0;
        if (-1 != _NSGetExecutablePath(NULL, &len)) {
            goto error;
        }
        self_path = malloc(len);
        if (NULL == self_path) {
            goto error;
        }
        if (0 != _NSGetExecutablePath(self_path, &len)) {
            goto error;
        }

        // realpath it
        char *real_path = malloc(PATH_MAX);
        if (NULL == real_path) {
            goto error;
        }
        if (NULL == realpath(self_path, real_path)) {
            goto error;
        }
        free(self_path);
        self_path = real_path;
    }
    return self_path;
error:
    if (NULL != self_path) {
        self_path[0] = '\0';
    }
    return NULL;
}
#else
#    error "not support this system yet"
#endif

size_t micro_get_filename_len(void) {
    static size_t _micro_filename_l = -1;
    if (-1 == _micro_filename_l) {
        _micro_filename_l = strlen(micro_get_filename());
    }
    return _micro_filename_l;
}

// deprecated
#if MICRO_USE_OLD_PHAR_HOOK
int is_stream_self(php_stream *stream) {
    dbgprintf("checking %s\n", stream->orig_path);
#    ifdef PHP_WIN32
    LPCWSTR stream_path_w = php_win32_ioutil_any_to_w(stream->orig_path);
    size_t stream_path_w_len = wcslen(stream_path_w);
    LPCWSTR my_path_w = micro_get_filename_w();
    size_t my_path_w_len = wcslen(my_path_w);
    dbgprintf("with self: %S\n", my_path_w);
    if (my_path_w_len == stream_path_w_len && 0 == wcscmp(stream_path_w, my_path_w)) {
#    else
    const char *stream_path = stream->orig_path;
    size_t stream_path_len = strlen(stream_path);
    const char *my_path = micro_get_filename();
    size_t my_path_len = strlen(my_path);
    dbgprintf("with self: %s\n", my_path);
    if (my_path_len == stream_path_len && 0 == strcmp(stream_path, my_path)) {
#    endif
        dbgprintf("is self\n");
        return 1;
    }
    dbgprintf("not self\n");
    return 0;
}
#endif

PHP_FUNCTION(micro_get_self_filename) {
    RETURN_STRING(micro_get_filename());
}

PHP_FUNCTION(micro_get_sfx_filesize) {
    zend_error(E_DEPRECATED, "micro_get_sfx_filesize is deprecated, use micro_get_sfxsize instead");
    RETURN_LONG(micro_get_sfxsize());
}

PHP_FUNCTION(micro_get_sfxsize) {
    RETURN_LONG(micro_get_sfxsize());
}
