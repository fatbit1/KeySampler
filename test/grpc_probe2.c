#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/grpc_lite.c"
int main(void) {
    GrpcConn c = { .fd = -1, .port = 50077 };
    strcpy(c.host, "127.0.0.1");
    static uint8_t buf[60000];
    uint8_t req[4] = { 0x08, 7 };
    int n = grpc_call(&c, "/sushi_rpc.ParameterController/GetProcessorParameters", req, 2, buf, sizeof buf);
    size_t pos = 0; PbField f; int k = 0;
    while (pb_next(buf, n, &pos, &f) == 0 && k++ < 3) {
        printf("outer field %d wire %d sublen %zu\n", f.field, f.wire, f.sublen);
        size_t q = 0; PbField g;
        while (pb_next(f.sub, f.sublen, &q, &g) == 0) printf("   field %d wire %d v=%llu sub=%.*s f=%g\n", g.field, g.wire, (unsigned long long)g.v, (int)g.sublen, g.sub ? (const char*)g.sub : "", g.f);
    }
    return 0;
}
