//Module name: Floating IPS, shared core for all frontends
//Author: Alcaro
//Date: June 18, 2015
//Licence: GPL v3.0 or higher

#include "flips.h"

//get rid of dependencies on libstdc++, they eat 200KB on Windows
void* operator new(size_t n) { return malloc(n); } // forget allocation failures, let them segfault.
void operator delete(void * p) { free(p); }
extern "C" void __cxa_pure_virtual() { while(1); }


//TODO: delete
struct mem ReadWholeFile(LPCWSTR filename)
{
	return file::read(filename);
}

bool WriteWholeFile(LPCWSTR filename, struct mem data)
{
	return filewrite::write(filename, data);
}

bool WriteWholeFileWithHeader(LPCWSTR filename, struct mem header, struct mem data)
{
	filewrite* f = filewrite::create(filename);
	if (!f) return false;
	bool ret = (f->append(header.ptr, 512) && f->append(data.ptr, data.len)); // do not use header.len, that'd prepend the entire file
	delete f;
	return ret;
}

void FreeFileMemory(struct mem mem)
{
	free(mem.ptr);
}





LPWSTR GetExtension(LPCWSTR fname)
{
	LPWSTR ptr1=(LPWSTR)fname;
	LPWSTR ptr2;
	ptr2=wcsrchr(ptr1, '/'); if (ptr2) ptr1=ptr2;
#ifdef FLIPS_WINDOWS
	ptr2=wcsrchr(ptr1, '\\'); if (ptr2) ptr1=ptr2;
#endif
	ptr2=wcsrchr(ptr1, '.'); if (ptr2) ptr1=ptr2;
	if (*ptr1=='.') return ptr1;
	else return wcsrchr(ptr1, '\0');
}

LPWSTR GetBaseName(LPCWSTR fname)
{
	LPWSTR ptr1=(LPWSTR)fname;
	LPWSTR ptr2;
	ptr2=wcsrchr(ptr1, '/'); if (ptr2) ptr1=ptr2+1;
#ifdef FLIPS_WINDOWS
	ptr2=wcsrchr(ptr1, '\\'); if (ptr2) ptr1=ptr2+1;
#endif
	return ptr1;
}

bool forceKeepHeader=false;

#ifndef FLIPS_CLI
bool guiActive=false;
#endif


struct mem file::read()
{
	struct mem out;
	out.len = len();
	out.ptr = (uint8_t*)malloc(out.len);
	if (!read(out.ptr, 0, out.len))
	{
		free(out.ptr);
		struct mem err = {NULL, 0};
		return err;
	}
	return out;
}

struct mem file::read(LPCWSTR filename)
{
	struct mem err = {NULL, 0};
	file* f = file::create(filename);
	if (!f) return err;
	struct mem ret = f->read();
	delete f;
	return ret;
}

bool filewrite::write(LPCWSTR filename, struct mem data)
{
	filewrite* f = filewrite::create(filename);
	if (!f) return false;
	bool ret = f->append(data.ptr, data.len);
	delete f;
	return ret;
}


class fileheader : public file {
	file* child;
	
public:
	fileheader(file* child) : child(child) {}
	
	size_t len() { return child->len()-512; }
	bool read(uint8_t* target, size_t start, size_t len) { return child->read(target, start+512, len); }
	
	~fileheader() { delete child; }
};




const struct errorinfo ipserrors[]={
		{ el_ok, NULL },//ips_ok
		{ el_unlikelythis, "The patch was applied, but is most likely not intended for this ROM." },//ips_notthis
		{ el_unlikelythis, "The patch was applied, but did nothing. You most likely already had the output file of this patch." },//ips_thisout
		{ el_warning, "The patch was applied, but appears scrambled or malformed." },//ips_suspicious
		{ el_broken, "The patch is broken and can't be used." },//ips_invalid
		
		{ el_broken, "The IPS format does not support files larger than 16MB." },//ips_16MB
		{ el_warning, "The files are identical! The patch will do nothing." },//ips_identical
	};

const struct errorinfo bpserrors[]={
		{ el_ok, NULL },//bps_ok,
		{ el_notthis, "That's the output file already." },//bps_to_output
		{ el_notthis, "This patch is not intended for this ROM." },//bps_not_this
		{ el_broken, "This patch is broken and can't be used." },//bps_broken
		{ el_broken, "Couldn't read input patch. What exactly are you doing?" },//bps_io
		
		{ el_warning, "The files are identical! The patch will do nothing." },//bps_identical
		{ el_broken, "These files are too big for this program to handle." },//bps_too_big
		{ el_broken, "These files are too big for this program to handle." },//bps_out_of_mem (same message as above, it's accurate for both.)
		{ el_broken, "Patch creation was canceled." },//bps_canceled
	};

LPCWSTR GetManifestName(LPCWSTR romname)
{
	//static WCHAR manifestname[MAX_PATH];
	//wcscpy(manifestname, romname);
	//LPWSTR manifestext=GetExtension(manifestname);
	//if (!manifestext) manifestext=wcschr(manifestname, '\0');
	//wcscpy(manifestext, TEXT(".bml"));
	//return manifestname;
	
	static WCHAR * manifestname=NULL;
	if (manifestname) free(manifestname);
	manifestname=(WCHAR*)malloc((wcslen(romname)+1+4)*sizeof(WCHAR));
	wcscpy(manifestname, romname);
	LPWSTR manifestext=GetExtension(manifestname);
	if (manifestext) wcscpy(manifestext, TEXT(".bml"));
	return manifestname;
}

enum patchtype IdentifyPatch(file* patch)
{
	size_t len = patch->len();
	uint8_t data[16];
	if (len>16) len=16;
	
	patch->read(data, 0, len);
	if (len>=5 && !memcmp(data, "PATCH", 5)) return ty_ips;
	if (len>=4 && !memcmp(data, "BPS1", 4)) return ty_bps;
	if (len>=4 && !memcmp(data, "UPS1", 4)) return ty_ups;
	return ty_null;
}

enum {
	ch_crc32,
	ch_last
};
struct checkmap {
	uint8_t* sum;
	LPWSTR name;
};
static struct checkmap * checkmap[ch_last]={NULL};
static uint32_t checkmap_len[ch_last]={0};
static const uint8_t checkmap_sum_size[]={ 4 };
static const uint8_t checkmap_sum_size_max = 4;

static LPCWSTR FindRomForSum(int type, void* sum)
{
//printf("SE.CRC=%.8X\n",*(uint32_t*)sum);
	for (unsigned int i=0;i<checkmap_len[type];i++)
	{
		if (!memcmp(checkmap[type][i].sum, sum, checkmap_sum_size[type]))
		{
			return checkmap[type][i].name;
		}
	}
	return NULL;
}

static void AddRomForSum(int type, void* sum, LPCWSTR filename)
{
//printf("AD.CRC=%.8X:%ls\n",*(uint32_t*)sum,filename);
	if (FindRomForSum(type, sum)) return;
	
	int ch_pos=(checkmap_len[type]++);
	
	if (!(ch_pos&(ch_pos+1)))
	{
		checkmap[type]=(struct checkmap*)realloc(checkmap[type], sizeof(struct checkmap)*((ch_pos+1)*2));
	}
	
	struct checkmap* item=&checkmap[type][ch_pos];
	item->sum=(uint8_t*)malloc(checkmap_sum_size[type]);
	memcpy(item->sum, sum, checkmap_sum_size[type]);
	item->name=wcsdup(filename);
}

struct mem GetRomList()
{
	struct mem out={NULL, 0};
	for (unsigned int type=0;type<ch_last;type++)
	{
		out.len+=sizeof(uint32_t);
		for (uint32_t i=0;i<checkmap_len[type];i++)
		{
			out.len+=sizeof(uint8_t);
			out.len+=sizeof(uint16_t);
			out.len+=checkmap_sum_size[type];
			out.len+=sizeof(WCHAR)*wcslen(checkmap[type][i].name);
		}
	}
	out.ptr=(uint8_t*)malloc(out.len);
	uint8_t* data=out.ptr;
	for (unsigned int type=0;type<ch_last;type++)
	{
#define write(ptr, size) \
	memcpy(data, ptr, size); \
	data+=size
#define write_obj(obj) write(&obj, sizeof(obj))
		write_obj(checkmap_len[type]);
		for (uint32_t i=0;i<checkmap_len[type];i++)
		{
			write_obj(checkmap_sum_size[type]);
			uint16_t len=sizeof(WCHAR)*wcslen(checkmap[type][i].name);
			write_obj(len);
			
			write(checkmap[type][i].sum, checkmap_sum_size[type]);
			write(checkmap[type][i].name, len);
		}
#undef write
#undef write_obj
	}
	return out;
}

void FreeRomList(struct mem data)
{
	free(data.ptr);
}

void SetRomList(struct mem data)
{
	for (int type=0;type<ch_last;type++)
	{
#define read(target, bytes) \
	if (bytes > data.len) return; \
	memcpy(target, data.ptr, bytes); \
	data.ptr += bytes; \
	data.len -= bytes
#define read_discard(bytes) \
	if (bytes > data.len) return; \
	data.ptr += bytes; \
	data.len -= bytes
		uint32_t count;
		read(&count, sizeof(count));
		checkmap[type]=(struct checkmap*)malloc(sizeof(struct checkmap)*count*2);//overallocate so I won't need to round the count
		
		while (count--)
		{
			uint8_t hashlen;
			read(&hashlen, sizeof(hashlen));
			uint16_t strlen;
			read(&strlen, sizeof(strlen));
			if (hashlen==checkmap_sum_size[type])
			{
				if (data.len < hashlen+strlen) return;
				
				struct checkmap* item=&checkmap[type][checkmap_len[type]++];
				item->sum=(uint8_t*)malloc(checkmap_sum_size[type]);
				read(item->sum, hashlen);
				item->name=(WCHAR*)malloc(strlen+sizeof(WCHAR));
				read(item->name, strlen);
				memset((uint8_t*)item->name + strlen, 0, sizeof(WCHAR));
			}
			else
			{
				read_discard(hashlen);
				read_discard(strlen);
			}
		}
#undef read
	}
}

LPCWSTR FindRomForPatch(file* patch, bool * possibleToFind)
{
	if (possibleToFind) *possibleToFind=false;
	enum patchtype patchtype=IdentifyPatch(patch);
	if (patchtype==ty_bps)
	{
		struct bpsinfo info = bps_get_info(patch, false);
		if (info.error) return NULL;
		if (possibleToFind) *possibleToFind=true;
		return FindRomForSum(ch_crc32, &info.crc_in);
	}
	//UPS has checksums too, but screw UPS. Nobody cares.
	return NULL;
}

void AddToRomList(file* patch, LPCWSTR path)
{
	enum patchtype patchtype=IdentifyPatch(patch);
	if (patchtype==ty_bps)
	{
		struct bpsinfo info = bps_get_info(patch, false);
		if (info.error) return;
		AddRomForSum(ch_crc32, &info.crc_in, path);
	}
}

void DeleteRomFromList(LPCWSTR path)
{
	for (unsigned int type=0;type<ch_last;type++)
	{
		for (unsigned int i=0;i<checkmap_len[type];i++)
		{
			if (!wcscmp(checkmap[type][i].name, path))
			{
				free(checkmap[type][i].name);
				free(checkmap[type][i].sum);
				memmove(&checkmap[type][i], &checkmap[type][i+1], sizeof(struct checkmap)*(checkmap_len[type]-1 - i));
				i--;
				checkmap_len[type]--;
			}
		}
	}
}

static struct errorinfo error(errorlevel level, const char * text)
{
	struct errorinfo errinf = { level, text };
	return errinf;
}

struct errorinfo ApplyPatchMem2(file* patch, struct mem inrom, bool verifyinput, bool removeheader,
                                LPCWSTR outromname, struct manifestinfo * manifestinfo)
{
	struct mem patchmem = patch->read(); // There's no real reason to remove this, no patcher knows how to handle these file objects.
	
	enum patchtype patchtype=IdentifyPatch(patch);
	struct errorinfo errinf;
	removeheader=(removeheader && patchtype==ty_bps);
	if (removeheader)
	{
		inrom.ptr+=512;
		inrom.len-=512;
	}
	struct mem outrom={NULL,0};
	struct mem manifest={NULL,0};
	
	errinf=error(el_broken, "Unknown patch format.");
	if (patchtype==ty_bps)
	{
		errinf=bpserrors[bps_apply(patchmem, inrom, &outrom, &manifest, !verifyinput)];
		if (!verifyinput && outrom.ptr) errinf.level=el_warning;
	}
	if (patchtype==ty_ips) errinf=ipserrors[ips_apply(patchmem, inrom, &outrom)];
	if (patchtype==ty_ups) errinf=bpserrors[ups_apply(patchmem, inrom, &outrom)];
	if (errinf.level==el_ok) errinf.description="The patch was applied successfully!";
	
	struct manifestinfo defmanifestinfo={true,false,NULL};
	if (!manifestinfo) manifestinfo=&defmanifestinfo;
	if (manifestinfo->use)
	{
		if (manifest.ptr)
		{
			LPCWSTR manifestname;
			if (manifestinfo->name) manifestname=manifestinfo->name;
			else manifestname=GetManifestName(outromname);
			if (!WriteWholeFile(manifestname, manifest) && manifestinfo->required)
			{
				if (errinf.level==el_ok) errinf=error(el_warning, "The patch was applied, but the manifest could not be created.");
			}
		}
		else if (manifestinfo->required && errinf.level==el_ok) errinf=error(el_warning, "The patch was applied, but there was no manifest present.");
	}
	
	if (removeheader)
	{
		inrom.ptr-=512;
		inrom.len+=512;
		if (errinf.level<el_notthis)
		{
			if (!WriteWholeFileWithHeader(outromname, inrom, outrom)) errinf=error(el_broken, "Couldn't write ROM. Are you on a read-only medium?");
		}
	}
	else if (errinf.level<el_notthis)
	{
		if (!WriteWholeFile(outromname, outrom)) errinf=error(el_broken, "Couldn't write ROM. Are you on a read-only medium?");
	}
	free(outrom.ptr);
	free(patchmem.ptr);
	
	if (errinf.level==el_notthis && removeheader)
	{
		errinf=ApplyPatchMem2(patch, inrom, verifyinput, false, outromname, manifestinfo);
		if (errinf.level==el_ok)
		{
			errinf=error(el_warning, "The patch was applied, but it was created from a headered ROM, which may not work for everyone.");
		}
	}
	return errinf;
}

bool shouldRemoveHeader(LPCWSTR romname, size_t romlen)
{
	LPWSTR romext=GetExtension(romname);
	return ((romlen&0x7FFF)==512 &&
	        (!wcsicmp(romext, TEXT(".smc")) || !wcsicmp(romext, TEXT(".sfc"))) &&
	        !forceKeepHeader);
}

struct errorinfo ApplyPatchMem(file* patch, LPCWSTR inromname, bool verifyinput,
                               LPCWSTR outromname, struct manifestinfo * manifestinfo, bool update_rom_list)
{
	struct mem inrom=ReadWholeFile(inromname);
	if (!inrom.ptr)
	{
		if (update_rom_list) DeleteRomFromList(inromname);
		return error(el_broken, "Couldn't read ROM. What exactly are you doing?");
	}
	struct errorinfo errinf=ApplyPatchMem2(patch, inrom, verifyinput, shouldRemoveHeader(inromname, inrom.len), outromname, manifestinfo);
	if (update_rom_list && errinf.level==el_ok) AddToRomList(patch, inromname);
	FreeFileMemory(inrom);
	return errinf;
}

struct errorinfo ApplyPatch(LPCWSTR patchname, LPCWSTR inromname, bool verifyinput,
                            LPCWSTR outromname, struct manifestinfo * manifestinfo, bool update_rom_list)
{
	file* patch = file::create(patchname);
	if (!patch)
	{
		return error(el_broken, "Couldn't read input patch. What exactly are you doing?");
	}
	struct errorinfo errinf=ApplyPatchMem(patch, inromname, verifyinput, outromname, manifestinfo, update_rom_list);
	delete patch;
	return errinf;
}


char bpsdProgStr[24];
int bpsdLastPromille=-1;

bool bpsdeltaGetProgress(size_t done, size_t total)
{
	if (total<1000) total=1000;//avoid div by zero
	int promille=done/(total/1000);//don't set this to done*1000/total, it'd just give overflows on huge stuff. 100% is handled later
	if (promille==bpsdLastPromille) return false;
	bpsdLastPromille=promille;
	if (promille>=1000) return false;
	strcpy(bpsdProgStr, "Please wait... ");
	bpsdProgStr[15]='0'+promille/100;
	int digit1=((promille<100)?15:16);
	bpsdProgStr[digit1+0]='0'+promille/10%10;
	bpsdProgStr[digit1+1]='.';
	bpsdProgStr[digit1+2]='0'+promille%10;
	bpsdProgStr[digit1+3]='%';
	bpsdProgStr[digit1+4]='\0';
	return true;
}

bool bpsdeltaProgressCLI(void* userdata, size_t done, size_t total)
{
	if (!bpsdeltaGetProgress(done, total)) return true;
	fputs(bpsdProgStr, stdout);
	putchar('\r');
	fflush(stdout);
	return true;
}

struct errorinfo CreatePatchToMem(LPCWSTR inromname, LPCWSTR outromname, enum patchtype patchtype,
                                  struct manifestinfo * manifestinfo, struct mem * patchmem)
{
	//pick roms
	file* roms[2]={NULL, NULL};
	for (int i=0;i<2;i++)
	{
		LPCWSTR romname=((i==0)?inromname:outromname);
		roms[i] = file::create(romname);
		if (!roms[i])
		{
			return error(el_broken, "Couldn't read this ROM. What exactly are you doing?");
		}
		if (shouldRemoveHeader(romname, roms[i]->len()) && (patchtype==ty_bps || patchtype==ty_bps_linear || patchtype==ty_bps_moremem))
		{
			roms[i] = new fileheader(roms[i]);
		}
	}
	
	struct mem manifest={NULL,0};
	struct errorinfo manifesterr={el_ok, NULL};
	struct manifestinfo defmanifestinfo={true,false,NULL};
	if (!manifestinfo) manifestinfo=&defmanifestinfo;
	if (patchtype==ty_bps || patchtype==ty_bps_linear)
	{
		LPCWSTR manifestname;
		if (manifestinfo->name) manifestname=manifestinfo->name;
		else manifestname=GetManifestName(outromname);
		manifest=ReadWholeFile(manifestname);
		if (!manifest.ptr) manifesterr=error(el_warning, "The patch was created, but the manifest could not be read.");
	}
	else manifesterr=error(el_warning, "The patch was created, but this patch format does not support manifests.");
	
	struct errorinfo errinf={ el_broken, "Unknown patch format." };
	if (patchtype==ty_ips)
	{
		struct mem rommem[2]={ roms[0]->read(), roms[1]->read() };
		errinf=ipserrors[ips_create(rommem[0], rommem[1], patchmem)];
		free(rommem[0].ptr);
		free(rommem[1].ptr);
	}
	if (patchtype==ty_bps || patchtype==ty_bps_moremem)
	{
#ifndef FLIPS_CLI
		if (guiActive)
		{
			bpsdeltaBegin();
			errinf=bpserrors[bps_create_delta(roms[0], roms[1], manifest, patchmem, bpsdeltaProgress, NULL, (patchtype==ty_bps_moremem))];
			bpsdeltaEnd();
		}
		else
#endif
		{
			errinf=bpserrors[bps_create_delta(roms[0], roms[1], manifest, patchmem, bpsdeltaProgressCLI, NULL, (patchtype==ty_bps_moremem))];
		}
	}
	if (patchtype==ty_bps_linear)
	{
		struct mem rommem[2]={ roms[0]->read(), roms[1]->read() };
		errinf=bpserrors[bps_create_linear(rommem[0], rommem[1], manifest, patchmem)];
		free(rommem[0].ptr);
		free(rommem[1].ptr);
	}
	FreeFileMemory(manifest);
	if (errinf.level==el_ok) errinf.description="The patch was created successfully!";
	
	if (manifestinfo->required && errinf.level==el_ok && manifesterr.level!=el_ok) errinf=manifesterr;
	
	if (errinf.level==el_ok && roms[0]->len() > roms[1]->len())
	{
		errinf=error(el_warning, "The patch was created, but the input ROM is larger than the "
		                         "output ROM. Double check whether you've gotten them backwards.");
	}
	
	delete roms[0];
	delete roms[1];
	
	return errinf;
}

struct errorinfo CreatePatch(LPCWSTR inromname, LPCWSTR outromname, enum patchtype patchtype,
                             struct manifestinfo * manifestinfo, LPCWSTR patchname)
{
	struct mem patch={NULL,0};
	struct errorinfo errinf = CreatePatchToMem(inromname, outromname, patchtype, manifestinfo, &patch);
	
	if (errinf.level<el_notthis)
	{
		if (!WriteWholeFile(patchname, patch)) errinf=error(el_broken, "Couldn't write ROM. Are you on a read-only medium?");
	}
	if (patch.ptr) free(patch.ptr);
	return errinf;
}

int patchinfo(LPCWSTR patchname)
{
	GUIClaimConsole();
	
	file* patch = file::create(patchname);
	if (!patch)
	{
		puts("Couldn't read file");
		return el_broken;
	}
	
	enum patchtype patchtype=IdentifyPatch(patch);
	if (patchtype==ty_bps)
	{
		struct bpsinfo info = bps_get_info(patch, false);
		if (info.error)
		{
			puts(bpserrors[info.error].description);
			return bpserrors[info.error].level;
		}
		
#ifndef FLIPS_CLI
		GUILoadConfig();
		LPCWSTR inromname = FindRomForPatch(patch, NULL);
#else
		LPCWSTR inromname = NULL;
#endif
#ifdef FLIPS_WINDOWS
#define z "I"
#else
#define z "z"
#endif
		printf("Input ROM: %" z "u bytes, CRC32 %.8X", info.size_in, info.crc_in);
		if (inromname) wprintf(TEXT(", %s"), inromname);
		puts("");
		
		printf("Output ROM: %" z "u bytes, CRC32 %.8X\n", info.size_out, info.crc_out);
		//floating point may lose a little precision, but it's easier than dodging overflows, and this
		//is the output of inaccurate heuristics anyways, losing a little more makes no difference.
		//Windows MulDiv could also work, but it's kinda nonportable.
		//printf("Change index: %i / 1000\n", (int)(info.change_num / (float)info.change_denom * 1000));
		
		return 0;
	}
	puts("No information available for this patch type");
	return el_broken;
}



void usage()
{
	GUIClaimConsole();
	puts(
	// 12345678901234567890123456789012345678901234567890123456789012345678901234567890
		"usage:\n"
		"   "
#ifndef FLIPS_CLI
       "flips\n"
		"or flips patch.ips\n"
		"or "
#endif
		   "flips [--apply] [--exact] patch.bps rom.smc [outrom.smc]\n"
		"or flips [--create] [--exact] [--bps | --bps-linear | --ips] clean.smc\n"
		"  hack.smc [patch.bps]\n"
#ifndef FLIPS_CLI
		"(for scripting, only the latter two are sensible)\n"
#endif
		"(patch.ips is valid in all cases patch.bps is)\n"
		"\n"
	// 12345678901234567890123456789012345678901234567890123456789012345678901234567890
		"options:\n"
		"-a --apply: apply patch (default if given two arguments)\n"
		"-c --create: create patch (default if given three arguments)\n"
		"-I --info: BPSes contain information about input and output roms, print it\n"
		//"  also estimates how much of the source file is retained\n"
		//"  anything under 400 is fine, anything over 600 should be treated with suspicion\n"
		"-i --ips, -b -B --bps --bps-delta, --bps-linear, --bps-delta-moremem:\n"
		"  create this patch format instead of guessing based on file extension\n"
		"  ignored when applying\n"
		" bps formats:\n"
		"  delta is the recommended one; it's a good balance between creation time and\n"
		"    patch size\n"
		"    -b and -B both refer to this, for backwards compatibility reasons\n"
		"  delta-moremem is usually slightly faster than delta, but uses about twice\n"
		"    as much memory; it gives identical patches to delta\n"
		"  linear is the fastest, but tends to give pretty big patches\n"
		"--exact: do not remove SMC headers when applying or creating a BPS patch\n"
		"  (ignored for IPS)\n"
		"--ignore-checksum: accept checksum mismatches when applying a BPS patch\n"
		"-m or --manifest: emit or insert a manifest file as romname.bml\n"
		"  (valid only for BPS)\n"
		"-mfilename or --manifest=filename: emit or insert a manifest file exactly here\n"
		"-h -? --help: show this information\n"
		"-v --version: show application version\n"
	// 12345678901234567890123456789012345678901234567890123456789012345678901234567890
		);
	exit(0);
}


int flipsmain(int argc, WCHAR * argv[])
{
	enum patchtype patchtype=ty_null;
	enum { a_default, a_apply_filepicker, a_apply_given, a_create, a_info } action=a_default;
	int numargs=0;
	LPCWSTR arg[3]={NULL,NULL,NULL};
	bool hasFlags=false;
	
	bool ignoreChecksum=false;
	
	struct manifestinfo manifestinfo={false, false, NULL};
//	 {
//	bool use;
//	bool required;
//	LPCWSTR name;
//	bool success;
//};
	for (int i=1;i<argc;i++)
	{
		if (argv[i][0]=='-')
		{
			hasFlags=true;
			if(0);
			else if (!wcscmp(argv[i], TEXT("--apply")) || !wcscmp(argv[i], TEXT("-a")))
			{
				if (action==a_default) action=a_apply_given;
				else usage();
			}
			else if (!wcscmp(argv[i], TEXT("--create")) || !wcscmp(argv[i], TEXT("-c")))
			{
				if (action==a_default) action=a_create;
				else usage();
			}
			else if (!wcscmp(argv[i], TEXT("--info")) || !wcscmp(argv[i], TEXT("-I")))
			{
				if (action==a_default) action=a_info;
				else usage();
			}
			else if (!wcscmp(argv[i], TEXT("--ips")) || !wcscmp(argv[i], TEXT("-i")))
			{
				if (patchtype==ty_null) patchtype=ty_ips;
				else usage();
			}
			else if (!wcscmp(argv[i], TEXT("--bps")) || !wcscmp(argv[i], TEXT("--bps-delta")) ||
			         !wcscmp(argv[i], TEXT("-b")) || !wcscmp(argv[i], TEXT("-B")))
			{
				if (patchtype==ty_null) patchtype=ty_bps;
				else usage();
			}
			else if (!wcscmp(argv[i], TEXT("--bps-delta-moremem")))
			{
				if (patchtype==ty_null) patchtype=ty_bps_moremem;
				else usage();
			}
			else if (!wcscmp(argv[i], TEXT("--bps-linear")))
			{
				if (patchtype==ty_null) patchtype=ty_bps_linear;
				else usage();
			}
			else if (!wcscmp(argv[i], TEXT("--exact"))) // no short form
			{
				if (forceKeepHeader) usage();
				forceKeepHeader=true;
			}
			else if (!wcscmp(argv[i], TEXT("--ignore-checksum")))
			{
				if (ignoreChecksum) usage();
				ignoreChecksum=true;
			}
			else if (!wcscmp(argv[i], TEXT("--manifest")) || !wcscmp(argv[i], TEXT("-m")))
			{
				manifestinfo.use=true;
				manifestinfo.required=true;
			}
			else if (!wcsncmp(argv[i], TEXT("--manifest="), wcslen(TEXT("--manifest="))))
			{
				manifestinfo.use=true;
				manifestinfo.required=true;
				manifestinfo.name=argv[i]+wcslen(TEXT("--manifest="));
			}
			else if (!wcsncmp(argv[i], TEXT("-m"), wcslen(TEXT("-m"))))
			{
				manifestinfo.use=true;
				manifestinfo.required=true;
				manifestinfo.name=argv[i]+wcslen(TEXT("--m"));
			}
			else if (!wcscmp(argv[i], TEXT("--version")) || !wcscmp(argv[i], TEXT("-v")))
			{
				GUIClaimConsole();
				puts(flipsversion);
				return 0;
			}
			else if (!wcscmp(argv[i], TEXT("--help")) || !wcscmp(argv[i], TEXT("-h")) || !wcscmp(argv[i], TEXT("-?"))) usage();
			else usage();
		}
		else
		{
			if (numargs==3) usage();
			arg[numargs++]=argv[i];
		}
	}
	if (action==a_default)
	{
		if (numargs==0) action=a_default;
		if (numargs==1) action=a_apply_filepicker;
		if (numargs==2) action=a_apply_given;
		if (numargs==3) action=a_create;
	}
	switch (action)
	{
		case a_default:
		{
			if (numargs!=0 || hasFlags) usage();
#ifndef FLIPS_CLI
			guiActive=true;
			return GUIShow(NULL);
#else
			usage();
#endif
		}
		case a_apply_filepicker:
		{
			if (numargs!=1 || hasFlags) usage();
#ifndef FLIPS_CLI
			guiActive=true;
			return GUIShow(arg[0]);
#else
			usage();
#endif
		}
		case a_apply_given:
		{
			if (numargs!=2 && numargs!=3) usage();
			GUIClaimConsole();
			struct errorinfo errinf=ApplyPatch(arg[0], arg[1], !ignoreChecksum, arg[2]?arg[2]:arg[1], &manifestinfo, false);
			puts(errinf.description);
			return errinf.level;
		}
		case a_create:
		{
			if (numargs!=2 && numargs!=3) usage();
			GUIClaimConsole();
			if (!arg[2])
			{
				if (patchtype==ty_null)
				{
					puts("Error: Unknown patch type.");
					return el_broken;
				}
				LPWSTR arg2=(WCHAR*)malloc(sizeof(WCHAR)*(wcslen(arg[1])+4+1));
				arg[2]=arg2;
				wcscpy(arg2, arg[1]);
				GetExtension(arg2)[0]='\0';
				if (patchtype==ty_ips) wcscat(arg2, TEXT(".ips"));
				if (patchtype==ty_bps) wcscat(arg2, TEXT(".bps"));
				if (patchtype==ty_bps_linear) wcscat(arg2, TEXT(".bps"));
			}
			if (patchtype==ty_null)
			{
				LPCWSTR patchext=GetExtension(arg[2]);
				if (!*patchext)
				{
					puts("Error: Unknown patch type.");
					return el_broken;
				}
				else if (!wcsicmp(patchext, TEXT(".ips"))) patchtype=ty_ips;
				else if (!wcsicmp(patchext, TEXT(".bps"))) patchtype=ty_bps;
				else
				{
					wprintf(TEXT("Error: Unknown patch type (%s)\n"), patchext);
					return el_broken;
				}
			}
			struct errorinfo errinf=CreatePatch(arg[0], arg[1], patchtype, &manifestinfo, arg[2]);
			puts(errinf.description);
			return errinf.level;
		}
		case a_info:
		{
			if (numargs!=1) usage();
			return patchinfo(arg[0]);
		}
	}
	return 99;//doesn't happen
}








