/*
 * Copyright (C) 1997-2001, The University of Queensland
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/***************************************************************************/ /**
 * \file ElfBinaryLoader.cpp
 * Desc: This file contains the implementation of the class ElfBinaryLoader.
 ******************************************************************************/

#include "ElfBinaryLoader.h"

#include "ElfTypes.h"
#include "include/config.h"
#include "util/util.h"
#include "include/IBoomerang.h"
#include "db/IBinaryImage.h"
#include "db/IBinarySymbols.h"

#include <QtCore/QDebug>
#include <sys/types.h> // Next three for open()
#include <sys/stat.h>
#include <fcntl.h>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <inttypes.h>
#include <QBuffer>
#include <QFile>


struct SectionParam
{
	QString  Name;
	ADDRESS  SourceAddr;
	size_t   Size;
	size_t   entry_size;
	bool     ReadOnly;
	bool     Bss;
	bool     Code;
	bool     Data;
	ADDRESS  image_ptr;
	unsigned uType;             // Type of section (format dependent)
};

// not part of anonymous namespace, since it would create an ambiguity
// anonymous_namespace::Translated_ElfSym vs. ElfTypes.h/Translated_ElfSym declarations
struct Translated_ElfSym
{
	QString          Name;
	ElfSymType       Type;
	ElfSymBinding    Binding;
	ElfSymVisibility Visibility;
	uint32_t         SymbolSize;
	uint16_t         SectionIdx;
	ADDRESS          Value;
};

typedef std::map<QString, int, std::less<QString> > StrIntMap;


ElfBinaryLoader::ElfBinaryLoader()
	: m_nextExtern(ADDRESS::g(0L))
{
	init(); // Initialise all the common stuff
}


ElfBinaryLoader::~ElfBinaryLoader()
{
	// Delete the array of import stubs
	delete  []m_importStubs;
	delete  []m_shLink;
	delete  []m_shInfo;
}


void ElfBinaryLoader::initialize(IBinaryImage *image, IBinarySymbolTable *symbols)
{
	m_binaryImage = image;
	m_symbols     = symbols;
}


void ElfBinaryLoader::init()
{
	m_loadedImage   = nullptr;
	m_elfHeader     = nullptr;  // No ELF header
	m_programHdrs   = nullptr;  // No program headers
	m_sectionhdrs   = nullptr;  // No section headers
	m_strings       = nullptr;  // No strings
	m_relocSection  = nullptr;
	m_symbolSection = nullptr;
	m_pltMin        = 0;  // No PLT limits
	m_pltMax        = 0;
	m_lastSize      = 0;
	m_importStubs   = nullptr;
	m_elfSections.clear();
}


// Hand decompiled from sparc library function
extern "C" { // So we can call this with dlopen() in test function
Q_DECL_EXPORT
unsigned elf_hash(const char *o0)
{
	int        o3  = *o0;
	const char *g1 = o0;
	unsigned   o4  = 0;

	while (o3 != 0) {
		o4 <<= 4;
		o3  += o4;
		g1++;
		o4 = o3 & 0xf0000000;

		if (o4 != 0) {
			int o2 = (int)((unsigned)o4 >> 24);
			o3 = o3 ^ o2;
		}

		o4 = o3 & ~o4;
		o3 = *g1;
	}

	return o4;
}
} // extern "C"


bool ElfBinaryLoader::loadFromMemory(QByteArray& img)
{
	m_loadedImageSize = img.size();

	// Allocate memory to hold the file
	m_loadedImage = (Byte *)img.data();

	m_elfHeader = (Elf32_Ehdr *)img.data(); // Save a lot of casts

	// Basic checks
	if ((m_elfHeader->e_ident[0] != 0x7F) ||
		(m_elfHeader->e_ident[1] != 'E') ||
		(m_elfHeader->e_ident[2] != 'L') ||
		(m_elfHeader->e_ident[3] != 'F')) {
		fprintf(stderr, "Incorrect header: %02X %02X %02X %02X\n",
				m_elfHeader->e_ident[0],
				m_elfHeader->e_ident[1],
				m_elfHeader->e_ident[2],
				m_elfHeader->e_ident[3]);
		return 0;
	}

	switch (m_elfHeader->endianness)
	{
	case 1: // little endian
		m_bigEndian = false;
		break;

	case 2: // big endian
		m_bigEndian = true;
		break;

	default:
		qWarning() << QString("Unknown endianness ").arg(m_elfHeader->endianness, 2, 16, QChar('0'));
		return 0;
	}

	// Set up program header pointer (in case needed)
	DWord phOffset = elfRead4(&m_elfHeader->e_phoff);

	if (phOffset > 0) {
		m_programHdrs = (Elf32_Phdr *)(m_loadedImage + phOffset);
	}

	// Set up section header pointer
	DWord shOffset = elfRead4(&m_elfHeader->e_shoff);

	if (shOffset > 0) {
		m_sectionhdrs = (Elf32_Shdr *)(m_loadedImage + shOffset);
	}

	// Set up section header string table pointer
	// NOTE: it does not appear that endianness affects shorts.. they are always in little endian format
	// Gerard: I disagree. I need the elfRead on linux/i386
	const DWord stringSectionIndex = elfRead2(&m_elfHeader->e_shstrndx); // pHeader->e_shstrndx;

	if (stringSectionIndex > 0) {
		m_strings = (const char *)(m_loadedImage + elfRead4(&m_sectionhdrs[stringSectionIndex].sh_offset));
	}

	// Number of sections
	SWord numSections = elfRead2(&m_elfHeader->e_shnum);
	// Set up the m_sh_link and m_sh_info arrays
	m_shLink = new int[numSections];
	m_shInfo = new int[numSections];

	// Number of elf sections
	bool    bGotCode         = false; // True when have seen a code sect
	ADDRESS arbitaryLoadAddr = ADDRESS::g(0x08000000);

	for (SWord i = 0; i < numSections; i++) {
		// Get section information.
		Elf32_Shdr *pShdr = m_sectionhdrs + i;

		if ((Byte *)pShdr > m_loadedImage + m_loadedImageSize) {
			fprintf(stderr, "section %u header is outside the image size\n", i);
			return false;
		}

		const char *sectionName = m_strings + elfRead4(&pShdr->sh_name);

		if ((Byte *)sectionName > m_loadedImage + m_loadedImageSize) {
			fprintf(stderr, "name for section %u is outside the image size\n", i);
			return false;
		}

		SectionParam sect;
		sect.Name = sectionName;
		// Can't use the SHF_ALLOC bit to determine bss section; the bss section has SHF_ALLOC but also SHT_NOBITS.
		// (But many other sections, such as .comment, also have SHT_NOBITS). So for now, just use the name
		//      if ((elfRead4(&pShdr->sh_flags) & SHF_ALLOC) == 0)
		sect.Bss      = (strcmp(sectionName, ".bss") == 0);
		sect.Code     = false;
		sect.Data     = false;
		sect.ReadOnly = false;
		int _off = elfRead4(&pShdr->sh_offset);

		if (_off) {
			sect.image_ptr = ADDRESS::host_ptr(m_loadedImage + _off);
		}

		sect.SourceAddr = elfRead4(&pShdr->sh_addr);
		sect.Size       = elfRead4(&pShdr->sh_size);

		if (sect.SourceAddr.isZero() && strncmp(sectionName, ".rel", 4)) {
			const DWord align = elfRead4(&pShdr->sh_addralign);

			if (align > 1) {
				if (arbitaryLoadAddr.m_value % align != 0) {
					arbitaryLoadAddr += align - (arbitaryLoadAddr.m_value % align);
				}
			}

			sect.SourceAddr   = arbitaryLoadAddr;
			arbitaryLoadAddr += sect.Size ? sect.Size : 1;
		}

		sect.uType      = elfRead4(&pShdr->sh_type);
		m_shLink[i]     = elfRead4(&pShdr->sh_link);
		m_shInfo[i]     = elfRead4(&pShdr->sh_info);
		sect.entry_size = elfRead4(&pShdr->sh_entsize);

		if (sect.SourceAddr + sect.Size > m_nextExtern) {
			m_firstExtern = m_nextExtern = sect.SourceAddr + sect.Size;
		}

		if ((elfRead4(&pShdr->sh_flags) & SHF_WRITE) == 0) {
			sect.ReadOnly = true;
		}

		if (elfRead4(&pShdr->sh_flags) & SHF_EXECINSTR) {
			sect.Code = true;
			bGotCode  = true; // We've got to a code section
		}

		// Deciding what is data and what is not is actually quite tricky but important.
		// For example, it's crucial to flag the .exception_ranges section as data,
		// otherwise there is a "hole" in the allocation map, that means
		// that there is more than one "delta" from a read-only section to a page,
		// and in the end using -C results in a file that looks OK
		// but when run just says "Killed". So we use the Elf designations;
		// it seems that ALLOC.!EXEC -> data
		// But we don't want sections before the .text section, like .interp, .hash, etc etc.
		// Hence bGotCode.
		//
		// NOTE: this ASSUMES that sections appear in a sensible order in the input binary file:
		// junk, code, rodata, data, bss
		if (bGotCode &&
			((elfRead4(&pShdr->sh_flags) & (SHF_EXECINSTR | SHF_ALLOC)) == SHF_ALLOC) &&
			(elfRead4(&pShdr->sh_type) != SHT_NOBITS)) {
			sect.Data = true;
		}

		m_elfSections.push_back(sect);
	} // for each section

	// assign arbitary addresses to .rel.* sections too
	for (SectionParam& sect : m_elfSections) {
		if (sect.SourceAddr.isZero() && sect.Name.startsWith(".rel")) {
			sect.SourceAddr   = arbitaryLoadAddr;
			arbitaryLoadAddr += sect.Size ? sect.Size : 1;
		}
	}

	// Inform Boomerang about new sections
	for (SectionParam par : m_elfSections) {
		if (par.Size == 0) {
			// this is most probably the NULL section
			qDebug() << "Not adding 0 sized section " << par.Name;
			continue;
		}

		IBinarySection *sect = m_binaryImage->createSection(par.Name, par.SourceAddr, par.SourceAddr + par.Size);
		assert(sect);

		if (sect) {
			sect->setBss(par.Bss)
			   .setCode(par.Code)
			   .setData(par.Data)
			   .setEndian(m_bigEndian)
			   .setHostAddr(par.image_ptr)
			   .setEntrySize(par.entry_size);

			if (!(par.Bss || par.SourceAddr.isZero())) {
				sect->addDefinedArea(par.SourceAddr, par.SourceAddr + par.Size);
			}
		}
	}

	// Add symbol info. Note that some symbols will be in the main table only, and others in the dynamic table only.
	// So the best idea is to add symbols for all sections of the appropriate type
	for (unsigned i = 1; i < m_elfSections.size(); ++i) {
		unsigned uType = m_elfSections[i].uType;

		if ((uType == SHT_SYMTAB) || (uType == SHT_DYNSYM)) {
			addSyms(i);
		}
	}

	// Save the relocation to symbol table info
	IBinarySection *pRel = m_binaryImage->getSectionInfoByName(".rela.text");

	if (pRel) {
		m_relocHasAddend = true;                                     // Remember its a relA table
		m_relocSection   = (Elf32_Rel *)pRel->getHostAddr().m_value; // Save pointer to reloc table
		// SetRelocInfo(pRel);
	}
	else {
		m_relocHasAddend = false;
		pRel             = m_binaryImage->getSectionInfoByName(".rel.text");

		if (pRel) {
			// SetRelocInfo(pRel);
			m_relocSection = (Elf32_Rel *)pRel->getHostAddr().m_value;          // Save pointer to reloc table
		}
	}

	// Find the PLT limits. Required for IsDynamicLinkedProc(), e.g.
	IBinarySection *pPlt = m_binaryImage->getSectionInfoByName(".plt");

	if (pPlt) {
		m_pltMin = pPlt->getSourceAddr();
		m_pltMax = pPlt->getSourceAddr() + pPlt->getSize();
	}

	// Apply relocations; important when the input program is not compiled with -fPIC
	applyRelocations();
	markImports();
	return true; // Success
}


void ElfBinaryLoader::unload()
{
	init(); // Set all internal state to 0
}


const char *ElfBinaryLoader::getStrPtr(int idx, int offset)
{
	if (idx < 0) {
		// Most commonly, this will be an index of -1, because a call to GetSectionIndexByName() failed
		fprintf(stderr, "Error! GetStrPtr passed index of %d\n", idx);
		return (char *)"Error!";
	}

	// Get a pointer to the start of the string table
	char *pSym = (char *)m_elfSections[idx].image_ptr.m_value;
	// Just add the offset
	return pSym + offset;
}


ADDRESS ElfBinaryLoader::findRelPltOffset(int i)
{
	const IBinarySection *siPlt    = m_binaryImage->getSectionInfoByName(".plt");
	ADDRESS              addrPlt   = siPlt ? siPlt->getSourceAddr() : ADDRESS::g(0L);
	const IBinarySection *siRelPlt = m_binaryImage->getSectionInfoByName(".rel.plt");
	int sizeRelPlt = 8; // Size of each entry in the .rel.plt table

	if (siRelPlt == nullptr) {
		siRelPlt   = m_binaryImage->getSectionInfoByName(".rela.plt");
		sizeRelPlt = 12; // Size of each entry in the .rela.plt table is 12 bytes
	}

	ADDRESS addrRelPlt = ADDRESS::g(0L);
	int     numRelPlt  = 0;

	if (siRelPlt) {
		addrRelPlt = siRelPlt->getHostAddr();
		numRelPlt  = sizeRelPlt ? siRelPlt->getSize() / sizeRelPlt : 0;
	}
	else {
		return NO_ADDRESS; // neither .rel.plt nor .rela.plt are available
	}

	int first = i;

	if (first >= numRelPlt) {
		first = numRelPlt - 1;
	}

	int curr         = first;
	int pltEntrySize = siPlt->getEntrySize();

	do {
		// Each entry is sizeRelPlt bytes, and will contain the offset, then the info (addend optionally follows)
		DWord *pEntry    = (DWord *)(addrRelPlt + (curr * sizeRelPlt)).m_value;
		int   entry      = elfRead4(pEntry + 1);
		int   sym        = entry >> 8;         // The symbol index is in the top 24 bits (Elf32 only)
		int   entry_type = entry & 0xFF;

		if (sym == i) {
			const IBinarySection *targetSect = m_binaryImage->getSectionInfoByAddr(ADDRESS::n(elfRead4(pEntry)));

			if (targetSect->getName().contains("got")) {
				int c           = elfRead4(pEntry) - targetSect->getSourceAddr().m_value;
				int plt_offset2 = elfRead4((DWord *)(targetSect->getHostAddr() + c).m_value);
				int plt_idx     = (plt_offset2 % pltEntrySize);

				if (entry_type == R_386_JUMP_SLOT) {
					return ADDRESS::n(plt_offset2 - 6);
				}

				return addrPlt + plt_idx * pltEntrySize;

				qDebug() << "x";
			}

			int plt_offset = elfRead4(pEntry) - siPlt->getSourceAddr().m_value;
			// Found! Now we want the native address of the associated PLT entry.
			// For now, assume a size of 0x10 for each PLT entry, and assume that each entry in the .rel.plt section
			// corresponds exactly to an entry in the .plt (except there is one dummy .plt entry)
			return addrPlt + plt_offset;

			return addrPlt + pltEntrySize * (curr + 1);
			// return ADDRESS::n(elfRead4(pEntry));
			// return addrPlt + 0xC * (curr + 1);
		}

		if (--curr < 0) {
			curr = numRelPlt - 1;
		}
	} while (curr != first); // Will eventually wrap around to first if not present

	return ADDRESS::g(0L);   // Exit if this happens
}


void ElfBinaryLoader::processSymbol(Translated_ElfSym& sym, int e_type, int i)
{
	static QString       current_file;
	bool                 imported = sym.SectionIdx == SHT_NULL;
	bool                 local    = sym.Binding == STB_LOCAL || sym.Binding == STB_WEAK;
	const IBinarySection *siPlt   = m_binaryImage->getSectionInfoByName(".plt");

	if (sym.Value.isZero() && siPlt) { // && i < max_i_for_hack) {
		// Special hack for gcc circa 3.3.3: (e.g. test/pentium/settest).  The value in the dynamic symbol table
		// is zero!  I was assuming that index i in the dynamic symbol table would always correspond to index i
		// in the .plt section, but for fedora2_true, this doesn't work. So we have to look in the .rel[a].plt
		// section. Thanks, gcc!  Note that this hack can cause strange symbol names to appear
		sym.Value = findRelPltOffset(i);
	}
	else if (e_type == E_REL) {
		if (sym.SectionIdx < m_elfSections.size()) {
			sym.Value += m_elfSections[sym.SectionIdx].SourceAddr;
		}
	}

	// try to find given symbol, if it has Value of 0, try to use the name.
	const IBinarySymbol *symbol = sym.Value.isZero() ? m_symbols->find(sym.Name) : m_symbols->find(sym.Value);

	// Ensure no overwriting (except functions)
	if (symbol != nullptr) { // TODO: if symbol already exists
		return;
	}

	if ((sym.Binding == STB_WEAK) && (sym.Type == STT_NOTYPE)) {
		return;
	}

	if (sym.Type == STT_FILE) {
		current_file = sym.Name;
		return;
	}

	if ((sym.Binding != STB_LOCAL) && !current_file.isEmpty()) {
		// first non-local symbol, clear the current_file
		current_file.clear();
	}

	if (sym.Name.isEmpty()) {
		return;
	}

	if (sym.Value.isZero()) {
		qDebug() << "Skipping symbol " << sym.Name << "with unknown location!";
		return;
	}

	// TODO: add more symbol information here (function/export etc. ) ?
	IBinarySymbol& new_symbol(m_symbols->create(sym.Value, sym.Name, local));
	new_symbol.setSize(elfRead4(&m_symbolSection[i].st_size));

	if (imported) {
		new_symbol.setAttr("Imported", true);
	}

	if (sym.Type == STT_FUNC) {
		new_symbol.setAttr("Function", true);
	}

	if (!current_file.isEmpty()) {
		new_symbol.setAttr("SourceFile", current_file);
	}
}


void ElfBinaryLoader::addSyms(int secIndex)
{
	SWord               e_type = elfRead2(&m_elfHeader->e_type);
	const SectionParam& pSect  = m_elfSections[secIndex];
	// Calc number of symbols
	int nSyms = pSect.Size / pSect.entry_size;

	m_symbolSection = (const Elf32_Sym *)pSect.image_ptr.m_value; // Pointer to symbols
	int strIdx = m_shLink[secIndex];                              // sh_link points to the string table

	// Index 0 is a dummy entry
	for (int i = 1; i < nSyms; i++) {
		Translated_ElfSym trans;
		ADDRESS           val  = ADDRESS::g(elfRead4(&m_symbolSection[i].st_value));
		int               name = elfRead4(&m_symbolSection[i].st_name);

		if (name == 0) { /* Silly symbols with no names */
			continue;
		}

		QString str(getStrPtr(strIdx, name));
		// Hack off the "@@GLIBC_2.0" of Linux, if present
		trans.Name       = str.left(str.indexOf("@@"));
		trans.Type       = ELF32_ST_TYPE(m_symbolSection[i].st_info);
		trans.Binding    = ELF32_ST_BIND(m_symbolSection[i].st_info);
		trans.Visibility = ELF32_ST_VISIBILITY(m_symbolSection[i].st_other);
		trans.SymbolSize = ELF32_ST_VISIBILITY(m_symbolSection[i].st_size);
		trans.SectionIdx = elfRead2(&m_symbolSection[i].st_shndx);
		trans.Value      = val;
		processSymbol(trans, e_type, i);
	}

	ADDRESS uMain = getMainEntryPoint();

	if ((uMain != NO_ADDRESS) && (nullptr == m_symbols->find(uMain))) {
		// Ugh - main mustn't have the STT_FUNC attribute. Add it
		m_symbols->create(uMain, "main");
	}
}


void ElfBinaryLoader::addRelocsAsSyms(uint32_t relSecIdx)
{
	if (relSecIdx >= m_elfSections.size()) {
		return;
	}

	const SectionParam& pSect(m_elfSections[relSecIdx]);
	// Calc number of relocations
	int nRelocs = pSect.Size / pSect.entry_size;
	m_relocSection = (const Elf32_Rel *)pSect.image_ptr.m_value;    // Pointer to symbols
	int symSecIdx = m_shLink[relSecIdx];
	int strSecIdx = m_shLink[symSecIdx];

	// Index 0 is a dummy entry
	for (int i = 1; i < nRelocs; i++) {
		ADDRESS val      = ADDRESS::g(elfRead4(&m_relocSection[i].r_offset));
		int     symIndex = elfRead4(&m_relocSection[i].r_info) >> 8;
		int     flags    = elfRead4(&m_relocSection[i].r_info);

		if ((flags & 0xFF) == R_386_32) {
			// Lookup the value of the symbol table entry
			ADDRESS a = ADDRESS::g(elfRead4(&m_symbolSection[symIndex].st_value));

			if (m_symbolSection[symIndex].st_info & STT_SECTION) {
				a = m_elfSections[elfRead2(&m_symbolSection[symIndex].st_shndx)].SourceAddr;
			}

			// Overwrite the relocation value... ?
			m_binaryImage->writeNative4(val, a.m_value);
			continue;
		}

		if ((flags & R_386_PC32) == 0) {
			continue;
		}

		if (symIndex == 0) { /* Silly symbols with no names */
			continue;
		}

		QString str(getStrPtr(strSecIdx, elfRead4(&m_symbolSection[symIndex].st_name)));

		str = str.left(str.indexOf("@@")); // Hack off the "@@GLIBC_2.0" of Linux, if present

		std::map<ADDRESS, QString>::iterator it;
		auto symbol = m_symbols->find(str);
		// Add new extern
		ADDRESS location = symbol ? symbol->getLocation() : m_nextExtern;

		if (nullptr == symbol) {
			m_symbols->create(m_nextExtern, str);
			m_nextExtern += 4;
		}

		m_binaryImage->writeNative4(val, (location - val - 4).m_value);
	}
}


ADDRESS ElfBinaryLoader::getMainEntryPoint()
{
	auto sym = m_symbols->find("main");

	if (sym) {
		return sym->getLocation();
	}

	return NO_ADDRESS;
}


ADDRESS ElfBinaryLoader::getEntryPoint()
{
	return ADDRESS::g(elfRead4(&m_elfHeader->e_entry));
}


ADDRESS ElfBinaryLoader::nativeToHostAddress(ADDRESS uNative)
{
	if (m_binaryImage->getNumSections() == 0) {
		return ADDRESS::g(0L);
	}

	return m_binaryImage->getSectionInfo(1)->getHostAddr() - m_binaryImage->getSectionInfo(1)->getSourceAddr() + uNative;
}


bool ElfBinaryLoader::postLoad(void *handle)
{
	Q_UNUSED(handle);
	// This function is called after an archive member has been loaded by ElfArchiveFile

	// Save the elf pointer
	// m_elf = (Elf*) handle;

	// return ProcessElfFile();
	return false;
}


void ElfBinaryLoader::close()
{
	unload();
}


LOAD_FMT ElfBinaryLoader::getFormat() const
{
	return LOADFMT_ELF;
}


MACHINE ElfBinaryLoader::getMachine() const
{
	SWord machine = elfRead2(&m_elfHeader->e_machine);

	if ((machine == EM_SPARC) || (machine == EM_SPARC32PLUS)) {
		return MACHINE_SPARC;
	}
	else if (machine == EM_386) {
		return MACHINE_PENTIUM;
	}
	else if (machine == EM_PA_RISC) {
		return MACHINE_HPRISC;
	}
	else if (machine == EM_68K) {
		return MACHINE_PALM; // Unlikely
	}
	else if (machine == EM_PPC) {
		return MACHINE_PPC;
	}
	else if (machine == EM_ST20) {
		return MACHINE_ST20;
	}
	else if (machine == EM_MIPS) {
		return MACHINE_MIPS;
	}
	else if (machine == EM_X86_64) {
		fprintf(stderr, "Error: ElfBinaryFile::GetMachine: The AMD x86-64 architecture is not supported yet\n");
		return (MACHINE)-1;
	}

	// An unknown machine type
	fprintf(stderr, "Error: ElfBinaryFile::GetMachine: Unsupported machine type: %d (0x%x)\n", machine, (unsigned int)machine);
	fprintf(stderr, "(Please add a description for this type, thanks!)\n");
	return (MACHINE)-1;
}


bool ElfBinaryLoader::isLibrary() const
{
	int type = elfRead2(&((Elf32_Ehdr *)m_loadedImage)->e_type);

	return(type == ET_DYN);
}


QStringList ElfBinaryLoader::getDependencyList()
{
	QStringList    result;
	ADDRESS        stringtab = NO_ADDRESS;
	IBinarySection *dynsect  = m_binaryImage->getSectionInfoByName(".dynamic");

	if (dynsect == nullptr) {
		return result; /* no dynamic section = statically linked */
	}

	Elf32_Dyn *dyn;

	for (dyn = (Elf32_Dyn *)dynsect->getHostAddr().m_value; dyn->d_tag != DT_NULL; dyn++) {
		if (dyn->d_tag == DT_STRTAB) {
			stringtab = ADDRESS::g(dyn->d_un.d_ptr);
			break;
		}
	}

	if (stringtab == NO_ADDRESS) { /* No string table = no names */
		return result;
	}

	stringtab = nativeToHostAddress(stringtab);

	for (dyn = (Elf32_Dyn *)dynsect->getHostAddr().m_value; dyn->d_tag != DT_NULL; dyn++) {
		if (dyn->d_tag == DT_NEEDED) {
			const char *need = (char *)(stringtab + dyn->d_un.d_val).m_value;

			if (need != nullptr) {
				result << need;
			}
		}
	}

	return result;
}


ADDRESS ElfBinaryLoader::getImageBase()
{
	return m_baseAddr;
}


size_t ElfBinaryLoader::getImageSize()
{
	return m_imageSize;
}


void ElfBinaryLoader::markImports()
{
	IBinarySymbolTable::const_iterator first = m_symbols->begin();
	IBinarySymbolTable::const_iterator last  = m_symbols->begin();
	IBinarySymbolTable::const_iterator end   = m_symbols->end();

	for ( ; first != end; ++first) {
		if ((*first)->getLocation() >= m_pltMin) {
			break;
		}
	}

	for (last = first; last != end; ++last) {
		if ((*last)->getLocation() >= m_pltMax) {
			break;
		}

		const IBinarySymbol *sym = *last;
		sym->setAttr("Imported", true);
	}
}


SWord ElfBinaryLoader::elfRead2(const SWord *ps) const
{
	assert(ps);
	SWord r = *ps;

	if (m_bigEndian) {
		r = (r >> 8 & 0x00FF) | (r << 8 & 0xFF00);
	}

	return r;
}


DWord ElfBinaryLoader::elfRead4(const DWord *pi) const
{
	assert(pi);
	DWord r = *pi;

	if (m_bigEndian) {
		r = (r >> 16 & 0x0000FFFF) | (r << 16 & 0xFFFF0000);
		r = (r >> 8 & 0x00FF00FF) | (r << 8 & 0xFF00FF00);
	}

	return r;
}


void ElfBinaryLoader::elfWrite4(DWord *pi, DWord val)
{
	// swap endian
	val = elfRead4(&val);

	Byte *p = (Byte *)pi;
	*p++ = (char)val;
	*p++ = (char)(val >> 8);
	*p++ = (char)(val >> 16);
	*p   = (char)(val >> 24);
}


void ElfBinaryLoader::applyRelocations()
{
	int nextFakeLibAddr = -2; // See R_386_PC32 below; -1 sometimes used for main

	if (m_loadedImage == nullptr) {
		return; // No file loaded
	}

	const SWord machine = elfRead2(&m_elfHeader->e_machine);
	const SWord e_type  = elfRead2(&m_elfHeader->e_type);

	switch (machine)
	{
	case EM_SPARC:

		for (size_t i = 1; i < m_elfSections.size(); ++i) {
			const SectionParam& ps(m_elfSections[i]);

			if ((ps.uType != SHT_REL) && (ps.uType == SHT_RELA)) {
				DWord *pReloc = (DWord *)ps.image_ptr.m_value;
				DWord size    = ps.Size;

				// NOTE: the r_offset is different for .o files (E_REL in the e_type header field) than for exe's
				// and shared objects!
				// ADDRESS destNatOrigin = ADDRESS::g(0L), destHostOrigin = ADDRESS::g(0L);
				for (unsigned u = 0; u < size; u += sizeof(Elf32_Rela)) {
					Elf32_Rela r;
					r.r_offset = elfRead4(pReloc++);
					r.r_info   = elfRead4(pReloc++);
					r.r_addend = elfRead4(pReloc++);

					unsigned char relType = r.r_info & 0xFF;
					// DWord symTabIndex     = r.r_info >> 8;

					switch (relType)
					{
					case 0: // R_386_NONE: just ignore (common)
						break;

					default:
					case R_SPARC_HI22:
					case R_SPARC_LO10:
					case R_SPARC_GLOB_DAT:
						qWarning() << "Unhandled sparc relocation";
						break;
					}
				}
			}
		}

		qDebug() << "Unhandled relocation!";
		break; // Not implemented yet

	case EM_386:

		for (size_t i = 1; i < m_elfSections.size(); ++i) {
			const SectionParam& ps(m_elfSections[i]);

			if (ps.uType == SHT_REL) {
				// A section such as .rel.dyn or .rel.plt (without an addend field).
				// Each entry has 2 words: r_offet and r_info. The r_offset is just the offset from the beginning
				// of the section (section given by the section header's sh_info) to the word to be modified.
				// r_info has the type in the bottom byte, and a symbol table index in the top 3 bytes.
				// A symbol table offset of 0 (STN_UNDEF) means use value 0. The symbol table involved comes from
				// the section header's sh_link field.
				DWord *pReloc = (DWord *)ps.image_ptr.m_value;
				DWord size    = ps.Size;

				// NOTE: the r_offset is different for .o files (E_REL in the e_type header field) than for exe's
				// and shared objects!
				ADDRESS destNatOrigin = ADDRESS::g(0L), destHostOrigin = ADDRESS::g(0L);

				if (e_type == E_REL) {
					int destSection = m_shInfo[i];
					destNatOrigin  = m_elfSections[destSection].SourceAddr;
					destHostOrigin = m_elfSections[destSection].image_ptr;
				}

				int             symSection   = m_shLink[i];          // Section index for the associated symbol table
				int             strSection   = m_shLink[symSection]; // Section index for the string section assoc with this
				char            *pStrSection = (char *)m_elfSections[strSection].image_ptr.m_value;
				const Elf32_Sym *symOrigin   = (const Elf32_Sym *)m_elfSections[symSection].image_ptr.m_value;

				for (unsigned u = 0; u < size; u += 2 * sizeof(unsigned)) {
					DWord r_offset    = elfRead4(pReloc++);
					DWord info        = elfRead4(pReloc++);
					Byte  relType     = (unsigned char)info;
					DWord symTabIndex = info >> 8;
					DWord *pRelWord; // Pointer to the word to be relocated

					if (e_type == E_REL) {
						pRelWord = ((DWord *)(destHostOrigin + r_offset).m_value);
					}
					else {
						const IBinarySection *destSec = m_binaryImage->getSectionInfoByAddr(ADDRESS::n(r_offset));
						pRelWord      = (DWord *)(destSec->getHostAddr() - destSec->getSourceAddr() + r_offset).m_value;
						destNatOrigin = ADDRESS::g(0L);
					}

					ADDRESS  A, S = ADDRESS::g(0L), P;
					unsigned nsec;

					switch (relType)
					{
					case 0: // R_386_NONE: just ignore (common)
						break;

					case 1: // R_386_32: S + A
						S = elfRead4((DWord *)&symOrigin[symTabIndex].st_value);

						if (e_type == E_REL) {
							nsec = elfRead2(&symOrigin[symTabIndex].st_shndx);

							if (nsec < m_elfSections.size()) {
								S += m_elfSections[nsec].SourceAddr;
							}
						}

						A = elfRead4(pRelWord);
						elfWrite4(pRelWord, (S + A).m_value);
						break;

					case 2: // R_386_PC32: S + A - P

						if (ELF32_ST_TYPE(symOrigin[symTabIndex].st_info) == STT_SECTION) {
							nsec = elfRead2(&symOrigin[symTabIndex].st_shndx);

							if (nsec < m_elfSections.size()) {
								S += m_elfSections[nsec].SourceAddr;
							}
						}
						else {
							S = elfRead4((DWord *)&symOrigin[symTabIndex].st_value);

							if (S.isZero()) {
								// This means that the symbol doesn't exist in this module, and is not accessed
								// through the PLT, i.e. it will be statically linked, e.g. strcmp. We have the
								// name of the symbol right here in the symbol table entry, but the only way
								// to communicate with the loader is through the target address of the call.
								// So we use some very improbable addresses (e.g. -1, -2, etc) and give them entries
								// in the symbol table
								DWord nameOffset = elfRead4((DWord *)&symOrigin[symTabIndex].st_name);
								char  *pName     = pStrSection + nameOffset;

								// this is too slow, I'm just going to assume it is 0
								// S = GetAddressByName(pName);
								// if (S == (e_type == E_REL ? 0x8000000 : 0)) {
								S = nextFakeLibAddr--; // Allocate a new fake address
								m_symbols->create(S, pName);
								// }
							}
							else if (e_type == E_REL) {
								nsec = elfRead2(&symOrigin[symTabIndex].st_shndx);

								if (nsec < m_elfSections.size()) {
									S += m_elfSections[nsec].SourceAddr;
								}
							}
						}

						A = elfRead4(pRelWord);
						P = destNatOrigin + r_offset;
						elfWrite4(pRelWord, (S + A - P).m_value);
						break;

					case 7:
					case 8:    // R_386_RELATIVE
						break; // No need to do anything with these, if a shared object

					default:
						// std::cout << "Relocation type " << (int)relType << " not handled yet\n";
						;
					}
				}
			}
		}

	default:
		break; // Not implemented
	}
}


bool ElfBinaryLoader::isRelocationAt(ADDRESS uNative)
{
	// int nextFakeLibAddr = -2;            // See R_386_PC32 below; -1 sometimes used for main
	if (m_loadedImage == nullptr) {
		return false; // No file loaded
	}

	const SWord machine = elfRead2(&m_elfHeader->e_machine);
	const SWord e_type  = elfRead2(&m_elfHeader->e_type);

	switch (machine)
	{
	case EM_386:

		for (size_t i = 1; i < m_elfSections.size(); ++i) {
			const SectionParam& ps(m_elfSections[i]);

			if (ps.uType == SHT_REL) {
				// A section such as .rel.dyn or .rel.plt (without an addend field).
				// Each entry has 2 words: r_offet and r_info. The r_offset is just the offset from the beginning
				// of the section (section given by the section header's sh_info) to the word to be modified.
				// r_info has the type in the bottom byte, and a symbol table index in the top 3 bytes.
				// A symbol table offset of 0 (STN_UNDEF) means use value 0. The symbol table involved comes from
				// the section header's sh_link field.
				DWord *pReloc = (DWord *)ps.image_ptr.m_value;
				DWord size    = ps.Size;

				// NOTE: the r_offset is different for .o files (E_REL in the e_type header field) than for exe's
				// and shared objects!
				ADDRESS destNatOrigin = ADDRESS::g(0L), destHostOrigin;

				if (e_type == E_REL) {
					int destSection = m_shInfo[i];
					destNatOrigin  = m_elfSections[destSection].SourceAddr;
					destHostOrigin = m_elfSections[destSection].image_ptr;
				}

				// int symSection = m_sh_link[i];            // Section index for the associated symbol table
				// int strSection = m_sh_link[symSection];    // Section index for the string section assoc with this
				// char* pStrSection = (char*)m_pSections[strSection].uHostAddr;
				// Elf32_Sym* symOrigin = (Elf32_Sym*) m_pSections[symSection].uHostAddr;
				for (DWord u = 0; u < size; u += 2 * sizeof(DWord)) {
					DWord r_offset = elfRead4(pReloc++);
					// DWord info     = elfRead4(pReloc);
					pReloc++;

					// unsigned char relType = (unsigned char) info;
					// unsigned symTabIndex = info >> 8;
					ADDRESS pRelWord; // Pointer to the word to be relocated

					if (e_type == E_REL) {
						pRelWord = destNatOrigin + r_offset;
					}
					else {
						const IBinarySection *destSec = m_binaryImage->getSectionInfoByAddr(ADDRESS::g(r_offset));
						pRelWord      = destSec->getSourceAddr() + r_offset;
						destNatOrigin = 0;
					}

					if (uNative == pRelWord) {
						return true;
					}
				}
			}
		}

		break;

	case EM_SPARC:
	default:
		qDebug() << "Unhandled relocation !";
		break; // Not implemented yet
	}

	return false;
}


#define TESTMAGIC4(buf, off, a, b, c, d)    (buf[off] == a && buf[off + 1] == b && buf[off + 2] == c && buf[off + 3] == d)

int ElfBinaryLoader::canLoad(QIODevice& fl) const
{
	QByteArray contents = fl.read(4);

	return TESTMAGIC4(contents.data(), 0, '\177', 'E', 'L', 'F') ? 4 : 0;
}


DEFINE_PLUGIN(PluginType::Loader, IFileLoader, ElfBinaryLoader,
			  "ELF32 loader plugin", "0.4.0", "Boomerang developers")