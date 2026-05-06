#include <cuda_runtime.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

int main() {
    // 1. Initialize CUDA and allocate GPU memory
    cudaSetDevice(0);

    size_t bytes = 1024 * 1024 * 1024; // 1 GB
    void *d_ptr = nullptr;

    printf("Step 1: Allocating %zu MB GPU memory...\n", bytes / (1024*1024));
    cudaError_t err = cudaMalloc(&d_ptr, bytes);
    if (err != cudaSuccess) {
        printf("ERROR: cudaMalloc failed: %s\n", cudaGetErrorString(err));
        return 1;
    }
    printf("SUCCESS: GPU memory allocated at %p\n", d_ptr);

    // 2. Open IB device
    printf("\nStep 2: Opening IB device...\n");
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        printf("ERROR: ibv_get_device_list failed\n");
        cudaFree(d_ptr);
        return 1;
    }

    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        printf("ERROR: ibv_open_device failed\n");
        ibv_free_device_list(dev_list);
        cudaFree(d_ptr);
        return 1;
    }
    printf("SUCCESS: Opened IB device: %s\n", ibv_get_device_name(dev_list[0]));
    ibv_free_device_list(dev_list);

    // 3. Allocate protection domain
    printf("\nStep 3: Allocating protection domain...\n");
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        printf("ERROR: ibv_alloc_pd failed\n");
        ibv_close_device(ctx);
        cudaFree(d_ptr);
        return 1;
    }
    printf("SUCCESS: PD allocated\n");

    // 4. Try to register GPU memory with RDMA
    printf("\nStep 4: Registering GPU memory with ibv_reg_mr...\n");
    int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    struct ibv_mr *mr = ibv_reg_mr(pd, d_ptr, bytes, mr_flags);
    if (!mr) {
        printf("ERROR: ibv_reg_mr FAILED\n");
        printf("  errno = %d (%s)\n", errno, strerror(errno));

        switch(errno) {
            case ENOTSUP:
                printf("  → nvidia-peermem not loaded or not working\n");
                break;
            case ENOMEM:
                printf("  → Memory limit exceeded (ulimit -l)\n");
                break;
            case EFAULT:
                printf("  → Bad address - nvidia-peermem not intercepting\n");
                break;
            case EINVAL:
                printf("  → Invalid arguments\n");
                break;
            default:
                printf("  → Unknown error\n");
        }

        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        cudaFree(d_ptr);
        return 1;
    }

    printf("SUCCESS: GPU memory registered!\n");
    printf("  lkey = 0x%x\n", mr->lkey);
    printf("  rkey = 0x%x\n", mr->rkey);

    // Cleanup
    printf("\nCleaning up...\n");
    ibv_dereg_mr(mr);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    cudaFree(d_ptr);

    printf("\n✓ ALL TESTS PASSED - GPUDirect RDMA is working!\n");
    return 0;
}
