// Converts the data objects that GNU as makes for the 64-bit PC port (x86-64
// ELF) into Mach-O objects, so they can be linked on macOS.
//
// The game's data is written in GNU assembler syntax, with macros that
// clang's assembler can't evaluate, so on macOS it's still assembled by GNU as
// (x86_64-elf-as) and converted here. The objects only hold data: sections of
// bytes, symbols, and 64-bit absolute pointers to symbols. Symbols get the
// leading underscore that C symbols have on macOS.
//
// usage: elf2macho [--arch arm64|x86_64] [--min-os VERSION] INPUT [OUTPUT]

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHN_UNDEF 0
#define SHN_ABS 0xFFF1
#define SHN_COMMON 0xFFF2
#define STB_LOCAL 0
#define STB_WEAK 2
#define STT_SECTION 3
#define STT_FILE 4
#define EM_X86_64 62
#define R_X86_64_NONE 0
#define R_X86_64_64 1

#define MH_MAGIC_64 0xFEEDFACF
#define MH_OBJECT 1
#define CPU_TYPE_X86_64 0x01000007
#define CPU_SUBTYPE_X86_64_ALL 3
#define CPU_TYPE_ARM64 0x0100000C
#define CPU_SUBTYPE_ARM64_ALL 0
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xB
#define LC_SEGMENT_64 0x19
#define LC_BUILD_VERSION 0x32
#define PLATFORM_MACOS 1
#define S_REGULAR 0
#define S_ZEROFILL 1
#define N_UNDF 0x0
#define N_EXT 0x1
#define N_ABS 0x2
#define N_SECT 0xE
#define N_WEAK_REF 0x40
#define N_WEAK_DEF 0x80
#define RELOC_UNSIGNED 0  // the same for arm64 and x86-64

#define MAX_SECTIONS 16

struct ElfSection {
    const char *name;
    uint32_t type;
    uint64_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t align;
    uint64_t entsize;
    int hasSymbols;
    // Where it goes in the Mach-O object, if anywhere
    int machoSection;  // 0 if it's left out
    uint64_t machoOffset;
};

struct MachoSection {
    char name[17];
    int zerofill;
    uint32_t alignLog2;
    uint64_t size;
    uint64_t addr;
    uint32_t fileOffset;
    uint8_t *data;
    uint32_t *relocs;  // pairs of words
    uint32_t relocCount;
    uint32_t relocCapacity;
    uint32_t relocOffset;
    int startSymbol;  // index of its local start symbol, or -1
};

struct Symbol {
    char *name;
    uint8_t type;
    uint8_t sect;
    uint16_t desc;
    uint64_t value;
    int group;  // 0 local, 1 defined external, 2 undefined
    uint32_t index;  // in the Mach-O symbol table
};

static const char *sProgram = "elf2macho";
static const char *sInput;

static void Fail(const char *message, ...) __attribute__((noreturn, format(printf, 1, 2)));

static void Fail(const char *message, ...)
{
    va_list args;
    fprintf(stderr, "%s: %s: ", sProgram, sInput);
    va_start(args, message);
    vfprintf(stderr, message, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}

static uint16_t Read16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t Read32(const uint8_t *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }
static uint64_t Read64(const uint8_t *p) { return Read32(p) | ((uint64_t)Read32(p + 4) << 32); }

static void Write32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = v >> (8 * i);
}

static void Write64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = v >> (8 * i);
}

static uint64_t AlignUp(uint64_t value, uint64_t align)
{
    return align > 1 ? (value + align - 1) / align * align : value;
}

static uint32_t Log2(uint64_t value)
{
    uint32_t log = 0;
    while (((uint64_t)1 << log) < value)
        log++;
    return log;
}

static uint8_t *ReadFile(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;

    if (f == NULL)
        Fail("can't open it");
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(*size ? *size : 1);
    if (data == NULL || fread(data, 1, *size, f) != *size)
        Fail("can't read it");
    fclose(f);
    return data;
}

// Mach-O sections are named after the ELF ones: read-only data that holds
// pointers goes in __DATA,__const, like clang puts it
static void MachoSectionName(const char *elfName, uint32_t type, char *name)
{
    if (type == SHT_NOBITS)
        strcpy(name, "__bss");
    else if (strncmp(elfName, ".rodata", 7) == 0 || strncmp(elfName, ".data.rel.ro", 12) == 0)
        strcpy(name, "__const");
    else if (strncmp(elfName, ".data", 5) == 0)
        strcpy(name, "__data");
    else
    {
        // Like script_data: __script_data
        snprintf(name, 17, "__%s", elfName[0] == '.' ? elfName + 1 : elfName);
        for (char *c = name; *c; c++)
            if (*c == '.')
                *c = '_';
    }
}

static void AddReloc(struct MachoSection *section, uint32_t address, uint32_t symbol)
{
    if (section->relocCount == section->relocCapacity)
    {
        section->relocCapacity = section->relocCapacity ? section->relocCapacity * 2 : 64;
        section->relocs = realloc(section->relocs, section->relocCapacity * 8);
        if (section->relocs == NULL)
            Fail("out of memory");
    }
    section->relocs[section->relocCount * 2] = address;
    // r_symbolnum:24, r_pcrel:1, r_length:2 (8 bytes), r_extern:1, r_type:4
    section->relocs[section->relocCount * 2 + 1] = symbol | (3u << 25) | (1u << 27) | (RELOC_UNSIGNED << 28);
    section->relocCount++;
}

static int CompareSymbols(const void *a, const void *b)
{
    const struct Symbol *x = *(const struct Symbol *const *)a;
    const struct Symbol *y = *(const struct Symbol *const *)b;

    if (x->group != y->group)
        return x->group - y->group;
    // Locals keep their order; externals are sorted by name, like the linker expects
    if (x->group == 0)
        return x < y ? -1 : x > y;
    return strcmp(x->name, y->name);
}

int main(int argc, char **argv)
{
    const char *output = NULL;
    uint32_t cpuType = CPU_TYPE_ARM64, cpuSubtype = CPU_SUBTYPE_ARM64_ALL;
    uint32_t minOs = 11 << 16;
    size_t size;
    uint8_t *elf;
    uint16_t sectionCount, shstrndx;
    uint64_t sectionHeaders;
    struct ElfSection *sections;
    struct MachoSection macho[MAX_SECTIONS];
    int machoCount = 0;
    const struct ElfSection *symtab = NULL;
    const char *strtab;
    uint32_t elfSymbolCount;
    struct Symbol *symbols;
    int symbolCount = 0;
    int *elfToSymbol;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--arch") == 0 && i + 1 < argc)
        {
            i++;
            if (strcmp(argv[i], "arm64") == 0)
            {
                cpuType = CPU_TYPE_ARM64;
                cpuSubtype = CPU_SUBTYPE_ARM64_ALL;
            }
            else if (strcmp(argv[i], "x86_64") == 0)
            {
                cpuType = CPU_TYPE_X86_64;
                cpuSubtype = CPU_SUBTYPE_X86_64_ALL;
            }
            else
            {
                fprintf(stderr, "%s: unknown architecture %s\n", sProgram, argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--min-os") == 0 && i + 1 < argc)
        {
            unsigned major = 0, minor = 0, patch = 0;
            if (sscanf(argv[++i], "%u.%u.%u", &major, &minor, &patch) < 1)
            {
                fprintf(stderr, "%s: bad version %s\n", sProgram, argv[i]);
                return 1;
            }
            minOs = (major << 16) | (minor << 8) | patch;
        }
        else if (sInput == NULL)
            sInput = argv[i];
        else if (output == NULL)
            output = argv[i];
        else
            sInput = NULL, i = argc;
    }
    if (sInput == NULL)
    {
        fprintf(stderr, "usage: %s [--arch arm64|x86_64] [--min-os VERSION] INPUT [OUTPUT]\n", sProgram);
        return 1;
    }
    if (output == NULL)
        output = sInput;

    elf = ReadFile(sInput, &size);
    if (size < 64 || memcmp(elf, "\x7F" "ELF", 4) != 0 || elf[4] != 2 || elf[5] != 1)
        Fail("not a 64-bit little-endian ELF file");
    if (Read16(elf + 16) != 1 || Read16(elf + 18) != EM_X86_64)
        Fail("not an x86-64 ELF object");
    sectionHeaders = Read64(elf + 40);
    sectionCount = Read16(elf + 60);
    shstrndx = Read16(elf + 62);
    if (sectionHeaders + (uint64_t)sectionCount * 64 > size || shstrndx >= sectionCount)
        Fail("bad section headers");

    sections = calloc(sectionCount, sizeof(*sections));
    for (int i = 0; i < sectionCount; i++)
    {
        const uint8_t *h = elf + sectionHeaders + i * 64;
        struct ElfSection *s = &sections[i];

        s->type = Read32(h + 4);
        s->flags = Read64(h + 8);
        s->offset = Read64(h + 24);
        s->size = Read64(h + 32);
        s->link = Read32(h + 40);
        s->info = Read32(h + 44);
        s->align = Read64(h + 48);
        s->entsize = Read64(h + 56);
        if (s->type != SHT_NOBITS && s->offset + s->size > size)
            Fail("section %d is past the end of the file", i);
    }
    for (int i = 0; i < sectionCount; i++)
    {
        uint32_t name = Read32(elf + sectionHeaders + i * 64);

        if (name >= sections[shstrndx].size)
            Fail("bad name for section %d", i);
        sections[i].name = (const char *)elf + sections[shstrndx].offset + name;
    }

    for (int i = 0; i < sectionCount; i++)
        if (sections[i].type == SHT_SYMTAB)
            symtab = &sections[i];
    if (symtab == NULL || symtab->link >= sectionCount)
        Fail("no symbol table");
    for (uint64_t i = 1; i < symtab->size / 24; i++)
    {
        uint16_t shndx = Read16(elf + symtab->offset + i * 24 + 6);
        if (shndx < sectionCount && (elf[symtab->offset + i * 24 + 4] & 0xF) != STT_SECTION)
            sections[shndx].hasSymbols = 1;
    }

    // Symbols go in their sections. Sections only the linker would use, like
    // .note.gnu.property, are left out, and so are empty ones.
    for (int i = 0; i < sectionCount; i++)
    {
        struct ElfSection *s = &sections[i];
        char name[17];
        int m;

        if (!(s->flags & SHF_ALLOC) || s->type == SHT_NOTE || (s->size == 0 && !s->hasSymbols))
            continue;
        if (s->flags & SHF_EXECINSTR)
        {
            if (s->size != 0)
                Fail("section %s has code, but only data can be converted", s->name);
            continue;
        }
        MachoSectionName(s->name, s->type, name);
        for (m = 0; m < machoCount; m++)
            if (strcmp(macho[m].name, name) == 0)
                break;
        if (m == machoCount)
        {
            if (machoCount == MAX_SECTIONS)
                Fail("too many sections");
            memset(&macho[m], 0, sizeof(macho[m]));
            strcpy(macho[m].name, name);
            macho[m].zerofill = s->type == SHT_NOBITS;
            macho[m].startSymbol = -1;
            machoCount++;
        }
        if (macho[m].zerofill != (s->type == SHT_NOBITS))
            Fail("section %s mixes data and zeros", s->name);
        s->machoSection = m + 1;
        s->machoOffset = AlignUp(macho[m].size, s->align);
        macho[m].size = s->machoOffset + s->size;
        if (Log2(s->align) > macho[m].alignLog2)
            macho[m].alignLog2 = Log2(s->align);
    }
    for (int m = 0; m < machoCount; m++)
        if (!macho[m].zerofill && (macho[m].data = calloc(macho[m].size ? macho[m].size : 1, 1)) == NULL)
            Fail("out of memory");
    for (int i = 0; i < sectionCount; i++)
        if (sections[i].machoSection && sections[i].type != SHT_NOBITS)
            memcpy(macho[sections[i].machoSection - 1].data + sections[i].machoOffset, elf + sections[i].offset,
                   sections[i].size);

    // Addresses in the object start at 0, section after section
    {
        uint64_t addr = 0;
        for (int m = 0; m < machoCount; m++)
        {
            addr = AlignUp(addr, (uint64_t)1 << macho[m].alignLog2);
            macho[m].addr = addr;
            addr += macho[m].size;
        }
    }

    strtab = (const char *)elf + sections[symtab->link].offset;
    elfSymbolCount = symtab->size / 24;
    // Every ELF symbol, plus a start symbol for each section
    symbols = calloc(elfSymbolCount + machoCount, sizeof(*symbols));
    elfToSymbol = malloc(elfSymbolCount * sizeof(*elfToSymbol));
    for (uint32_t i = 0; i < elfSymbolCount; i++)
    {
        const uint8_t *e = elf + symtab->offset + i * 24;
        const char *name;

        if (Read32(e) >= sections[symtab->link].size)
            Fail("bad name for symbol %u", i);
        name = strtab + Read32(e);
        uint8_t bind = e[4] >> 4, type = e[4] & 0xF;
        uint16_t shndx = Read16(e + 6);
        uint64_t value = Read64(e + 8);
        struct Symbol *sym = &symbols[symbolCount];

        elfToSymbol[i] = -1;
        if (i == 0 || type == STT_SECTION || type == STT_FILE)
            continue;
        if (shndx == SHN_COMMON)
            Fail("common symbol %s", name);
        if (shndx == SHN_ABS && bind == STB_LOCAL)
            continue;  // assembler constants, which relocations don't need
        sym->name = malloc(strlen(name) + 2);
        snprintf(sym->name, strlen(name) + 2, "_%s", name);
        if (shndx == SHN_UNDEF)
        {
            sym->type = N_UNDF | N_EXT;
            sym->desc = bind == STB_WEAK ? N_WEAK_REF : 0;
            sym->group = 2;
        }
        else
        {
            if (shndx == SHN_ABS)
            {
                sym->type = N_ABS;
                sym->value = value;
            }
            else
            {
                if (shndx >= sectionCount || !sections[shndx].machoSection)
                    Fail("symbol %s is in section %d, which isn't converted", name, shndx);
                sym->type = N_SECT;
                sym->sect = sections[shndx].machoSection;
                sym->value = macho[sym->sect - 1].addr + sections[shndx].machoOffset + value;
            }
            if (bind != STB_LOCAL)
            {
                sym->type |= N_EXT;
                sym->desc = bind == STB_WEAK ? N_WEAK_DEF : 0;
                sym->group = 1;
            }
        }
        elfToSymbol[i] = symbolCount++;
    }

    // The pointers. On arm64, the linker wants every relocation to name a
    // symbol, so pointers into a section without one go through a local
    // symbol at the start of the section, like clang's ltmp symbols.
    for (int i = 0; i < sectionCount; i++)
    {
        const struct ElfSection *rela = &sections[i];
        struct MachoSection *target;
        const struct ElfSection *targetElf;

        if (rela->type != SHT_RELA)
            continue;
        if (rela->info >= sectionCount)
            Fail("bad relocation section %s", rela->name);
        targetElf = &sections[rela->info];
        if (!targetElf->machoSection)
        {
            if ((targetElf->flags & SHF_ALLOC) && rela->size != 0)
                Fail("relocations for section %s, which isn't converted", targetElf->name);
            continue;
        }
        target = &macho[targetElf->machoSection - 1];
        for (uint64_t r = 0; r < rela->size / 24; r++)
        {
            const uint8_t *e = elf + rela->offset + r * 24;
            uint64_t offset = Read64(e);
            uint64_t info = Read64(e + 8);
            int64_t addend = (int64_t)Read64(e + 16);
            uint32_t elfSymbol = info >> 32, type = info & 0xFFFFFFFF;
            const uint8_t *s;
            uint16_t shndx;
            uint64_t where = targetElf->machoOffset + offset;
            int symbol;

            if (type == R_X86_64_NONE)
                continue;
            if (type != R_X86_64_64)
                Fail("relocation type %u at %s+0x%llx; only 64-bit pointers can be converted", type,
                     targetElf->name, (unsigned long long)offset);
            if (elfSymbol >= elfSymbolCount || offset + 8 > targetElf->size)
                Fail("bad relocation at %s+0x%llx", targetElf->name, (unsigned long long)offset);
            s = elf + symtab->offset + elfSymbol * 24;
            shndx = Read16(s + 6);
            symbol = elfToSymbol[elfSymbol];
            if (symbol < 0 && shndx == SHN_ABS)
            {
                // A constant: nothing to relocate
                Write64(target->data + where, Read64(s + 8) + addend);
                continue;
            }
            if (symbol < 0)
            {
                // A section symbol: point from the start of the section it's in
                const struct ElfSection *to;
                struct MachoSection *toMacho;

                if ((s[4] & 0xF) != STT_SECTION || shndx >= sectionCount || !sections[shndx].machoSection)
                    Fail("relocation at %s+0x%llx to a symbol that isn't converted", targetElf->name,
                         (unsigned long long)offset);
                to = &sections[shndx];
                toMacho = &macho[to->machoSection - 1];
                if (toMacho->startSymbol < 0)
                {
                    struct Symbol *start = &symbols[symbolCount];
                    start->name = malloc(24);
                    snprintf(start->name, 24, "ltmp%d", to->machoSection - 1);
                    start->type = N_SECT;
                    start->sect = to->machoSection;
                    start->value = toMacho->addr;
                    toMacho->startSymbol = symbolCount++;
                }
                symbol = toMacho->startSymbol;
                addend += to->machoOffset + Read64(s + 8);
            }
            // Mach-O keeps the addend in the pointer itself
            Write64(target->data + where, addend);
            AddReloc(target, where, symbol);
        }
    }

    // The symbol table goes locals, defined externals, then undefined ones,
    // and relocations refer to symbols by their place in it
    {
        struct Symbol **order = malloc(symbolCount * sizeof(*order) + 1);
        uint32_t groupStart[3] = {0}, groupCount[3] = {0};
        uint32_t strSize = 1;
        uint32_t fileOffset, commandsSize;
        uint8_t *out, *p;
        size_t outSize;
        FILE *f;

        for (int i = 0; i < symbolCount; i++)
            order[i] = &symbols[i];
        qsort(order, symbolCount, sizeof(*order), CompareSymbols);
        for (int i = 0; i < symbolCount; i++)
        {
            order[i]->index = i;
            groupCount[order[i]->group]++;
            strSize += strlen(order[i]->name) + 1;
        }
        groupStart[1] = groupCount[0];
        groupStart[2] = groupCount[0] + groupCount[1];
        for (int m = 0; m < machoCount; m++)
            for (uint32_t r = 0; r < macho[m].relocCount; r++)
                macho[m].relocs[r * 2 + 1] = (macho[m].relocs[r * 2 + 1] & 0xFF000000)
                                           | symbols[macho[m].relocs[r * 2 + 1] & 0xFFFFFF].index;

        commandsSize = (72 + 80 * machoCount) + 24 + 24 + 80;
        fileOffset = 32 + commandsSize;
        for (int m = 0; m < machoCount; m++)
        {
            if (macho[m].zerofill)
                continue;
            fileOffset = AlignUp(fileOffset, (uint64_t)1 << macho[m].alignLog2);
            macho[m].fileOffset = fileOffset;
            fileOffset += macho[m].size;
        }
        fileOffset = AlignUp(fileOffset, 8);
        for (int m = 0; m < machoCount; m++)
        {
            macho[m].relocOffset = macho[m].relocCount ? fileOffset : 0;
            fileOffset += macho[m].relocCount * 8;
        }
        {
            uint32_t symOffset = fileOffset;
            uint32_t strOffset = symOffset + symbolCount * 16;
            uint32_t strSizeAligned = AlignUp(strSize, 8);
            uint64_t vmSize = 0, fileSize = 0, firstData = 32 + commandsSize;

            outSize = strOffset + strSizeAligned;
            out = calloc(outSize, 1);
            if (out == NULL)
                Fail("out of memory");
            for (int m = 0; m < machoCount; m++)
            {
                if (macho[m].addr + macho[m].size > vmSize)
                    vmSize = macho[m].addr + macho[m].size;
                if (!macho[m].zerofill && macho[m].fileOffset + macho[m].size > firstData + fileSize)
                    fileSize = macho[m].fileOffset + macho[m].size - firstData;
            }

            p = out;
            Write32(p, MH_MAGIC_64);
            Write32(p + 4, cpuType);
            Write32(p + 8, cpuSubtype);
            Write32(p + 12, MH_OBJECT);
            Write32(p + 16, 4);
            Write32(p + 20, commandsSize);
            p += 32;

            Write32(p, LC_SEGMENT_64);
            Write32(p + 4, 72 + 80 * machoCount);
            Write64(p + 24, 0);
            Write64(p + 32, vmSize);
            Write64(p + 40, firstData);
            Write64(p + 48, fileSize);
            Write32(p + 56, 7);
            Write32(p + 60, 7);
            Write32(p + 64, machoCount);
            p += 72;
            for (int m = 0; m < machoCount; m++, p += 80)
            {
                memcpy(p, macho[m].name, strlen(macho[m].name));
                memcpy(p + 16, "__DATA", 6);
                Write64(p + 32, macho[m].addr);
                Write64(p + 40, macho[m].size);
                Write32(p + 48, macho[m].fileOffset);
                Write32(p + 52, macho[m].alignLog2);
                Write32(p + 56, macho[m].relocOffset);
                Write32(p + 60, macho[m].relocCount);
                Write32(p + 64, macho[m].zerofill ? S_ZEROFILL : S_REGULAR);
            }

            Write32(p, LC_BUILD_VERSION);
            Write32(p + 4, 24);
            Write32(p + 8, PLATFORM_MACOS);
            Write32(p + 12, minOs);
            p += 24;

            Write32(p, LC_SYMTAB);
            Write32(p + 4, 24);
            Write32(p + 8, symOffset);
            Write32(p + 12, symbolCount);
            Write32(p + 16, strOffset);
            Write32(p + 20, strSizeAligned);
            p += 24;

            Write32(p, LC_DYSYMTAB);
            Write32(p + 4, 80);
            Write32(p + 8, groupStart[0]);
            Write32(p + 12, groupCount[0]);
            Write32(p + 16, groupStart[1]);
            Write32(p + 20, groupCount[1]);
            Write32(p + 24, groupStart[2]);
            Write32(p + 28, groupCount[2]);

            for (int m = 0; m < machoCount; m++)
            {
                if (!macho[m].zerofill)
                    memcpy(out + macho[m].fileOffset, macho[m].data, macho[m].size);
                for (uint32_t r = 0; r < macho[m].relocCount * 2; r++)
                    Write32(out + macho[m].relocOffset + r * 4, macho[m].relocs[r]);
            }

            {
                uint32_t strPos = 1;
                for (int i = 0; i < symbolCount; i++)
                {
                    uint8_t *n = out + symOffset + i * 16;
                    size_t length = strlen(order[i]->name);

                    Write32(n, strPos);
                    n[4] = order[i]->type;
                    n[5] = order[i]->sect;
                    n[6] = order[i]->desc & 0xFF;
                    n[7] = order[i]->desc >> 8;
                    Write64(n + 8, order[i]->value);
                    memcpy(out + strOffset + strPos, order[i]->name, length);
                    strPos += length + 1;
                }
            }
        }

        f = fopen(output, "wb");
        if (f == NULL || fwrite(out, 1, outSize, f) != outSize || fclose(f) != 0)
        {
            sInput = output;
            Fail("can't write it");
        }
    }
    return 0;
}
