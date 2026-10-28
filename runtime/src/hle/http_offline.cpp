// libSceHttp / libSceSsl offline: contexts, templates, connections and
// requests can all be created, but nothing can be sent, so every request
// fails with a network error. That's what a console without a cable reports,
// and the game falls back to playing offline. Error codes follow shadPS4.
#include <atomic>

#include "hle/kernel_errors.h"
#include "hle/net_delay.h"
#include "hle/register.h"

namespace hle::http {
namespace {

constexpr int kErrorNetwork = static_cast<int>(0x80431063);
constexpr int kErrorBeforeSend = static_cast<int>(0x80431065);
constexpr int kErrorInvalidValue = static_cast<int>(0x804311fe);
constexpr int kErrorSslInvalidId = static_cast<int>(0x8095F001);
constexpr int kENetUnreach = 51;
constexpr int kFirstId = 1;

std::atomic<int> g_nextId{kFirstId};

int NewId() { return g_nextId++; }

}  // namespace

int PS4_SYSV_ABI sceHttpInit(int, int, size_t) { return NewId(); }
int PS4_SYSV_ABI sceHttpTerm(int) { return kernel::SCE_OK; }

int PS4_SYSV_ABI sceHttpCreateTemplate(int, const char*, int, int) { return NewId(); }
int PS4_SYSV_ABI sceHttpDeleteTemplate(int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpCreateConnection(int, const char*, const char*, uint16_t, int) { return NewId(); }
int PS4_SYSV_ABI sceHttpCreateConnectionWithURL(int, const char*, int) { return NewId(); }
int PS4_SYSV_ABI sceHttpDeleteConnection(int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpCreateRequest(int, int, const char*, uint64_t) { return NewId(); }
int PS4_SYSV_ABI sceHttpCreateRequestWithURL(int, int, const char*, uint64_t) { return NewId(); }
int PS4_SYSV_ABI sceHttpDeleteRequest(int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpAbortRequest(int) { return kernel::SCE_OK; }

// The one that matters: there's no route out.
int PS4_SYSV_ABI sceHttpSendRequest(int, const void*, size_t) {
    net::UnreachableDelay("sceHttpSendRequest");
    return kErrorNetwork;
}

int PS4_SYSV_ABI sceHttpGetStatusCode(int, int*) { return kErrorBeforeSend; }
int PS4_SYSV_ABI sceHttpGetResponseContentLength(int, int*, uint64_t*) { return kErrorBeforeSend; }
int PS4_SYSV_ABI sceHttpGetAllResponseHeaders(int, char**, size_t*) { return kErrorBeforeSend; }
int PS4_SYSV_ABI sceHttpParseResponseHeader(const char*, size_t, const char*, const char**, size_t*) {
    return kErrorInvalidValue;
}
int PS4_SYSV_ABI sceHttpReadData(int, void*, size_t) { return kErrorBeforeSend; }

int PS4_SYSV_ABI sceHttpGetLastErrno(int, int* err) {
    if (err) *err = kENetUnreach;
    return kernel::SCE_OK;
}

// Settings: accepted and remembered nowhere.
int PS4_SYSV_ABI sceHttpAddRequestHeader(int, const char*, const char*, int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpRemoveRequestHeader(int, const char*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetRequestContentLength(int, uint64_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetAutoRedirect(int, int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetAuthEnabled(int, int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetCookieEnabled(int, int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetConnectTimeOut(int, uint32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetSendTimeOut(int, uint32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetRecvTimeOut(int, uint32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetRedirectCallback(int, void*, void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetCookieRecvCallback(int, void*, void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpSetCookieSendCallback(int, void*, void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpsSetSslCallback(int, void*, void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpsLoadCert(int, int, const void*, const void*, const void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpsUnloadCert(int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpAddCookie(int, const char*, const char*, size_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceHttpCookieFlush(int) { return kernel::SCE_OK; }

int PS4_SYSV_ABI sceHttpGetAutoRedirect(int, int* enabled) {
    if (enabled) *enabled = 1;
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceHttpGetCookieEnabled(int, int* enabled) {
    if (enabled) *enabled = 1;
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceHttpGetAuthEnabled(int, int* enabled) {
    if (enabled) *enabled = 0;
    return kernel::SCE_OK;
}

// --- libSceSsl ---------------------------------------------------------------------

int PS4_SYSV_ABI sceSslInit(size_t) { return NewId(); }
int PS4_SYSV_ABI sceSslTerm(int) { return kernel::SCE_OK; }
// Certificates only exist for real connections.
int PS4_SYSV_ABI sceSslGetIssuerName(const void*, void**) { return kErrorSslInvalidId; }
int PS4_SYSV_ABI sceSslGetSerialNumber(int, const void*, const uint8_t**, size_t*) { return kErrorSslInvalidId; }
int PS4_SYSV_ABI sceSslGetNameEntryInfo(const void*, int, char*, size_t, uint8_t*, size_t, size_t*) {
    return kErrorSslInvalidId;
}
int PS4_SYSV_ABI sceSslFreeSslCertName(int, void*) { return kernel::SCE_OK; }

}  // namespace hle::http

namespace hle {

void RegisterHttpOffline(core::Linker& linker) {
    using namespace http;
    const char* h = "libSceHttp";
    HLE_REG(linker, h, sceHttpInit);
    HLE_REG(linker, h, sceHttpTerm);
    HLE_REG(linker, h, sceHttpCreateTemplate);
    HLE_REG(linker, h, sceHttpDeleteTemplate);
    HLE_REG(linker, h, sceHttpCreateConnection);
    HLE_REG(linker, h, sceHttpCreateConnectionWithURL);
    HLE_REG(linker, h, sceHttpDeleteConnection);
    HLE_REG(linker, h, sceHttpCreateRequest);
    HLE_REG(linker, h, sceHttpCreateRequestWithURL);
    HLE_REG(linker, h, sceHttpDeleteRequest);
    HLE_REG(linker, h, sceHttpAbortRequest);
    HLE_REG(linker, h, sceHttpSendRequest);
    HLE_REG(linker, h, sceHttpGetStatusCode);
    HLE_REG(linker, h, sceHttpGetResponseContentLength);
    HLE_REG(linker, h, sceHttpGetAllResponseHeaders);
    HLE_REG(linker, h, sceHttpParseResponseHeader);
    HLE_REG(linker, h, sceHttpReadData);
    HLE_REG(linker, h, sceHttpGetLastErrno);
    HLE_REG(linker, h, sceHttpAddRequestHeader);
    HLE_REG(linker, h, sceHttpRemoveRequestHeader);
    HLE_REG(linker, h, sceHttpSetRequestContentLength);
    HLE_REG(linker, h, sceHttpSetAutoRedirect);
    HLE_REG(linker, h, sceHttpSetAuthEnabled);
    HLE_REG(linker, h, sceHttpSetCookieEnabled);
    HLE_REG(linker, h, sceHttpSetConnectTimeOut);
    HLE_REG(linker, h, sceHttpSetSendTimeOut);
    HLE_REG(linker, h, sceHttpSetRecvTimeOut);
    HLE_REG(linker, h, sceHttpSetRedirectCallback);
    HLE_REG(linker, h, sceHttpSetCookieRecvCallback);
    HLE_REG(linker, h, sceHttpSetCookieSendCallback);
    HLE_REG(linker, h, sceHttpsSetSslCallback);
    HLE_REG(linker, h, sceHttpsLoadCert);
    HLE_REG(linker, h, sceHttpsUnloadCert);
    HLE_REG(linker, h, sceHttpAddCookie);
    HLE_REG(linker, h, sceHttpCookieFlush);
    HLE_REG(linker, h, sceHttpGetAutoRedirect);
    HLE_REG(linker, h, sceHttpGetCookieEnabled);
    HLE_REG(linker, h, sceHttpGetAuthEnabled);
    const char* s = "libSceSsl";
    HLE_REG(linker, s, sceSslInit);
    HLE_REG(linker, s, sceSslTerm);
    HLE_REG(linker, s, sceSslGetIssuerName);
    HLE_REG(linker, s, sceSslGetSerialNumber);
    HLE_REG(linker, s, sceSslGetNameEntryInfo);
    HLE_REG(linker, s, sceSslFreeSslCertName);
}

}  // namespace hle
