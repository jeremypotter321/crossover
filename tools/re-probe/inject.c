/*
 * Minimal injector for the re-probe DLL.
 *
 * Same technique as the mod's own launcher (CreateProcess suspended ->
 * VirtualAllocEx -> WriteProcessMemory -> CreateRemoteThread on LoadLibraryA ->
 * resume), but it takes the game and DLL paths as arguments instead of
 * hardcoding them, and it reports what actually happened.
 *
 * The mod's launcher returns 0 whether or not LoadLibrary succeeded inside the
 * target, which makes a failed injection look like a success. This one reads the
 * remote thread's exit code -- that is LoadLibrary's return value, so zero means
 * the DLL genuinely failed to load.
 *
 *   usage: inject.exe <game.exe> <probe.dll>
 */

#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    char dllPath[MAX_PATH];
    SIZE_T len;
    LPVOID remote;
    HMODULE k32;
    LPTHREAD_START_ROUTINE loadLibrary;
    HANDLE thread;
    DWORD exitCode = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <game.exe> <probe.dll>\n", argv[0]);
        return 2;
    }

    if (!GetFullPathNameA(argv[2], sizeof dllPath, dllPath, NULL)) {
        fprintf(stderr, "GetFullPathName failed: %lu\n", GetLastError());
        return 1;
    }
    printf("[*] dll   : %s\n", dllPath);
    printf("[*] game  : %s\n", argv[1]);

    if (!CreateProcessA(NULL, argv[1], NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    printf("[*] started suspended, pid %lu\n", pi.dwProcessId);

    len = strlen(dllPath) + 1;
    remote = VirtualAllocEx(pi.hProcess, NULL, len, MEM_COMMIT, PAGE_READWRITE);
    if (!remote) {
        fprintf(stderr, "VirtualAllocEx failed: %lu\n", GetLastError());
        goto fail;
    }
    if (!WriteProcessMemory(pi.hProcess, remote, dllPath, len, NULL)) {
        fprintf(stderr, "WriteProcessMemory failed: %lu\n", GetLastError());
        goto fail;
    }

    k32 = GetModuleHandleA("kernel32.dll");
    loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    if (!loadLibrary) {
        fprintf(stderr, "GetProcAddress(LoadLibraryA) failed: %lu\n", GetLastError());
        goto fail;
    }

    thread = CreateRemoteThread(pi.hProcess, NULL, 0, loadLibrary, remote, 0, NULL);
    if (!thread) {
        fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
        goto fail;
    }
    WaitForSingleObject(thread, INFINITE);

    /* LoadLibrary's return value: the remote HMODULE, or 0 on failure. */
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);

    if (exitCode == 0) {
        fprintf(stderr, "[!] LoadLibrary returned NULL in the target - DLL did not load\n");
        TerminateProcess(pi.hProcess, 1);
        goto fail;
    }
    printf("[+] injected, remote HMODULE = 0x%08lX\n", exitCode);

    ResumeThread(pi.hThread);
    printf("[+] resumed\n");

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;

fail:
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}
