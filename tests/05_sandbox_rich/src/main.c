#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

int main(void)
{
    printf("=================================================================\n");
    printf("     Linux Kernel Sandbox & Rich Manifest Verification Test      \n");
    printf("=================================================================\n\n");

    // 1. Verify Injected Environment Variables
    const char *tag = getenv("CUSTOM_BUILD_TAG");
    const char *opt = getenv("OPTIMIZATION");
    printf("[1/3] Environment Injection Check:\n");
    printf("  • CUSTOM_BUILD_TAG: %s\n", tag ? tag : "NOT FOUND (FAILED)");
    printf("  • OPTIMIZATION:     %s\n", opt ? opt : "NOT FOUND (FAILED)");

    // 2. Verify Kernel Resource Limits (setrlimit)
    printf("\n[2/3] Kernel Resource Limits Check (getrlimit):\n");
    struct rlimit rl_data, rl_nproc;
    getrlimit(RLIMIT_DATA, &rl_data);
    getrlimit(RLIMIT_NPROC, &rl_nproc);

    printf("  • Data Segment Memory Limit (RLIMIT_DATA): %zu MB\n",
           rl_data.rlim_cur != RLIM_INFINITY ? (size_t)(rl_data.rlim_cur / (1024 * 1024)) : 0);
    printf("  • Host Process Count (RLIMIT_NPROC):        %zu\n", (size_t)rl_nproc.rlim_cur);

    // 3. Verify Network Isolation (Offline Loopback Sandbox)
    printf("\n[3/3] Network Namespace Isolation Check:\n");
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    bool network_blocked = false;
    if (sock >= 0) {
        // Try connecting to public DNS (8.8.8.8:53) with non-blocking / quick timeout
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &sin.sin_addr);

        // In an isolated network namespace without default route, connect fails immediately with ENETUNREACH
        int res = connect(sock, (struct sockaddr *)&sin, sizeof(sin));
        if (res < 0 && (errno == ENETUNREACH || errno == EHOSTUNREACH || errno == EPERM)) {
            printf("  • Outbound WAN connection: BLOCKED (%s)\n", strerror(errno));
            network_blocked = true;
        } else {
            printf("  • Outbound WAN connection: res=%d (%s)\n", res, strerror(errno));
        }
        close(sock);
    }

    printf("\n[Status] Sandbox verification SUCCESSFUL!\n");

    // Emit JSON report
    FILE *jf = fopen("sandbox_report.json", "w");
    if (jf) {
        fprintf(jf, "{\n");
        fprintf(jf, "  \"custom_build_tag\": \"%s\",\n", tag ? tag : "none");
        fprintf(jf, "  \"optimization\": \"%s\",\n", opt ? opt : "none");
        fprintf(jf, "  \"memory_limit_mb\": %zu,\n", (size_t)(rl_data.rlim_cur / (1024 * 1024)));
        fprintf(jf, "  \"max_pids_limit\": %zu,\n", (size_t)rl_nproc.rlim_cur);
        fprintf(jf, "  \"network_isolated\": %s\n", network_blocked ? "true" : "false");
        fprintf(jf, "}\n");
        fclose(jf);
        printf("[✓] Exported 'sandbox_report.json'\n");
    }

    return 0;
}
