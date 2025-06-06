#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucp/api/ucp.h>
#include <mpi.h>
#include <omp.h>
#include <math.h>
#include <stdbool.h>
#include <sys/time.h>
#include <sys/mman.h>

#define MPI_SAFE(fncall)                                                      \
do {                                                                          \
    int retcode = (fncall);                                                   \
    if (retcode != MPI_SUCCESS) {                                             \
        char msg[1024];                                                       \
        int resultlen;                                                        \
        MPI_Error_string(retcode, msg, &resultlen);                           \
        fprintf(stderr, "Call %s failed with %s(%d)\n",                       \
                     #fncall, msg, retcode);                                  \
        MPI_Abort(MPI_COMM_WORLD, retcode);                                   \
    }                                                                         \
} while (0)

#define UCS_SAFE(fncall)                                                      \
    do {                                                                      \
        ucs_status_t status = (fncall);                                       \
        if (status != UCS_OK) {                                               \
            fprintf(stderr, "%s:%d Call %s failed with %s\n",                 \
                    __FILE__, __LINE__, #fncall, ucs_status_string(status));  \
        }                                                                     \
    } while (0)

static ucp_context_h ucp_context;
static ucp_ep_h *endpoints;
static ucp_ep_params_t *ep_params;
static ucp_worker_h ucp_worker;

static int pid;
static int nprocs;

typedef void *bsp_request;

static void request_init(void *request)
{
    (void)request;
}

static ucs_status_t ucx_wait(ucp_worker_h ucp_worker, bsp_request request)
{
    ucs_status_t stat;

    if (UCS_PTR_IS_ERR(request)) {
        stat = UCS_PTR_STATUS(request);
    } else if (UCS_PTR_IS_PTR(request)) {
        while ((stat = ucp_request_check_status(request)) == UCS_INPROGRESS) {
            ucp_worker_progress(ucp_worker);
        }
    } else {
        stat = UCS_OK;
        if (request != UCS_OK && !UCS_PTR_IS_ERR(request)) {
            ucp_request_free(request);
        }
    }

    return stat;
}

/* Uses MPI for wire-up, assumes MPI_Init has been called. */
void bsp_init(void)
{
    MPI_SAFE(MPI_Comm_rank(MPI_COMM_WORLD, &pid));
    MPI_SAFE(MPI_Comm_size(MPI_COMM_WORLD, &nprocs));

    ucp_config_t *config;
    UCS_SAFE(ucp_config_read(NULL, NULL, &config));

    ucp_params_t ucp_params = {
        .field_mask   = UCP_PARAM_FIELD_FEATURES |
                        UCP_PARAM_FIELD_REQUEST_SIZE |
                        UCP_PARAM_FIELD_REQUEST_INIT |
                        UCP_PARAM_FIELD_ESTIMATED_NUM_EPS |
                        UCP_PARAM_FIELD_ESTIMATED_NUM_PPN |
                        UCP_PARAM_FIELD_NAME,
        .features     = UCP_FEATURE_TAG |
                        UCP_FEATURE_RMA,
        .request_size = sizeof(bsp_request),
        .request_init = request_init,
        .estimated_num_eps = nprocs,
        .estimated_num_ppn = 1,
        .name         = "BSP"};

    UCS_SAFE(ucp_init(&ucp_params, config, &ucp_context));
    ucp_config_release(config);

    ucp_worker_params_t worker_params = {
        .field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE,
        .thread_mode = UCS_THREAD_MODE_MULTI
    };
    UCS_SAFE(ucp_worker_create(ucp_context, &worker_params, &ucp_worker));

    ucp_worker_attr_t worker_attr = {
        .field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS
    };
    UCS_SAFE(ucp_worker_query(ucp_worker, &worker_attr));

    uint64_t local_addr_len    = worker_attr.address_length;
    ucp_address_t *local_addr  = worker_attr.address;
    int *remote_addr_len       = malloc(nprocs * sizeof(int));
    int *remote_addr_off       = malloc(nprocs * sizeof(int));
    MPI_SAFE(MPI_Allgather(&local_addr_len, 1, MPI_INT,
                           remote_addr_len, 1, MPI_INT,
                           MPI_COMM_WORLD));
    size_t total_addr_len = remote_addr_len[0];
    remote_addr_off[0] = 0;
    for (int i = 1; i < nprocs; i++) {
        total_addr_len += remote_addr_len[i];
        remote_addr_off[i] = remote_addr_off[i - 1] + remote_addr_len[i - 1];
    }
    char *remote_addresses = malloc(total_addr_len);
    MPI_SAFE(MPI_Allgatherv(local_addr, local_addr_len, MPI_BYTE,
                            remote_addresses, remote_addr_len,
                            remote_addr_off, MPI_BYTE,
                            MPI_COMM_WORLD));

    endpoints = malloc(nprocs * sizeof(ucp_ep_h));
    ep_params = malloc(nprocs * sizeof(ucp_ep_params_t));
    for (int t = 0; t < nprocs; t++) {
        ep_params[t].field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
        ep_params[t].address         = (ucp_address_t *)(remote_addresses +
                                                      remote_addr_off[t]);
        UCS_SAFE(ucp_ep_create(ucp_worker, &ep_params[t], &endpoints[t]));
    }

    free(remote_addr_len);
    free(remote_addr_off);
}

void bsp_end(void)
{
    MPI_SAFE(MPI_Barrier(MPI_COMM_WORLD));
    ucp_request_param_t ep_req = {.flags = 0};
    for (int i = 0; i < nprocs; i++) {
        UCS_SAFE(ucx_wait(ucp_worker, ucp_ep_close_nbx(endpoints[i], &ep_req)));
    }
    free(endpoints);
    free(ep_params);
    ucp_worker_destroy(ucp_worker);
    ucp_cleanup(ucp_context);
}

double bsp_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)(tv.tv_usec) / 1e6 + (double)tv.tv_sec;
}

int bsp_nprocs(void)
{
    return nprocs;
}

int bsp_pid(void)
{
    return pid;
}

#define MMAP_SAFE(variable, address, length, prot)                            \
{                                                                         \
    variable = mmap(address, length, prot, MAP_ANONYMOUS | MAP_PRIVATE,   \
    -1, 0);                                                       \
    fprintf(stderr, "mmap: %p = mmap(%p, %zu, %s, "                            \
        "MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);",                   \
    variable, address, length, #prot);                        \
    if (variable == MAP_FAILED) {                                         \
        fprintf(stderr, "%s:%d [node %d]: ",                              \
        __FILE__, __LINE__, pid);                          \
        perror("mmap failed");                                            \
        MPI_Abort(MPI_COMM_WORLD, 1);                                                    \
    }                                                                     \
}

#define MPROTECT_SAFE(addr, len, prot)                                        \
{                                                                         \
    fprintf(stderr, "mprotect: protected [%p, %p[ to %s", addr,                \
    (void *)((uintptr_t)addr + len), #prot);                      \
    if (mprotect(addr, len, prot) != 0) {                                 \
        fprintf(stderr, "%s:%d [node %d]: ",                              \
        __FILE__, __LINE__, pid);                          \
        perror("mprotect failed");                                        \
        MPI_Abort(MPI_COMM_WORLD, 1);                                                    \
    }                                                                     \
}

#define HUGE_PGSZ (2 * 1024 * 1024)

intptr_t roundUp(intptr_t a, intptr_t b)
{
    return (a + b - 1) / b * b;
}

void scale(double *x, size_t n, double alpha)
{
    #pragma omp parallel for
    for (size_t i = 0; i < n; i++) {
        x[i] *= alpha;
    }
}

int main(int argc, char **argv)
{
    MPI_SAFE(MPI_Init(&argc, &argv));
    bsp_init();

    if (argc != 3) {
        printf("Usage: %s MIN MAX ITER\n"
               "\tMIN:  smallest tested size in GB\n"
               "\tMAX:  largest tested size in GB\n",
               argv[0]);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    size_t min  = 1e9 * atof(argv[1]);
    size_t max  = 1e9 * atof(argv[2]);

    double *x = malloc(max);
    #pragma omp parallel for
    for (size_t i = 0; i < max / sizeof(double); i++) {
        x[i] = 1.0;
    }

#ifdef MEM_MAP
    ucp_mem_h memh;
#endif

    printf("size in gb, bandwidth gb/s\n");
    for (size_t size = min; size <= max; size *= 2) {
#ifdef MEM_MAP
        ucp_mem_map_params_t params = {
            .field_mask  = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                           UCP_MEM_MAP_PARAM_FIELD_LENGTH  |
                           UCP_MEM_MAP_PARAM_FIELD_PROT    |
                           UCP_MEM_MAP_PARAM_FIELD_MEMORY_TYPE,
            .address     = x,
            .length      = size,
            .prot        = UCP_MEM_MAP_PROT_LOCAL_READ  |
                           UCP_MEM_MAP_PROT_LOCAL_WRITE |
                           UCP_MEM_MAP_PROT_REMOTE_READ |
                           UCP_MEM_MAP_PROT_REMOTE_WRITE,
            .memory_type = UCS_MEMORY_TYPE_HOST
        };

        UCS_SAFE(ucp_mem_map(ucp_context, &params, &memh));
#endif

        double start = bsp_time();
        scale(x, size / sizeof(double), 2.0);
        double stop = bsp_time();

#ifdef MEM_MAP
        UCS_SAFE(ucp_mem_unmap(ucp_context, memh));
#endif

        double bandwidth = 2.0 * (double)size / 1e9 / (stop - start);

        printf("%lf, %lf\n", (double)size / 1e9, bandwidth);
    }

    free(x);

    bsp_end();
    MPI_SAFE(MPI_Finalize());
}
