/* Steam platform network gate: adapters, DNS, TCP, TLS, WinHTTP, and WinInet. */

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <windns.h>
#include <winhttp.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

#include "../thirdparty/wine/programs/winehua_smoke_protocol.h"

#define STORE_HOST_A "store.steampowered.com"
#define STORE_HOST_W L"store.steampowered.com"
#define UPDATE_HOST_A "client-update.steamstatic.com"
#define UPDATE_HOST_W L"client-update.steamstatic.com"
#define UPDATE_PATH_W L"/steam_client_win32"
#define UPDATE_URL_W L"https://client-update.steamstatic.com/steam_client_win32"
#define SELF_SIGNED_HOST_W L"self-signed.badssl.com"
#define WRONG_HOST_W L"wrong.host.badssl.com"
#define MAX_HTTP_BODY (16 * 1024 * 1024)

enum http_stage
{
    HTTP_STAGE_NONE,
    HTTP_STAGE_SESSION,
    HTTP_STAGE_CONNECTION,
    HTTP_STAGE_REQUEST,
    HTTP_STAGE_SENT,
    HTTP_STAGE_RESPONSE,
    HTTP_STAGE_HEADERS,
    HTTP_STAGE_BODY,
    HTTP_STAGE_COMPLETE
};

struct adapter_probe
{
    ULONG flags;
    ULONG size_result;
    ULONG query_result;
    ULONG adapter_count;
    ULONG ipv4_count;
};

struct http_probe
{
    DWORD stage;
    DWORD error;
    DWORD status;
    DWORD bytes;
    DWORD cert_query_error;
    DWORD cert_chain_error;
    DWORD family;
    DWORD reused; /* 0: connect callback, 1: no new connection, 2: unknown */
    DWORD body_hash;
    ULONGLONG dns_ms, connect_tls_ms, send_request_total_ms, ttfb_ms, read_ms, total_ms;
    ULONGLONG dns_started, connect_started;
};

struct wininet_probe
{
    DWORD stage;
    DWORD error;
    DWORD status;
    DWORD bytes;
};

BOOL winehua_probe_wininet_update(DWORD *stage, DWORD *error, DWORD *status, DWORD *bytes);

static BOOL probe_adapters(struct adapter_probe *probe)
{
    IP_ADAPTER_ADDRESSES *buffer, *adapter;
    ULONG size = 0;

    probe->size_result = GetAdaptersAddresses(AF_UNSPEC, probe->flags, NULL, NULL, &size);
    if (probe->size_result != ERROR_BUFFER_OVERFLOW || !size) return FALSE;

    buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!buffer) return FALSE;
    probe->query_result = GetAdaptersAddresses(AF_UNSPEC, probe->flags, NULL, buffer, &size);
    if (!probe->query_result)
    {
        for (adapter = buffer; adapter; adapter = adapter->Next)
        {
            IP_ADAPTER_UNICAST_ADDRESS *address;
            ++probe->adapter_count;
            for (address = adapter->FirstUnicastAddress; address; address = address->Next)
                if (address->Address.lpSockaddr && address->Address.lpSockaddr->sa_family == AF_INET)
                    ++probe->ipv4_count;
        }
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    return probe->query_result == ERROR_SUCCESS && probe->adapter_count > 0;
}

static DWORD probe_dns(const char *host, DWORD *count, ULONGLONG *elapsed)
{
    DNS_RECORD *records = NULL, *record;
    ULONGLONG start = GetTickCount64();
    DNS_STATUS status = DnsQuery_A(host, DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &records, NULL);

    *elapsed = GetTickCount64() - start;
    *count = 0;
    if (!status)
        for (record = records; record; record = record->pNext)
            if (record->wType == DNS_TYPE_A) ++*count;
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
    return status;
}

static DWORD probe_tcp(const char *host, DWORD *family, ULONGLONG *elapsed)
{
    ADDRINFOA hints, *addresses = NULL, *address;
    DWORD result = WSAHOST_NOT_FOUND;
    ULONGLONG start, deadline;
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ret = GetAddrInfoA(host, "443", &hints, &addresses);
    if (ret) return (DWORD)ret;
    start = GetTickCount64();
    deadline = start + 10000;

    for (address = addresses; address; address = address->ai_next)
    {
        ULONGLONG now = GetTickCount64(), remaining;
        SOCKET socket_handle;
        if (now >= deadline) { result = WSAETIMEDOUT; break; }
        remaining = deadline - now;
        *family = address->ai_family;
        socket_handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        u_long nonblocking = 1;
        int socket_error = 0;
        int socket_error_size = sizeof(socket_error);
        fd_set write_set, error_set;
        struct timeval timeout;

        if (socket_handle == INVALID_SOCKET)
        {
            result = WSAGetLastError();
            continue;
        }
        if (ioctlsocket(socket_handle, FIONBIO, &nonblocking))
        {
            result = WSAGetLastError();
            closesocket(socket_handle);
            continue;
        }
        ret = connect(socket_handle, address->ai_addr, (int)address->ai_addrlen);
        if (!ret)
        {
            closesocket(socket_handle);
            result = ERROR_SUCCESS;
            break;
        }
        result = WSAGetLastError();
        if (result == WSAEWOULDBLOCK || result == WSAEINPROGRESS)
        {
            FD_ZERO(&write_set);
            FD_ZERO(&error_set);
            FD_SET(socket_handle, &write_set);
            FD_SET(socket_handle, &error_set);
            timeout.tv_sec = (long)(remaining / 1000);
            timeout.tv_usec = (long)((remaining % 1000) * 1000);
            ret = select(0, NULL, &write_set, &error_set, &timeout);
            if (ret > 0 && !getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                                      (char *)&socket_error, &socket_error_size))
                result = socket_error;
            else if (!ret)
                result = WSAETIMEDOUT;
            else if (ret < 0)
                result = WSAGetLastError();
        }
        closesocket(socket_handle);
        if (!result) break;
    }
    *elapsed = GetTickCount64() - start;
    FreeAddrInfoA(addresses);
    return result;
}

static DWORD verify_certificate_chain(PCCERT_CONTEXT certificate, const WCHAR *host)
{
    CERT_CHAIN_PARA chain_parameters;
    CERT_CHAIN_POLICY_PARA policy_parameters;
    CERT_CHAIN_POLICY_STATUS policy_status;
    HTTPSPolicyCallbackData ssl_parameters;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    DWORD result;

    memset(&chain_parameters, 0, sizeof(chain_parameters));
    chain_parameters.cbSize = sizeof(chain_parameters);
    if (!CertGetCertificateChain(NULL, certificate, NULL, certificate->hCertStore,
                                 &chain_parameters, 0, NULL, &chain))
        return GetLastError();

    memset(&ssl_parameters, 0, sizeof(ssl_parameters));
    ssl_parameters.cbStruct = sizeof(ssl_parameters);
    ssl_parameters.dwAuthType = AUTHTYPE_SERVER;
    ssl_parameters.pwszServerName = (WCHAR *)host;
    memset(&policy_parameters, 0, sizeof(policy_parameters));
    policy_parameters.cbSize = sizeof(policy_parameters);
    policy_parameters.pvExtraPolicyPara = &ssl_parameters;
    memset(&policy_status, 0, sizeof(policy_status));
    policy_status.cbSize = sizeof(policy_status);

    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                          &policy_parameters, &policy_status))
        result = GetLastError();
    else
        result = policy_status.dwError;
    CertFreeCertificateChain(chain);
    return result;
}

/* Callback payloads may contain endpoints. Never store or log those payloads. */
static void CALLBACK network_timing_callback(HINTERNET handle, DWORD_PTR context, DWORD status,
                                             void *information, DWORD information_size)
{
    struct http_probe *probe = (struct http_probe *)context;
    ULONGLONG now = GetTickCount64();
    (void)handle;
    (void)information;
    (void)information_size;
    if (!probe) return;
    switch (status)
    {
    case WINHTTP_CALLBACK_STATUS_RESOLVING_NAME: probe->dns_started = now; break;
    case WINHTTP_CALLBACK_STATUS_NAME_RESOLVED: probe->dns_ms += now - probe->dns_started; break;
    case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER:
        probe->connect_started = now;
        probe->reused = 0;
        break;
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
        /* Wine emits CONNECTED after TLS. Do not present this as TCP-only. */
        probe->connect_tls_ms += now - probe->connect_started;
        break;
    }
}

static BOOL probe_winhttp(const WCHAR *host, const WCHAR *path, struct http_probe *probe)
{
    HINTERNET session = NULL, connection = NULL, request = NULL;
    PCCERT_CONTEXT certificate = NULL;
    DWORD size, read, i;
    ULONGLONG start = GetTickCount64(), phase, read_start = 0;
    WINHTTP_CONNECTION_INFO info;
    BYTE buffer[4096];
    BOOL ok = FALSE;

    memset(probe, 0, sizeof(*probe));
    probe->reused = 2;
    probe->body_hash = 2166136261u;
    session = WinHttpOpen(L"WineHua Steam network gate/2.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto failed;
    probe->stage = HTTP_STAGE_SESSION;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 20000);

    connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) goto failed;
    probe->stage = HTTP_STAGE_CONNECTION;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (!request) goto failed;
    probe->stage = HTTP_STAGE_REQUEST;
    if (WinHttpSetStatusCallback(request, network_timing_callback,
                                 WINHTTP_CALLBACK_FLAG_RESOLVE_NAME | WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER,
                                 0) == WINHTTP_INVALID_STATUS_CALLBACK) goto failed;
    phase = GetTickCount64();
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR)probe))
    {
        probe->send_request_total_ms = GetTickCount64() - phase;
        goto failed;
    }
    probe->send_request_total_ms = GetTickCount64() - phase;
    if (probe->reused == 2) probe->reused = 1;
    probe->stage = HTTP_STAGE_SENT;
    phase = GetTickCount64();
    if (!WinHttpReceiveResponse(request, NULL))
    {
        probe->ttfb_ms = GetTickCount64() - phase;
        goto failed;
    }
    probe->ttfb_ms = GetTickCount64() - phase;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    size = sizeof(info);
    if (WinHttpQueryOption(request, WINHTTP_OPTION_CONNECTION_INFO, &info, &size))
        probe->family = info.RemoteAddress.ss_family;
    probe->stage = HTTP_STAGE_RESPONSE;

    size = sizeof(probe->status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &probe->status, &size,
                             WINHTTP_NO_HEADER_INDEX))
        goto failed;
    probe->stage = HTTP_STAGE_HEADERS;

    size = sizeof(certificate);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &size))
        probe->cert_query_error = GetLastError();
    else
    {
        probe->cert_chain_error = verify_certificate_chain(certificate, host);
        CertFreeCertificateContext(certificate);
        certificate = NULL;
    }

    read_start = GetTickCount64();
    for (;;)
    {
        read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) goto failed;
        if (!read) break;
        if (probe->bytes > MAX_HTTP_BODY - read)
        {
            SetLastError(ERROR_FILE_TOO_LARGE);
            goto failed;
        }
        for (i = 0; i < read; ++i) probe->body_hash = (probe->body_hash ^ buffer[i]) * 16777619u;
        probe->bytes += read;
        probe->stage = HTTP_STAGE_BODY;
    }
    probe->stage = HTTP_STAGE_COMPLETE;
    ok = probe->status >= 200 && probe->status < 400 && probe->bytes > 0 &&
         !probe->cert_query_error && !probe->cert_chain_error;
    goto done;

failed:
    probe->error = GetLastError();
done:
    probe->total_ms = GetTickCount64() - start;
    if (read_start) probe->read_ms = GetTickCount64() - read_start;
    if (certificate) CertFreeCertificateContext(certificate);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

static SECURITY_STATUS probe_schannel(void)
{
    SCHANNEL_CRED credentials;
    CredHandle handle;
    TimeStamp expiry;
    SECURITY_STATUS status;

    memset(&credentials, 0, sizeof(credentials));
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    status = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND,
                                       NULL, &credentials, NULL, NULL, &handle, &expiry);
    if (status == SEC_E_OK) FreeCredentialsHandle(&handle);
    return status;
}

static DWORD count_root_certificates(DWORD *error)
{
    HCERTSTORE store;
    PCCERT_CONTEXT certificate = NULL;
    DWORD count = 0, enumeration_error;

    *error = ERROR_SUCCESS;
    store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store)
    {
        *error = GetLastError();
        return 0;
    }
    while ((certificate = CertEnumCertificatesInStore(store, certificate))) ++count;
    enumeration_error = GetLastError();
    if (enumeration_error != (DWORD)CRYPT_E_NOT_FOUND) *error = enumeration_error;
    CertCloseStore(store, 0);
    return count;
}

int main(int argc, char **argv)
{
    static const ULONG flags[] = {0x00, 0x0f, 0x27, 0x2e};
    struct winehua_smoke_options options;
    struct adapter_probe adapters[4];
    struct http_probe store_http, update_http, self_signed_http, wrong_host_http;
    struct wininet_probe update_wininet;
    WSADATA winsock_data;
    SECURITY_STATUS schannel_status;
    char metrics[6144], message[512], timing[2048];
    DWORD store_answers, update_answers, tcp_family = 0;
    ULONGLONG store_dns_ms, update_dns_ms, tcp_ms = 0, wininet_ms, phase;
    DWORD store_dns, update_dns, tcp_error, root_error, root_count;
    BOOL ok = TRUE, store_ok, update_ok, wininet_ok;
    int winsock_status;
    unsigned int i;

    if (!winehua_smoke_parse_options(&options, argc, argv, 15)) return 2;
    winsock_status = WSAStartup(MAKEWORD(2, 2), &winsock_data);
    if (winsock_status) ok = FALSE;

    memset(adapters, 0, sizeof(adapters));
    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
    {
        adapters[i].flags = flags[i];
        if (!probe_adapters(&adapters[i])) ok = FALSE;
    }

    store_dns = probe_dns(STORE_HOST_A, &store_answers, &store_dns_ms);
    update_dns = probe_dns(UPDATE_HOST_A, &update_answers, &update_dns_ms);
    tcp_error = winsock_status ? (DWORD)winsock_status : probe_tcp(UPDATE_HOST_A, &tcp_family, &tcp_ms);
    store_ok = probe_winhttp(STORE_HOST_W, L"/", &store_http);
    update_ok = probe_winhttp(UPDATE_HOST_W, UPDATE_PATH_W, &update_http);
    phase = GetTickCount64();
    wininet_ok = winehua_probe_wininet_update(&update_wininet.stage, &update_wininet.error,
                                              &update_wininet.status, &update_wininet.bytes);
    wininet_ms = GetTickCount64() - phase;
    probe_winhttp(SELF_SIGNED_HOST_W, L"/", &self_signed_http);
    probe_winhttp(WRONG_HOST_W, L"/", &wrong_host_http);
    schannel_status = probe_schannel();
    root_count = count_root_certificates(&root_error);

    if (store_dns || !store_answers || update_dns || !update_answers || tcp_error ||
        !store_ok || !update_ok || !wininet_ok || schannel_status != SEC_E_OK ||
        root_error || !root_count ||
        self_signed_http.error != ERROR_WINHTTP_SECURE_FAILURE ||
        wrong_host_http.error != ERROR_WINHTTP_SECURE_FAILURE)
        ok = FALSE;

    snprintf(metrics, sizeof(metrics),
             "{\"gaa\":["
             "{\"flags\":0,\"size\":%lu,\"query\":%lu,\"adapters\":%lu,\"ipv4\":%lu},"
             "{\"flags\":15,\"size\":%lu,\"query\":%lu,\"adapters\":%lu,\"ipv4\":%lu},"
             "{\"flags\":39,\"size\":%lu,\"query\":%lu,\"adapters\":%lu,\"ipv4\":%lu},"
             "{\"flags\":46,\"size\":%lu,\"query\":%lu,\"adapters\":%lu,\"ipv4\":%lu}],"
             "\"storeDnsError\":%lu,\"storeDnsAnswers\":%lu,"
             "\"updateDnsError\":%lu,\"updateDnsAnswers\":%lu,\"updateTcpError\":%lu,"
             "\"storeWinHttp\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu,\"bytes\":%lu,\"certQueryError\":%lu,\"certChainError\":%lu},"
             "\"updateWinHttp\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu,\"bytes\":%lu,\"certQueryError\":%lu,\"certChainError\":%lu},"
             "\"updateWinInet\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu,\"bytes\":%lu},"
             "\"schannelStatus\":%ld,\"rootCertificates\":%lu,\"rootError\":%lu,"
             "\"selfSigned\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu},"
             "\"wrongHost\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu}}",
             adapters[0].size_result, adapters[0].query_result, adapters[0].adapter_count, adapters[0].ipv4_count,
             adapters[1].size_result, adapters[1].query_result, adapters[1].adapter_count, adapters[1].ipv4_count,
             adapters[2].size_result, adapters[2].query_result, adapters[2].adapter_count, adapters[2].ipv4_count,
             adapters[3].size_result, adapters[3].query_result, adapters[3].adapter_count, adapters[3].ipv4_count,
             store_dns, store_answers, update_dns, update_answers, tcp_error,
             store_http.stage, store_http.error, store_http.status, store_http.bytes,
             store_http.cert_query_error, store_http.cert_chain_error,
             update_http.stage, update_http.error, update_http.status, update_http.bytes,
             update_http.cert_query_error, update_http.cert_chain_error,
             update_wininet.stage, update_wininet.error, update_wininet.status, update_wininet.bytes,
             (long)schannel_status, root_count, root_error,
             self_signed_http.stage, self_signed_http.error, self_signed_http.status,
             wrong_host_http.stage, wrong_host_http.error, wrong_host_http.status);
    snprintf(timing, sizeof(timing),
             ",\"timing\":{\"storeDnsMs\":%llu,\"updateDnsMs\":%llu,\"tcpConnectMs\":%llu,\"tcpFamily\":%lu,"
             "\"wininetTotalMs\":%llu,\"winhttp\":{\"dnsMs\":%llu,\"connectTlsMs\":%llu,"
             "\"sendRequestTotalMs\":%llu,\"ttfbMs\":%llu,\"readTotalMs\":%llu,\"totalMs\":%llu,"
             "\"family\":%lu,\"reused\":%lu,\"bodyFnv1a\":%lu}}}",
             (unsigned long long)store_dns_ms, (unsigned long long)update_dns_ms,
             (unsigned long long)tcp_ms, tcp_family, (unsigned long long)wininet_ms,
             (unsigned long long)update_http.dns_ms, (unsigned long long)update_http.connect_tls_ms,
             (unsigned long long)update_http.send_request_total_ms, (unsigned long long)update_http.ttfb_ms,
             (unsigned long long)update_http.read_ms, (unsigned long long)update_http.total_ms,
             update_http.family, update_http.reused, update_http.body_hash);
    /* Append only bounded, non-identifying numeric metrics to the JSON object. */
    if (strlen(metrics) + strlen(timing) < sizeof(metrics))
        strcpy(metrics + strlen(metrics) - 1, timing);
    snprintf(message, sizeof(message),
             "Network DNS rc=%lu TCP=%lu; WinHTTP stage=%lu err=%lu status=%lu bytes=%lu cert=%lu; WinInet stage=%lu err=%lu status=%lu bytes=%lu",
             update_dns, tcp_error,
             update_http.stage, update_http.error, update_http.status, update_http.bytes,
             update_http.cert_chain_error, update_wininet.stage, update_wininet.error,
             update_wininet.status, update_wininet.bytes);

    if (!winsock_status) WSACleanup();
    if (!winehua_smoke_write_result(&options, ok ? "PASS" : "FAIL",
                                    ok ? "complete" : "platform-network",
                                    message, metrics))
        return 3;
    return ok ? 0 : 1;
}
