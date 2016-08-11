/*******************************************************************************
*
*  (C) COPYRIGHT AUTHORS, 2016
*
*  TITLE:       FUZZNTOS.C
*
*  VERSION:     1.00
*
*  DATE:        11 July 2016
*
*  Service table fuzzing routines.
*
* THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
* ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED
* TO THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
* PARTICULAR PURPOSE.
*
*******************************************************************************/
#include "main.h"
#include "fuzz.h"

const BYTE  KiSystemServiceStartPattern[] = { 0x45, 0x33, 0xC9, 0x44, 0x8B, 0x05 };

#define MAX_FUZZTHREADS     32

RAW_SERVICE_TABLE	g_Sdt;
HANDLE              g_FuzzingThreads[MAX_FUZZTHREADS];
BADCALLS            g_NtOsSyscallBlacklist;

/*
* find_kiservicetable
*
* Purpose:
*
* Locate KiServiceTable in mapped ntoskrnl copy.
*
*/
BOOL find_kiservicetable(
    ULONG_PTR			MappedImageBase,
    PRAW_SERVICE_TABLE	ServiceTable
)
{
    PIMAGE_NT_HEADERS	nthdr = RtlImageNtHeader((PVOID)MappedImageBase);
    ULONG				c, p, SizeLimit =
        nthdr->OptionalHeader.SizeOfImage - sizeof(KiSystemServiceStartPattern);

    p = 0;
    for (c = 0; c < SizeLimit; c++)
        if (RtlCompareMemory(
            (PVOID)(MappedImageBase + c),
            KiSystemServiceStartPattern,
            sizeof(KiSystemServiceStartPattern)) == sizeof(KiSystemServiceStartPattern))
        {
            p = c;
            break;
        }

    if (p == 0)
        return FALSE;

    p += 3;
    c = *((PULONG)(MappedImageBase + p + 3)) + 7 + p;
    ServiceTable->CountOfEntries = *((PULONG)(MappedImageBase + c));
    p += 7;
    c = *((PULONG)(MappedImageBase + p + 3)) + 7 + p;
    ServiceTable->StackArgumentTable = (PBYTE)MappedImageBase + c;
    p += 7;
    c = *((PULONG)(MappedImageBase + p + 3)) + 7 + p;
    ServiceTable->ServiceTable = (LPVOID *)(MappedImageBase + c);

    return TRUE;
}

/*
* PELoaderGetProcNameBySDTIndex
*
* Purpose:
*
* Return name of service from ntdll by given syscall id.
*
*/
PCHAR PELoaderGetProcNameBySDTIndex(
    ULONG_PTR	MappedImageBase,
    ULONG		SDTIndex
)
{

    PIMAGE_NT_HEADERS			nthdr = RtlImageNtHeader((PVOID)MappedImageBase);
    PIMAGE_EXPORT_DIRECTORY		ExportDirectory;

    ULONG_PTR	ExportDirectoryOffset;
    PULONG		NameTableBase;
    PUSHORT		NameOrdinalTableBase;
    PULONG		Addr;
    PBYTE		pfn;
    ULONG		c;

    ExportDirectoryOffset =
        nthdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;

    if (ExportDirectoryOffset == 0)
        return NULL;

    ExportDirectory = (PIMAGE_EXPORT_DIRECTORY)(MappedImageBase + ExportDirectoryOffset);
    NameTableBase = (PULONG)(MappedImageBase + (ULONG)ExportDirectory->AddressOfNames);
    NameOrdinalTableBase = (PUSHORT)(MappedImageBase + (ULONG)ExportDirectory->AddressOfNameOrdinals);
    Addr = (PULONG)(MappedImageBase + (ULONG)ExportDirectory->AddressOfFunctions);

    for (c = 0; c < ExportDirectory->NumberOfNames; c++) {
        pfn = (PBYTE)(MappedImageBase + Addr[NameOrdinalTableBase[c]]);
        if (*((PULONG)pfn) == 0xb8d18b4c)
            if (*((PULONG)(pfn + 4)) == SDTIndex)
                return (PCHAR)(MappedImageBase + NameTableBase[c]);
    }

    return NULL;
}

/*
* fuzzntos_proc
*
* Purpose:
*
* Handler for fuzzing thread.
*
*/
DWORD WINAPI fuzzntos_proc(
    PVOID Parameter
)
{
    BOOL   bSkip = FALSE;
    ULONG  c, r;
    PCHAR  Name1;
    CHAR   textbuf[512];
    ULONG_PTR NtdllImage;

    NtdllImage = (ULONG_PTR)GetModuleHandle(TEXT("ntdll.dll"));
    if (NtdllImage == 0)
        return 0;

    for (c = 0; c < g_Sdt.CountOfEntries; c++) {
        Name1 = PELoaderGetProcNameBySDTIndex(NtdllImage, c);

        _strcpy_a(textbuf, "tid #");
        ultostr_a((ULONG)(ULONG_PTR)Parameter, _strend_a(textbuf));

        _strcat_a(textbuf, "\targs(stack): ");
        ultostr_a(g_Sdt.StackArgumentTable[c] / 4, _strend_a(textbuf));

        _strcat_a(textbuf, "\tsid ");
        ultostr_a(c, _strend_a(textbuf));
        _strcat_a(textbuf, "\tname:");
        if (Name1 != NULL) {
            _strcat_a(textbuf, Name1);
        }
        else {          
            _strcat_a(textbuf, "#noname#");
        }

        bSkip = SyscallBlacklisted(Name1, &g_NtOsSyscallBlacklist);
        if (bSkip) {
            _strcat_a(textbuf, " - skip, blacklist");
        }
        _strcat_a(textbuf, "\r\n");
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), textbuf, (DWORD)_strlen_a(textbuf), &r, NULL);

        if (bSkip)
            continue;

        for (r = 0; r < 64 * 1024; r++)
            gofuzz(c, g_Sdt.StackArgumentTable[c]);
    }
    return 0;
}

/*
* fuzz_ntos
*
* Purpose:
*
* Launch ntos service table fuzzing using MAX_FUZZTHREADS number of threads.
*
*/
void fuzz_ntos()
{
    WCHAR       szBuffer[MAX_PATH * 2];
    ULONG_PTR   KernelImage = 0;
    ULONG       c, r;

    RtlSecureZeroMemory(szBuffer, sizeof(szBuffer));
    if (GetSystemDirectory(szBuffer, MAX_PATH)) {
        _strcat(szBuffer, TEXT("\\ntoskrnl.exe"));
        KernelImage = (ULONG_PTR)LoadLibraryEx(szBuffer, NULL, 0);
    }

    RtlSecureZeroMemory(&g_NtOsSyscallBlacklist, sizeof(g_NtOsSyscallBlacklist));
    ReadBlacklistCfg(&g_NtOsSyscallBlacklist, CFG_FILE, "ntos");

    while (KernelImage != 0) {

        if (!find_kiservicetable(KernelImage, &g_Sdt))
            break;

        RtlSecureZeroMemory(g_FuzzingThreads, sizeof(g_FuzzingThreads));

        force_priv();

        for (c = 0; c < MAX_FUZZTHREADS; c++) {
            g_FuzzingThreads[c] = CreateThread(NULL, 0, fuzzntos_proc, (LPVOID)(ULONG_PTR)c, 0, &r);
        }

        WaitForMultipleObjects(MAX_FUZZTHREADS, g_FuzzingThreads, TRUE, INFINITE);

        for (c = 0; c < MAX_FUZZTHREADS; c++) {
            CloseHandle(g_FuzzingThreads[c]);
        }
        break;
    }
}