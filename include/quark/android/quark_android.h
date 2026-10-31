/* Implements 019-Platform-Abstraction-Layer §"Android target" (Draft amendment) and 021 §"Mobile member
 * profile"; ADR-071. The C ABI of libquark_android.so: what the application's JNI glue links against.
 * Deliberately C, deliberately tiny, no JNI types: the Kotlin/JNI side (AeroCoWorker) wraps it. Quark never
 * calls up into Java: the app calls suspend/resume/network_changed (lifecycle contract) and owns the
 * foreground service, WifiManager.MulticastLock, NsdManager and Doze handling.
 *
 * Threading: every call is safe from any thread (marshalled onto the node's I/O loop). Return 0 = ok.
 * Security: `allow_insecure_dev` != 0 lets the node start with the plaintext dev transport (no mTLS, no
 * SecureTransport). It exists for the repo's own tests and bring-up; a shipping build passes 0 and wraps the
 * node in the security stack owned by ADR-044 (not part of this ADR).
 */
#ifndef QUARK_ANDROID_H
#define QUARK_ANDROID_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define QUARK_ANDROID_API __attribute__((visibility("default")))
#else
#define QUARK_ANDROID_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QuarkAndroidNode QuarkAndroidNode;

/* Why the app suspends (mirrors quark::SuspendReason). */
enum { QUARK_SUSPEND_APP_BACKGROUND = 1, QUARK_SUSPEND_DOZE = 2, QUARK_SUSPEND_NETWORK_LOST = 3, QUARK_SUSPEND_OTHER = 4 };

/* Peer states reported by quark_android_node_peer_state. */
enum {
    QUARK_PEER_UNKNOWN = 0,
    QUARK_PEER_ALIVE = 1,
    QUARK_PEER_SUSPECT = 2,
    QUARK_PEER_DEAD = 3,
    QUARK_PEER_LEFT = 4,
    QUARK_PEER_SUSPENDED = 5 /* Alive, and a suspension lease is held for it */
};

/* bind_ipv4 is host byte order (0 = any). listen_port 0 = ephemeral. tick_ms = SWIM protocol period. */
QUARK_ANDROID_API QuarkAndroidNode* quark_android_node_create(uint64_t node_id, uint64_t cluster_id,
                                                              uint32_t bind_ipv4, uint16_t listen_port,
                                                              uint32_t tick_ms, int allow_insecure_dev);
QUARK_ANDROID_API int quark_android_node_add_peer(QuarkAndroidNode*, uint64_t peer_id, uint32_t peer_ipv4,
                                                  uint16_t peer_port);
QUARK_ANDROID_API int quark_android_node_start(QuarkAndroidNode*);
QUARK_ANDROID_API uint16_t quark_android_node_port(QuarkAndroidNode*);
QUARK_ANDROID_API int quark_android_node_join(QuarkAndroidNode*, uint64_t seed_id);
/* Fire one small test frame (trace_id = seq) at a peer over the data plane. */
QUARK_ANDROID_API int quark_android_node_send_probe(QuarkAndroidNode*, uint64_t peer_id, uint64_t seq);

/* Mobile lifecycle (ADR-071). lease_ms 0 = the profile default. */
QUARK_ANDROID_API void quark_android_node_suspend(QuarkAndroidNode*, int reason, uint32_t lease_ms);
QUARK_ANDROID_API void quark_android_node_resume(QuarkAndroidNode*);
QUARK_ANDROID_API void quark_android_node_network_changed(QuarkAndroidNode*);

/* Observability. */
QUARK_ANDROID_API uint64_t quark_android_node_frames_received(QuarkAndroidNode*);
QUARK_ANDROID_API uint64_t quark_android_node_view_size(QuarkAndroidNode*);
QUARK_ANDROID_API int quark_android_node_peer_state(QuarkAndroidNode*, uint64_t peer_id);
QUARK_ANDROID_API int quark_android_node_self_suspended(QuarkAndroidNode*);

QUARK_ANDROID_API void quark_android_node_destroy(QuarkAndroidNode*);

#ifdef __cplusplus
}
#endif
#endif /* QUARK_ANDROID_H */
