#include "runtime/cpu.h"

void imp_SteamAPI_Init(CPUContext* ctx) {
    /* Return false cleanly: PCGameApp::StartUp skips SteamUserStats & SteamRemoteStorage */
    ctx->eax = 0;
}

void imp_SteamAPI_Shutdown(CPUContext* ctx) { (void)ctx; }
void imp_SteamAPI_RunCallbacks(CPUContext* ctx) { (void)ctx; }
void imp_SteamAPI_RegisterCallback(CPUContext* ctx) { (void)ctx; }
void imp_SteamAPI_UnregisterCallback(CPUContext* ctx) { (void)ctx; }
void imp_SteamRemoteStorage(CPUContext* ctx) { ctx->eax = 0; }
void imp_SteamUserStats(CPUContext* ctx) { ctx->eax = 0; }
