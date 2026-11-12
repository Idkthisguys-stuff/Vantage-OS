#include <vantage/kernel/net.h>
#include <vantage/kernel/user_access.h>

/* Requests are snapshotted from the calling process and results copied back;
 * the network service only ever sees kernel memory. Output buffers are
 * checked before any effect, so a result is never lost after the fact. */
static int fetch(void* request, uintptr_t address, size_t size, unsigned int alignment) {
    return !(address % alignment) && user_copy_in(request, address, size);
}

static int writable(uintptr_t address, size_t size, unsigned int alignment) {
    return !(address % alignment) && user_access_ok(address, size, 1);
}

static void erase(void* memory, size_t bytes) {
    volatile unsigned char* p = memory;
    while (bytes--) *p++ = 0;
}

static int terminated(const char* text, unsigned int size) {
    for (unsigned int i = 0; i < size; i++) if (!text[i]) return 1;
    return 0;
}

int64_t handle_net_call(uint64_t id, uintptr_t argument, uint64_t owner) {
    if (!owner) return NET_UNAVAILABLE;
    if (id == SYS_NET_LEAVE) { net_poll(); return net_leave(); }
    if (id == SYS_NET_STATUS) {
        NetStatus status;
        if (!writable(argument, sizeof(status), 8)) return NET_BAD;
        net_poll();
        net_status(&status);
        return user_copy_out(argument, &status, sizeof(status)) ? 0 : NET_BAD;
    }
    if (id == SYS_NET_SCAN) {
        NetScan request;
        if (!fetch(&request, argument, sizeof(request), 4) || !writable(argument, sizeof(request), 4) ||
            request.operation > NET_SCAN_RESULT) return NET_BAD;
        net_poll();
        int result = net_scan(&request);
        if (result > 0 && !user_copy_out(argument, &request, sizeof(request))) return NET_BAD;
        return result;
    }
    if (id == SYS_NET_JOIN) {
        NetJoin request;
        if (!fetch(&request, argument, sizeof(request), 4)) return NET_BAD;
        net_poll();
        int result = net_join(&request);
        erase(&request, sizeof(request));
        return result;
    }
    if (id == SYS_NET_RESOLVE) {
        NetResolve request;
        if (!fetch(&request, argument, sizeof(request), 4) || !writable(argument, sizeof(request), 4) ||
            !terminated(request.name, sizeof(request.name))) return NET_BAD;
        net_poll();
        int result = net_resolve(&request);
        if (result > 0 && !user_copy_out(argument, &request, sizeof(request))) return NET_BAD;
        return result;
    }
    if (id == SYS_NET_TIME) {
        NetTime time;
        if (!writable(argument, sizeof(time), 8)) return NET_BAD;
        net_poll();
        int result = net_time(&time);
        return user_copy_out(argument, &time, sizeof(time)) ? result : NET_BAD;
    }
    if (id == SYS_NET_PROFILE) {
        NetProfile request;
        if (!fetch(&request, argument, sizeof(request), 4)) return NET_BAD;
        int list = request.operation == NET_PROFILE_LIST;
        if (request.operation > NET_PROFILE_CLEAR || (list && !writable(argument, sizeof(request), 4))) {
            erase(&request, sizeof(request));
            return NET_BAD;
        }
        net_poll();
        int result = net_profile(&request);
        if (list && result > 0 && !user_copy_out(argument, &request, sizeof(request))) result = NET_BAD;
        erase(&request, sizeof(request));
        return result;
    }
    if (id == SYS_NET_RANDOM) {
        NetRandom request;
        memset(&request, 0, sizeof(request));
        if (!fetch(&request, argument, 8, 4) || !request.length || request.length > NET_RANDOM_MAX ||
            request.flags || !writable(argument, sizeof(request), 4)) return NET_BAD;
        int result = net_entropy(request.data, request.length);
        if (result >= 0) result = user_copy_out(argument, &request, sizeof(request)) ? (int)request.length : NET_BAD;
        erase(&request, sizeof(request));
        return result;
    }
    if (id == SYS_NET_SOCKET) {
        NetSocketCall call;
        if (!fetch(&call, argument, sizeof(call), 8)) return NET_BAD;
        int output = call.operation == NET_SOCKET_RECEIVE || call.operation == NET_SOCKET_STATUS;
        if (output && !writable(argument, sizeof(call), 8)) return NET_BAD;
        if (call.operation == NET_SOCKET_RECEIVE && !call.length) return NET_BAD;
        net_poll();
        int64_t result = net_socket(owner, &call);
        if (output && result >= 0 && !user_copy_out(argument, &call, sizeof(call))) return NET_BAD;
        return result;
    }
    return NET_BAD;
}
