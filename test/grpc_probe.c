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
    int n = grpc_call(&c, "/sushi_rpc.AudioGraphController/GetAllProcessors", NULL, 0, buf, sizeof buf);
    printf("GetAllProcessors -> %d bytes\n", n);
    uint8_t req[4] = { 0x08, 7 };
    n = grpc_call(&c, "/sushi_rpc.ParameterController/GetProcessorParameters", req, 2, buf, sizeof buf);
    printf("GetProcessorParameters (same connection) -> %d bytes\n", n);
    grpc_close(&c);
    n = grpc_call(&c, "/sushi_rpc.ParameterController/GetProcessorParameters", req, 2, buf, sizeof buf);
    printf("GetProcessorParameters (new connection) -> %d bytes\n", n);
    return 0;
}
