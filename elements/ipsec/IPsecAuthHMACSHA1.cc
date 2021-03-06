#include "IPsecAuthHMACSHA1.hh"
#ifdef USE_CUDA
#include "IPsecAuthHMACSHA1_kernel.hh"
#endif
#ifdef USE_KNAPP
#include <nba/engines/knapp/kernels.hh>
#endif
#include <nba/element/annotation.hh>
#include <nba/element/nodelocalstorage.hh>
#include <nba/framework/threadcontext.hh>
#include <nba/framework/computedevice.hh>
#include <nba/framework/computecontext.hh>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <netinet/ip.h>
#include "util_esp.hh"
#include "util_ipsec_key.hh"
#include "util_sa_entry.hh"
#include <rte_memory.h>
#include <rte_ether.h>

using namespace std;
using namespace nba;

/* Array which stores per-tunnel HMAC key for each tunnel.
 * It is copied to each node's node local storage during per-node initialization
 * and freed in per-thread initialization.*/
struct hmac_sa_entry *hmac_sa_entry_array;
/* Map which stores (src-dst pair, tunnel index).
 * It is copied to each node's node local storage during per-node initialization*/
unordered_map<struct ipaddr_pair, int> hmac_sa_table;

IPsecAuthHMACSHA1::IPsecAuthHMACSHA1(): OffloadableElement()
{
    #ifdef USE_CUDA
    auto ch = [this](ComputeDevice *cdev, ComputeContext *ctx, struct resource_param *res) {
        this->accel_compute_handler(cdev, ctx, res);
    };
    offload_compute_handlers.insert({{"cuda", ch},});
    auto ih = [this](ComputeDevice *dev) { this->accel_init_handler(dev); };
    offload_init_handlers.insert({{"cuda", ih},});
    #endif
    #ifdef USE_KNAPP
    auto ch = [this](ComputeDevice *cdev, ComputeContext *ctx, struct resource_param *res) {
        this->accel_compute_handler(cdev, ctx, res);
    };
    offload_compute_handlers.insert({{"knapp.phi", ch},});
    auto ih = [this](ComputeDevice *dev) { this->accel_init_handler(dev); };
    offload_init_handlers.insert({{"knapp.phi", ih},});
    #endif

    num_tunnels = 0;
    dummy_index = 0;
}

int IPsecAuthHMACSHA1::initialize()
{
    // Get ptr for CPU & GPU pkt processing from the node-local storage.

    /* Storage for host ipsec tunnel index table */
    h_sa_table = (unordered_map<struct ipaddr_pair, int> *)ctx->node_local_storage->get_alloc("h_hmac_sa_table");

    /* Storage for host hmac key array */
    flows = (struct hmac_sa_entry *) ctx->node_local_storage->get_alloc("h_hmac_flows");

    /* Get device pointer from the node local storage. */
    flows_d = (dev_mem_t *) ctx->node_local_storage->get_alloc("d_hmac_flows_ptr");

    if (hmac_sa_entry_array != NULL) {
        free(hmac_sa_entry_array);
        hmac_sa_entry_array = NULL;
    }

    return 0;
}

int IPsecAuthHMACSHA1::initialize_global()
{
    // generate global table and array only once per element class.
    struct ipaddr_pair pair;
    struct hmac_sa_entry *entry;

    assert(num_tunnels != 0);
    hmac_sa_entry_array = (struct hmac_sa_entry *) malloc(sizeof(struct hmac_sa_entry)*num_tunnels);

    for (int i = 0; i < num_tunnels; i++) {
        pair.src_addr  = 0x0a000001u;
        pair.dest_addr = 0x0a000000u | (i + 1); // (rand() % 0xffffff);
        auto result = hmac_sa_table.insert(make_pair<ipaddr_pair&, int&>(pair, i));
        assert(result.second == true);

        entry = &hmac_sa_entry_array[i];
        entry->entry_idx = i;
        rte_memcpy(&entry->hmac_key, "abcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcd", HMAC_KEY_SIZE);
    }

    return 0;
};

int IPsecAuthHMACSHA1::initialize_per_node()
{
    unordered_map<struct ipaddr_pair, int> *temp_table = NULL;
    struct hmac_sa_entry *temp_array = NULL;
    struct ipaddr_pair key;
    int value, size;

    /* Storage for host ipsec tunnel index table */
    size = sizeof(unordered_map<struct ipaddr_pair, int>);
    ctx->node_local_storage->alloc("h_hmac_sa_table", size);
    temp_table = (unordered_map<struct ipaddr_pair, int> *)ctx->node_local_storage->get_alloc("h_hmac_sa_table");
    new (temp_table) unordered_map<struct ipaddr_pair, int>();

    for (auto iter = hmac_sa_table.begin(); iter != hmac_sa_table.end(); iter++) {
        key = iter->first;
        value = iter->second;
        temp_table->insert(make_pair<ipaddr_pair&, int&>(key, value));
    }

    /* Storage for host hmac key array */
    size = sizeof(struct hmac_sa_entry) * num_tunnels;
    ctx->node_local_storage->alloc("h_hmac_flows", size);
    temp_array = (struct hmac_sa_entry *) ctx->node_local_storage->get_alloc("h_hmac_flows");
    assert(hmac_sa_entry_array != NULL);
    rte_memcpy(temp_array, hmac_sa_entry_array, size);

    /* Storage for pointer, which points hmac key array in device */
    ctx->node_local_storage->alloc("d_hmac_flows_ptr", sizeof(dev_mem_t));

    return 0;
}

int IPsecAuthHMACSHA1::configure(comp_thread_context *ctx, std::vector<std::string> &args)
{
    Element::configure(ctx, args);
    num_tunnels = 1024;         // TODO: this value must be come from configuration.

    return 0;
}

// Input packet: assumes encaped
// +----------+---------------+--------+----+------------+---------+-------+---------------------+
// | Ethernet | IP(proto=ESP) |  ESP   | IP |  payload   | padding | extra | HMAC-SHA1 signature |
// +----------+---------------+--------+----+------------+---------+-------+---------------------+
// ^ethh      ^iph            ^esph    ^encaped_iph
//                            ^payload_out
//                            ^encapsulated
//                            <===== authenticated part (payload_len) =====>
//
int IPsecAuthHMACSHA1::process(int input_port, Packet *pkt)
{
    // We assume the size of hmac_key is less than 64 bytes.
    // TODO: check if input pkt is encapulated or not.
    struct ether_hdr *ethh = (struct ether_hdr *) pkt->data();
    struct iphdr *iph      = (struct iphdr *) (ethh + 1);
    struct esphdr *esph    = (struct esphdr *) (iph + 1);
    uint8_t *encaped_iph   = (uint8_t *) esph + sizeof(*esph);

    unsigned char *payload_out = (unsigned char*) ((uint8_t*)ethh + sizeof(struct ether_hdr)
                               + sizeof(struct iphdr));
    int payload_len = (ntohs(iph->tot_len) - (iph->ihl * 4) - SHA_DIGEST_LENGTH);
    uint8_t isum[SHA_DIGEST_LENGTH];
    uint8_t hmac_buf[2048];
    struct hmac_sa_entry *sa_entry;

    uint8_t *hmac_key;
    if (likely(anno_isset(&pkt->anno, NBA_ANNO_IPSEC_FLOW_ID))) {
        sa_entry = &flows[anno_get(&pkt->anno, NBA_ANNO_IPSEC_FLOW_ID)];
        hmac_key = sa_entry->hmac_key;

        rte_memcpy(hmac_buf + 64, payload_out, payload_len);
        for (int i = 0; i < 8; i++)
            *((uint64_t*)hmac_buf + i) = 0x3636363636363636LLU ^ *((uint64_t*)hmac_key + i);
        SHA1(hmac_buf, 64 + payload_len, isum);

        rte_memcpy(hmac_buf + 64, isum, SHA_DIGEST_LENGTH);
        for (int i = 0; i < 8; i++) {
            *((uint64_t*)hmac_buf + i) = 0x5c5c5c5c5c5c5c5cLLU ^ *((uint64_t*)hmac_key + i);
        }
        SHA1(hmac_buf, 64 + SHA_DIGEST_LENGTH, payload_out + payload_len);
        // TODO: correctness check..
    } else {
        pkt->kill();
        return 0;
    }
    output(0).push(pkt);
    return 0;
}

void IPsecAuthHMACSHA1::accel_init_handler(ComputeDevice *device)
{
    // Put key array content to device space.
    size_t flows_size = sizeof(struct hmac_sa_entry) * num_tunnels;
    flows = (struct hmac_sa_entry *) ctx->node_local_storage->get_alloc("h_hmac_flows");
    flows_d  = (dev_mem_t *) ctx->node_local_storage->get_alloc("d_hmac_flows_ptr");
    host_mem_t flows_h;
    flows_h  = device->alloc_host_buffer(flows_size, 0);
    *flows_d = device->alloc_device_buffer(flows_size, 0, flows_h);
    memcpy(device->unwrap_host_buffer(flows_h), flows, flows_size);
    device->memwrite(flows_h, *flows_d, 0, flows_size);
}

void IPsecAuthHMACSHA1::accel_compute_handler(ComputeDevice *cdev,
                                              ComputeContext *cctx,
                                              struct resource_param *res)
{
    struct kernel_arg arg;
    void *ptr_args[1];
    ptr_args[0] = cdev->unwrap_device_buffer(*flows_d);
    arg = {&ptr_args[0], sizeof(void *), alignof(void *)};
    cctx->push_kernel_arg(arg);

    dev_kernel_t kern;
#ifdef USE_CUDA
    kern.ptr = ipsec_hsha1_encryption_get_cuda_kernel();
#endif
#ifdef USE_KNAPP
    kern.ptr = (void *) (uintptr_t) knapp::ID_KERNEL_IPSEC_HMACSHA1;
#endif
    cctx->enqueue_kernel_launch(kern, res);
}

size_t IPsecAuthHMACSHA1::get_desired_workgroup_size(const char *device_name) const
{
    #ifdef USE_CUDA
    if (!strcmp(device_name, "cuda"))
        return 64u;
    #endif
    #ifdef USE_PHI
    if (!strcmp(device_name, "phi"))
        return 32u;
    #endif
    return 32u;
}

int IPsecAuthHMACSHA1::postproc(int input_port, void *custom_output, Packet *pkt)
{
    output(0).push(pkt);
    return 0;
}

// vim: ts=8 sts=4 sw=4 et
