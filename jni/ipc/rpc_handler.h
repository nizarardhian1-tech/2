#pragma once
// =============================================================================
// rpc_handler.h — RPC Handler Registrar (libinternal.so)
//
// Modul ini mendaftarkan semua handler IPC ke IPCServer.
// Setiap handler memanggil IL2CPP API atau KittyMemory langsung.
//
// CARA KERJA:
//   RpcHandler::registerAll() dipanggil dari internal_main.cpp
//   SETELAH Il2cpp::Init() dan Il2cpp::EnsureAttached() selesai.
//
// PRINSIP:
//   Handler hanya berisi logika IL2CPP/memory.
//   TIDAK ADA ImGui, EGL, atau GL di sini sama sekali.
// =============================================================================

#ifndef RPC_HANDLER_H
#define RPC_HANDLER_H

class RpcHandler {
public:
    /**
     * Daftarkan semua handler ke IPCServer::get().
     * Panggil setelah Il2cpp::Init() selesai.
     */
    static void registerAll();

private:
    RpcHandler() = delete;
};

#endif // RPC_HANDLER_H
